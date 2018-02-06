#pragma region License
/*
 * This file is part of the Boomerang Decompiler.
 *
 * See the file "LICENSE.TERMS" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL
 * WARRANTIES.
 */
#pragma endregion License
#include "Module.h"


#include "boomerang/core/Boomerang.h"
#include "boomerang/db/proc/LibProc.h"
#include "boomerang/db/proc/UserProc.h"
#include "boomerang/db/Prog.h"
#include "boomerang/db/Signature.h"
#include "boomerang/db/statements/CallStatement.h"
#include "boomerang/db/exp/Location.h"
#include "boomerang/util/Log.h"

#include <QDir>
#include <QString>


#if defined(_WIN32) && !defined(__MINGW32__)
#  include <windows.h>
namespace dbghelp
{
#  include <dbghelp.h>
}

#  include <iostream>
#  include "boomerang/util/Log.h"
#endif


#if defined(_WIN32) && !defined(__MINGW32__)
// From prog.cpp
BOOL CALLBACK addSymbol(dbghelp::PSYMBOL_INFO symInfo, ULONG SymbolSize, PVOID UserContext);
SharedType typeFromDebugInfo(int index, DWORD64 ModBase);

#endif


void Module::updateLibrarySignatures()
{
    m_currentFrontend->readLibraryCatalog();

    for (Function *func : m_functionList) {
        if (func->isLib()) {
            func->setSignature(getLibSignature(func->getName()));

            for (CallStatement *call_stmt : func->getCallers()) {
                call_stmt->setSigArguments();
            }

            Boomerang::get()->alertUpdateSignature(func);
        }
    }
}


Module::Module()
{
    m_strm.setDevice(&m_out);
}


Module::Module(const QString& name, Prog *prog, IFrontEnd *fe)
    : m_currentFrontend(fe)
    , m_name(name)
    , m_prog(prog)
{
    m_strm.setDevice(&m_out);
}


Module::~Module()
{
    for (Function *proc : m_functionList) {
        delete proc;
    }
}


size_t Module::getNumChildren() const
{
    return m_children.size();
}


Module *Module::getChild(size_t n)
{
    assert(n < getNumChildren());
    return m_children[n];
}


void Module::addChild(Module *module)
{
    if (module->m_parent) {
        module->m_parent->removeChild(module);
    }

    m_children.push_back(module);
    module->m_parent = this;
}


void Module::removeChild(Module *module)
{
    for (auto it = m_children.begin(); it != m_children.end(); it++) {
        if (*it == module) {
            m_children.erase(it);
        }
    }
}


Module *Module::getUpstream() const
{
    return m_parent;
}


bool Module::hasChildren() const
{
    return !m_children.empty();
}


void Module::openStream(const char *ext)
{
    if (m_out.isOpen()) {
        return;
    }

    m_out.setFileName(getOutPath(ext));
    m_out.open(QFile::WriteOnly | QFile::Text);
    m_stream_ext = ext;
}


void Module::openStreams(const char *ext)
{
    openStream(ext);

    for (Module *child : m_children) {
        child->openStreams(ext);
    }
}


void Module::closeStreams()
{
    if (m_out.isOpen()) {
        m_strm.flush();
        m_out.close();
    }

    for (Module *child : m_children) {
        child->closeStreams();
    }
}


QString Module::makeDirs() const
{
    QString path;

    if (m_parent) {
        path = m_parent->makeDirs();
    }
    else {
        path = Boomerang::get()->getSettings()->getOutputDirectory().absolutePath();
    }

    QDir dr(path);

    if (getNumChildren() > 0 || m_parent == nullptr) {
        dr.mkpath(m_name);
        dr.cd(m_name);
    }

    return dr.absolutePath();
}


QString Module::getOutPath(const char *ext) const
{
    QString basedir = makeDirs();
    QDir    dr(basedir);

    return dr.absoluteFilePath(m_name + "." + ext);
}


Module *Module::find(const QString& nam)
{
    if (m_name == nam) {
        return this;
    }

    for (Module *child : m_children) {
        Module *c = child->find(nam);

        if (c) {
            return c;
        }
    }

    return nullptr;
}


