#pragma region License
/*
 * This file is part of the Boomerang Decompiler.
 *
 * See the file "LICENSE.TERMS" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL
 * WARRANTIES.
 */
#pragma endregion License
#include "st20frontend.h"


#include "boomerang/util/Log.h"

#include "boomerang/db/Register.h"
#include "boomerang/db/RTL.h"
#include "boomerang/db/CFG.h"
#include "boomerang/db/proc/UserProc.h"
#include "boomerang/db/Prog.h"
#include "boomerang/db/Signature.h"
#include "boomerang/db/exp/Location.h"

#include "boomerang/loader/IFileLoader.h"
#include "boomerang/frontend/st20/st20decoder.h"

#include <cassert>
#include <iomanip> // For setfill etc
#include <sstream>


ST20FrontEnd::ST20FrontEnd(IFileLoader *loader, Prog *prog)
    : IFrontEnd(loader, prog)
{
    m_decoder.reset(new ST20Decoder(prog));
}


std::vector<SharedExp>& ST20FrontEnd::getDefaultParams()
{
    static std::vector<SharedExp> params;

    if (params.size() == 0) {
        params.push_back(Location::memOf(Location::regOf(3)));
    }

    return params;
}


std::vector<SharedExp>& ST20FrontEnd::getDefaultReturns()
{
    static std::vector<SharedExp> returns;

    if (returns.size() == 0) {
        returns.push_back(Location::regOf(0));
        returns.push_back(Location::regOf(3));
        //        returns.push_back(Terminal::get(opPC));
    }

    return returns;
}


Address ST20FrontEnd::getMainEntryPoint(bool& gotMain)
{
    gotMain = true;
    Address start = m_fileLoader->getMainEntryPoint();

    if (start != Address::INVALID) {
        return start;
    }

    start   = m_fileLoader->getEntryPoint();
    gotMain = false;

    if (start == Address::INVALID) {
        return Address::INVALID;
    }

    gotMain = true;
    return start;
}


bool ST20FrontEnd::processProc(Address entryAddr, UserProc *proc, QTextStream& os, bool frag /* = false */,
                               bool spec /* = false */)
{
    // Call the base class to do most of the work
    if (!IFrontEnd::processProc(entryAddr, proc, os, frag, spec)) {
        return false;
    }

    // This will get done twice; no harm
    proc->setEntryBB();

    return true;
}