void Module::printTree(QTextStream& ostr) const
{
    ostr << "\t\t" << m_name << "\n";

    for (Module *elem : m_children) {
        elem->printTree(ostr);
    }
}


void Module::setLocationMap(Address loc, Function *fnc)
{
    if (fnc == nullptr) {
        size_t count = m_labelsToProcs.erase(loc);
        Q_UNUSED(count);
        assert(count == 1);
    }
    else {
        m_labelsToProcs[loc] = fnc;
    }
}


void Module::addWin32DbgInfo(Function *function)
{
#if !defined(_WIN32) || defined(__MINGW32__)
    Q_UNUSED(function);
    LOG_VERBOSE("Adding debug information for Windows programs is only supported on Windows!");
    return;
#else
    if (!function) {
        return;
    }
    else if (!m_currentFrontend || !m_currentFrontend->isWin32()) {
        LOG_WARN("Cannot add debugging information for function '%1'", function->getName());
        return;
    }

    // use debugging information
    HANDLE               hProcess = GetCurrentProcess();
    dbghelp::SYMBOL_INFO *sym = (dbghelp::SYMBOL_INFO *)malloc(sizeof(dbghelp::SYMBOL_INFO) + 1000);
    sym->SizeOfStruct = sizeof(*sym);
    sym->MaxNameLen = 1000;
    sym->Name[0] = 0;
    BOOL  got = dbghelp::SymFromAddr(hProcess, function->getEntryAddress().value(), 0, sym);
    DWORD retType;

    if (got && *sym->Name &&
        dbghelp::SymGetTypeInfo(hProcess, sym->ModBase, sym->TypeIndex, dbghelp::TI_GET_TYPE, &retType)) {
        DWORD d;
        // get a calling convention
        got =
            dbghelp::SymGetTypeInfo(hProcess, sym->ModBase, sym->TypeIndex, dbghelp::TI_GET_CALLING_CONVENTION, &d);

        if (got) {
            LOG_VERBOSE("calling convention: %1", (int)d);
            // TODO: use it
        }
        else {
            // assume we're stdc calling convention, remove r28, r24 returns
            function->setSignature(Signature::instantiate(Platform::PENTIUM, CallConv::C, function->getName()));
        }

        // get a return type
        SharedType rtype = typeFromDebugInfo(retType, sym->ModBase);

        if (!rtype->isVoid()) {
            function->getSignature()->addReturn(rtype, Location::regOf(24));
        }

        // find params and locals
        dbghelp::IMAGEHLP_STACK_FRAME stack;
        stack.InstructionOffset = function->getEntryAddress().value();
        dbghelp::SymSetContext(hProcess, &stack, 0);
        dbghelp::SymEnumSymbols(hProcess, 0, nullptr, addSymbol, function);

        LOG_VERBOSE("Retrieved Win32 debugging information:");
        function->getSignature()->printToLog();
    }
#endif
}


Function *Module::createFunction(const QString& name, Address entryAddr, bool libraryFunction)
{
    Function *function;

    if (libraryFunction) {
        function = new LibProc(entryAddr, name, this);
    }
    else {
        function = new UserProc(entryAddr, name, this);
    }

    if (Address::INVALID != entryAddr) {
        assert(m_labelsToProcs.find(entryAddr) == m_labelsToProcs.end());
        m_labelsToProcs[entryAddr] = function;
    }

    m_functionList.push_back(function); // Append this to list of procs
    // alert the watchers of a new proc
    emit newFunction(function);
    Boomerang::get()->alertNew(function);

    // TODO: add platform agnostic way of using debug information, should be moved to Loaders, Prog should just collect info
    // from Loader
    addWin32DbgInfo(function);
    return function;
}


Function *Module::getFunction(const QString& name) const
{
    for (Function *f : m_functionList) {
        if (f->getName() == name) {
            return f;
        }
    }

    return nullptr;
}


Function *Module::getFunction(Address loc) const
{
    auto iter = m_labelsToProcs.find(loc);

    return (iter != m_labelsToProcs.end()) ? iter->second : nullptr;
}


std::shared_ptr<Signature> Module::getLibSignature(const QString& name)
{
    return m_currentFrontend->getLibSignature(name);
}