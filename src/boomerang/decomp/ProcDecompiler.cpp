#pragma region License
/*
 * This file is part of the Boomerang Decompiler.
 *
 * See the file "LICENSE.TERMS" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL
 * WARRANTIES.
 */
#pragma endregion License
#include "ProcDecompiler.h"

#include "boomerang/core/Project.h"
#include "boomerang/core/Settings.h"
#include "boomerang/db/BasicBlock.h"
#include "boomerang/db/Prog.h"
#include "boomerang/decomp/IndirectJumpAnalyzer.h"
#include "boomerang/ifc/IFrontEnd.h"
#include "boomerang/passes/PassManager.h"
#include "boomerang/ssl/RTL.h"
#include "boomerang/ssl/statements/CallStatement.h"
#include "boomerang/ssl/statements/ReturnStatement.h"
#include "boomerang/util/log/Log.h"
#include "boomerang/util/log/SeparateLogger.h"


ProcDecompiler::ProcDecompiler()
{
}


void ProcDecompiler::decompileRecursive(UserProc *proc)
{
    tryDecompileRecursive(proc);
}


ProcStatus ProcDecompiler::tryDecompileRecursive(UserProc *proc)
{
    /* Cycle detection logic:
     * *********************
     * cycleGrp is an initially null pointer to a set of procedures, representing the procedures
     * involved in the current recursion group, if any. These procedures have to be analysed
     * together as a group, after individual pre-group analysis. child is a set of procedures,
     * cleared at the top of decompile(), representing the cycles associated with the current
     * procedure and all of its children. If this is empty, the current procedure is not involved in
     * recursion, and can be decompiled up to and including removing unused statements. callStack is
     * an initially empty list of procedures, representing the call stack from the current entry
     * point to the current procedure, inclusive. If (after all children have been processed:
     * important!) the first element in callStack and also cycleGrp is the current procedure, we
     * have the maximal set of distinct cycles, so we can do the recursion group analysis and return
     * an empty set. At the end of the recursion group analysis, the whole group is complete, ready
     * for the global analyses.
     *
     *   cycleSet decompile(ProcList callStack)        // call stack initially empty
     *     child = new ProcSet
     *     push this proc to the call stack
     *     for each child c called by this proc
     *       if c has already been visited but not finished
     *         // have new cycle
     *         if c is in callStack
     *           // this is a completely new cycle
     *           insert every proc from c to the end of callStack into child
     *         else
     *           // this is a new branch of an existing cycle
     *           child = c->cycleGrp
     *           find first element f of callStack that is in cycleGrp
     *           insert every proc after f to the end of callStack into child
     *           for each element e of child
     *         insert e->cycleGrp into child
     *         e->cycleGrp = child
     *       else
     *         // no new cycle
     *         tmp = c->decompile(callStack)
     *         child = union(child, tmp)
     *         set return statement in call to that of c
     *
     *     if (child empty)
     *       earlyDecompile()
     *       child = middleDecompile()
     *       removeUnusedStatments()            // Not involved in recursion
     *     else
     *       // Is involved in recursion
     *       find first element f in callStack that is also in cycleGrp
     *       if (f == this) // The big test: have we got the complete strongly connected component?
     *         recursionGroupAnalysis() // Yes, we have
     *         child = new ProcSet      // Don't add these processed cycles to the parent
     *     remove last element (= this) from callStack
     *     return child
     */

    Project *project = proc->getProg()->getProject();

    LOG_MSG("%1 procedure '%2'", (proc->getStatus() >= PROC_VISITED) ? "Re-visiting" : "Visiting",
            proc->getName());
    project->alertDiscovered(proc);

    // Prevent infinite loops when there are cycles in the call graph (should never happen now)
    if (proc->getStatus() >= PROC_FINAL) {
        LOG_WARN("Proc %1 already has status PROC_FINAL", proc->getName());
        return PROC_FINAL; // Already decompiled
    }

    if (proc->getStatus() < PROC_DECODED) {
        // Can happen e.g. if a callee is visible only after analysing a switch statement
        // Actually decoding for the first time, not REdecoding
        if (!proc->getProg()->reDecode(proc)) {
            return PROC_UNDECODED;
        }
    }

    if (proc->getStatus() < PROC_VISITED) {
        proc->setStatus(PROC_VISITED); // We have at least visited this proc "on the way down"
    }

    m_callStack.push_back(proc);

    if (project->getSettings()->verboseOutput) {
        printCallStack();
    }

    if (project->getSettings()->decodeChildren) {
        // Recurse to callees first, to perform a depth first search
        for (BasicBlock *bb : *proc->getCFG()) {
            if (bb->getType() != BBType::Call) {
                continue;
            }

            // The call Statement will be in the last RTL in this BB
            CallStatement *call = static_cast<CallStatement *>(bb->getRTLs()->back()->getHlStmt());

            if (!call->isCall()) {
                LOG_WARN("BB at address %1 is a CALL but last stmt is not a call: %2",
                         bb->getLowAddr(), call);
                continue;
            }

            assert(call->isCall());
            UserProc *callee = dynamic_cast<UserProc *>(call->getDestProc());

            if (callee == nullptr) { // not an user proc, or missing dest
                continue;
            }

            if (callee->getStatus() == PROC_FINAL) {
                // Already decompiled, but the return statement still needs to be set for this call
                call->setCalleeReturn(callee->getRetStmt());
                continue;
            }

            // check if the callee has already been visited but not done (apart from global
            // analyses). This means that we have found a new cycle or a part of an existing cycle
            if ((callee->getStatus() >= PROC_VISITED) && (callee->getStatus() <= PROC_EARLYDONE)) {
                // if callee is in callStack
                ProcList::iterator calleeIt = std::find(m_callStack.begin(), m_callStack.end(),
                                                        callee);

                if (calleeIt != m_callStack.end()) {
                    // This is a completely new cycle
                    std::shared_ptr<ProcSet> newRecursionGroup(new ProcSet());
                    newRecursionGroup->insert(calleeIt, m_callStack.end());
                    createRecursionGoup(newRecursionGroup);
                }
                else if (callee->getRecursionGroup()) {
                    // This is a new branch of an existing cycle that was visited previously
                    std::shared_ptr<ProcSet> recursionGroup = callee->getRecursionGroup();

                    // Find first element func of callStack that is in callee->recursionGroup
                    ProcList::iterator _pi = std::find_if(
                        m_callStack.begin(), m_callStack.end(), [callee](UserProc *func) {
                            return callee->getRecursionGroup()->find(func) !=
                                   callee->getRecursionGroup()->end();
                        });

                    // Insert every proc after func to the end of path into child
                    assert(_pi != m_callStack.end());
                    for (auto it = std::next(_pi); it != m_callStack.end(); ++it) {
                        addToRecursionGroup(*it, recursionGroup);
                    }
                }

                proc->setStatus(PROC_INCYCLE);
            }
            else {
                // No new cycle
                LOG_VERBOSE("Preparing to decompile callee '%1' of '%2'", callee->getName(),
                            proc->getName());

                callee->promoteSignature();
                tryDecompileRecursive(callee);
                // Child has at least done middleDecompile(), possibly more
                call->setCalleeReturn(callee->getRetStmt());

                if (proc->getStatus() != PROC_INCYCLE &&
                    m_recursionGroups.find(proc) != m_recursionGroups.end()) {
                    proc->setStatus(PROC_INCYCLE);
                    proc->setRecursionGroup(m_recursionGroups.find(proc)->second);
                }
            }
        }
    }

    // if no child involved in recursion
    if (proc->getStatus() != PROC_INCYCLE) {
        project->alertDecompiling(proc);
        LOG_MSG("Decompiling procedure '%1'", proc->getName());

        earlyDecompile(proc);
        middleDecompile(proc);

        if (project->getSettings()->verboseOutput) {
            printCallStack();
        }
    }

    if (proc->getStatus() != PROC_INCYCLE) {
        lateDecompile(proc); // Do the whole works
        proc->setStatus(PROC_FINAL);
        project->alertEndDecompile(proc);
    }
    else if (m_recursionGroups.find(proc) != m_recursionGroups.end()) {
        // This proc's callees, and hence this proc, is/are involved in recursion.
        // Find first element f in path that is also in our recursion group
        ProcList::iterator f = std::find_if(
            m_callStack.begin(), m_callStack.end(), [proc](UserProc *func) {
                return proc->getRecursionGroup()->find(func) != proc->getRecursionGroup()->end();
            });

        // The big test: have we found the whole strongly connected component (in the call graph)?
        if (*f == proc) {
            // Yes, process these procs as a group
            recursionGroupAnalysis(proc->getRecursionGroup());
            proc->setStatus(PROC_FINAL);
            project->alertEndDecompile(proc);
        }
    }

    // Remove last element (= this) from path
    assert(!m_callStack.empty());
    assert(m_callStack.back() == proc);
    m_callStack.pop_back();

    LOG_MSG("Finished decompile of '%1'", proc->getName());

    if (project->getSettings()->verboseOutput) {
        printCallStack();
    }

    return proc->getStatus();
}


void ProcDecompiler::createRecursionGoup(const std::shared_ptr<ProcSet> &newGroup)
{
    LOG_VERBOSE("Creating recursion group:");
    for (UserProc *proc : *newGroup) {
        LOG_VERBOSE("    %1", proc->getName());
    }

    // find all exisiting groups and union them with the new one
    std::shared_ptr<ProcSet> unionGroup = newGroup;

    for (UserProc *proc : *newGroup) {
        auto it = m_recursionGroups.find(proc);

        if (it != m_recursionGroups.end()) {
            // proc is already part of a recursion group
            if (unionGroup == newGroup) {
                unionGroup.reset(new ProcSet);
            }

            for (UserProc *existingProc : *newGroup) {
                unionGroup->insert(existingProc);

                if (existingProc->getRecursionGroup()) {
                    unionGroup->insert(existingProc->getRecursionGroup()->begin(),
                                       existingProc->getRecursionGroup()->end());
                }
            }
        }
    }

    for (UserProc *proc : *unionGroup) {
        m_recursionGroups[proc] = unionGroup;
        proc->setRecursionGroup(unionGroup);
        proc->setStatus(PROC_INCYCLE);
    }
}


void ProcDecompiler::addToRecursionGroup(UserProc *proc,
                                         const std::shared_ptr<ProcSet> &recursionGroup)
{
    LOG_VERBOSE("Adding %1 to recursion group:", proc->getName());
    for (UserProc *_proc : *recursionGroup) {
        LOG_VERBOSE("    %1", _proc->getName());
    }

    // find all exisiting groups and union them with the new one
    std::shared_ptr<ProcSet> unionGroup = recursionGroup;

    auto it = m_recursionGroups.find(proc);

    if (it != m_recursionGroups.end()) {
        // proc is already part of a recursion group
        if (unionGroup == recursionGroup) {
            unionGroup.reset(new ProcSet);
        }

        for (UserProc *existingProc : *recursionGroup) {
            unionGroup->insert(existingProc);

            if (existingProc->getRecursionGroup()) {
                unionGroup->insert(existingProc->getRecursionGroup()->begin(),
                                   existingProc->getRecursionGroup()->end());
            }
        }
    }

    unionGroup->insert(proc);

    for (UserProc *_proc : *unionGroup) {
        m_recursionGroups[_proc] = unionGroup;
        _proc->setRecursionGroup(unionGroup);
        _proc->setStatus(PROC_INCYCLE);
    }
}


void ProcDecompiler::earlyDecompile(UserProc *proc)
{
    Project *project = proc->getProg()->getProject();

    project->alertStartDecompile(proc);
    project->alertDecompileDebugPoint(proc, "Before Initialise");

    PassManager::get()->executePass(PassID::StatementInit, proc);
    PassManager::get()->executePass(PassID::BBSimplify, proc); // Remove branches with false guards
    PassManager::get()->executePass(PassID::Dominators, proc);

    proc->debugPrintAll("After Decoding");
    project->alertDecompileDebugPoint(proc, "After Initialise");

    if (proc->getStatus() >= PROC_EARLYDONE) {
        return;
    }

    project->alertDecompileDebugPoint(proc, "Before Early");
    LOG_VERBOSE("### Beginning early decompile for '%1' ###", proc->getName());

    // Update the defines in the calls. Will redo if involved in recursion
    PassManager::get()->executePass(PassID::CallDefineUpdate, proc);
    PassManager::get()->executePass(PassID::GlobalConstReplace, proc);

    // First placement of phi functions, renaming, and initial propagation. This is mostly for the
    // stack pointer
    // TODO: Check if this makes sense. It seems to me that we only want to do one pass of
    // propagation here, since the status == check had been knobbled below. Hopefully, one call to
    // placing phi functions etc will be equivalent to depth 0 in the old scheme
    PassManager::get()->executePass(PassID::PhiPlacement, proc);


    // Rename variables
    PassManager::get()->executePass(PassID::BlockVarRename, proc);
    PassManager::get()->executePass(PassID::StatementPropagation, proc);

    project->alertDecompileDebugPoint(proc, "After Early");
}


void ProcDecompiler::middleDecompile(UserProc *proc)
{
    assert(m_callStack.back() == proc);
    Project *project = proc->getProg()->getProject();

    project->alertDecompileDebugPoint(proc, "Before Middle");
    LOG_VERBOSE("### Beginning middleDecompile for '%1' ###", proc->getName());

    // The call bypass logic should be staged as well. For example, consider m[r1{11}]{11} where 11
    // is a call. The first stage bypass yields m[r1{2}]{11}, which needs another round of
    // propagation to yield m[r1{-}-32]{11} (which can safely be processed at depth 1). Except that
    // this is now inherent in the visitor nature of the latest algorithm.
    PassManager::get()->executePass(PassID::CallAndPhiFix,
                                    proc); // Bypass children that are finalised (if any)
    proc->debugPrintAll("After call and phi bypass (1)");

    if (proc->getStatus() != PROC_INCYCLE) { // FIXME: need this test?
        PassManager::get()->executePass(PassID::StatementPropagation, proc);
    }

    // This part used to be calle middleDecompile():

    PassManager::get()->executePass(PassID::SPPreservation, proc);
    // Oops - the idea of splitting the sp from the rest of the preservations was to allow correct
    // naming of locals so you are alias conservative. But of course some locals are ebp (etc)
    // based, and so these will never be correct until all the registers have preservation analysis
    // done. So I may as well do them all together here.
    PassManager::get()->executePass(PassID::PreservationAnalysis, proc);
    PassManager::get()->executePass(PassID::CallAndPhiFix, proc); // Propagate and bypass sp

    proc->debugPrintAll("After preservation, bypass and propagation");

    // Oh, no, we keep doing preservations till almost the end...
    // setStatus(PROC_PRESERVEDS);        // Preservation done

    if (project->getSettings()->usePromotion) {
        // We want functions other than main to be promoted. Needed before mapExpressionsToLocals
        proc->promoteSignature();
    }

    // The problem with doing locals too early is that the symbol map ends up with some {-} and some
    // {0} Also, once named as a local, it is tempting to propagate the memory location, but that
    // might be unsafe if the address is taken. But see mapLocalsAndParams just a page below.
    // mapExpressionsToLocals();

    // Update the arguments for calls (mainly for the non recursion affected calls)
    // We have only done limited propagation and collecting to this point. Need e.g. to put m[esp-K]
    // into the collectors of calls, so when a stack parameter is created, it will be correctly
    // localised Note that we'd like to limit propagation before this point, because we have not yet
    // created any arguments, so it is possible to get "excessive propagation" to parameters. In
    // fact, because uses vary so much throughout a program, it may end up better not limiting
    // propagation until very late in the decompilation, and undoing some propagation just before
    // removing unused statements. Or even later, if that is possible. For now, we create the
    // initial arguments here (relatively early), and live with the fact that some apparently
    // distinct memof argument expressions (e.g. m[eax{30}] and m[esp{40}-4]) will turn out to be
    // duplicates, and so the duplicates must be eliminated.
    bool change = PassManager::get()->executePass(PassID::PhiPlacement, proc);

    PassManager::get()->executePass(PassID::BlockVarRename, proc);

    // Otherwise sometimes sp is not fully propagated
    PassManager::get()->executePass(PassID::StatementPropagation, proc);
    PassManager::get()->executePass(PassID::CallArgumentUpdate, proc);
    PassManager::get()->executePass(PassID::StrengthReductionReversal, proc);

    // Repeat until no change
    int pass = 3;

    do {
        // Redo the renaming process to take into account the arguments
        change = PassManager::get()->executePass(PassID::PhiPlacement, proc);
        change |= PassManager::get()->executePass(PassID::BlockVarRename,
                                                  proc); // E.g. for new arguments

        // Seed the return statement with reaching definitions
        // FIXME: does this have to be in this loop?
        if (proc->getRetStmt()) {
            proc->getRetStmt()
                ->updateModifieds(); // Everything including new arguments reaching the exit
            proc->getRetStmt()->updateReturns();
        }

        // Print if requested
        if (project->getSettings()->verboseOutput) { // was if debugPrintSSA
            QDir outputDir   = project->getSettings()->getOutputDirectory();
            QString filePath = outputDir.absoluteFilePath(proc->getName());

            LOG_SEPARATE(filePath, "--- Debug print SSA for %1 pass %2 (no propagations) ---",
                         proc->getName(), pass);
            LOG_SEPARATE(filePath, "%1", proc->toString());
            LOG_SEPARATE(filePath, "=== End debug print SSA for %1 pass %2 (no propagations) ===",
                         proc->getName(), pass);
        }

        // (* Was: mapping expressions to Parameters as we go *)

        // FIXME: Check if this is needed any more. At least fib seems to need it at present.
        if (project->getSettings()->changeSignatures) {
            // addNewReturns(depth);
            for (int i = 0; i < 3; i++) { // FIXME: should be iterate until no change
                LOG_VERBOSE("### update returns loop iteration %1 ###", i);

                if (proc->getStatus() != PROC_INCYCLE) {
                    PassManager::get()->executePass(PassID::BlockVarRename, proc);
                }

                PassManager::get()->executePass(PassID::PreservationAnalysis, proc);
                PassManager::get()->executePass(
                    PassID::CallDefineUpdate,
                    proc); // Returns have uses which affect call defines (if childless)
                PassManager::get()->executePass(PassID::CallAndPhiFix, proc);
                PassManager::get()->executePass(PassID::PreservationAnalysis,
                                                proc); // Preserveds subtract from returns
            }

            if (project->getSettings()->verboseOutput) {
                proc->debugPrintAll("SSA (after updating returns");
            }
        }

        // Print if requested
        if (project->getSettings()->verboseOutput) { // was if debugPrintSSA
            proc->debugPrintAll("SSA (after trimming return set)");
        }

        project->alertDecompileDebugPoint(proc, "Before propagating statements");

        change |= PassManager::get()->executePass(PassID::StatementPropagation, proc);
        change |= PassManager::get()->executePass(PassID::BlockVarRename, proc);

        project->alertDecompileDebugPoint(proc, "after propagating statements");

        // this is just to make it readable, do NOT rely on these statements being removed
        PassManager::get()->executePass(PassID::AssignRemoval, proc);
    } while (change && ++pass < 12);

    // At this point, there will be some memofs that have still not been renamed. They have been
    // prevented from getting renamed so that they didn't get renamed incorrectly (usually as {-}),
    // when propagation and/or bypassing may have ended up changing the address expression. There is
    // now no chance that this will happen, so we need to rename the existing memofs. Note that this
    // can still link uses to definitions, e.g. 50 r26 := phi(...) 51 m[r26{50}] := 99;
    //    ... := m[r26{50}]{should be 51}

    LOG_VERBOSE("### allowing SSA renaming of all memof expressions ###");

    proc->getDataFlow()->setRenameLocalsParams(true);

    // Now we need another pass to inert phis for the memofs, rename them and propagate them
    PassManager::get()->executePass(PassID::PhiPlacement, proc);
    PassManager::get()->executePass(PassID::BlockVarRename, proc);

    proc->debugPrintAll("after setting phis for memofs, renaming them");
    PassManager::get()->executePass(PassID::StatementPropagation, proc);

    // Now that memofs are renamed, the bypassing for memofs can work
    PassManager::get()->executePass(PassID::CallAndPhiFix,
                                    proc); // Bypass children that are finalised (if any)

    if (project->getSettings()->nameParameters) {
        // ? Crazy time to do this... haven't even done "final" parameters as yet
        // mapExpressionsToParameters();
    }

    // Check for indirect jumps or calls not already removed by propagation of constants
    bool changed = false;
    IndirectJumpAnalyzer analyzer;

    for (BasicBlock *bb : *proc->getCFG()) {
        changed |= analyzer.decodeIndirectJmp(bb, proc);
    }

    if (changed) {
        // There was at least one indirect jump or call found and decoded. That means that most of
        // what has been done to this function so far is invalid. So redo everything. Very
        // expensive!! Code pointed to by the switch table entries has merely had
        // FrontEnd::processFragment() called on it
        LOG_MSG(
            "Restarting decompilation of '%1' because indirect jumps or calls have been analyzed",
            proc->getName());

        project->alertDecompileDebugPoint(
            proc,
            "Before restarting decompilation because indirect jumps or calls have been analyzed");

        // First copy any new indirect jumps or calls that were decoded this time around. Just copy
        // them all, the map will prevent duplicates
        saveDecodedICTs(proc);

        // Now, decode from scratch
        proc->removeRetStmt();
        proc->getCFG()->clear();

        if (!proc->getProg()->reDecode(proc)) {
            return;
        }

        proc->getDataFlow()->setRenameLocalsParams(false); // Start again with memofs
        proc->setStatus(PROC_VISITED);                     // Back to only visited progress

        assert(m_callStack.back() == proc);

        m_callStack.pop_back();      // Remove self from call stack
        tryDecompileRecursive(proc); // Restart decompiling this proc
        m_callStack.push_back(proc); // Restore self to call stack
        return;
    }

    PassManager::get()->executePass(PassID::PreservationAnalysis, proc);

    // Used to be later...
    if (project->getSettings()->nameParameters) {
        // findPreserveds();    // FIXME: is this necessary here?
        // fixCallBypass();     // FIXME: surely this is not necessary now?
        // trimParameters();    // FIXME: surely there aren't any parameters to trim yet?
        proc->debugPrintAll("after replacing expressions, trimming params and returns");
    }

    PassManager::get()->executePass(PassID::DuplicateArgsRemoval, proc);

    proc->setStatus(PROC_EARLYDONE);

    project->alertDecompileDebugPoint(proc, "after middle");
}


bool ProcDecompiler::decompileProcInRecursionGroup(UserProc *proc, ProcSet &visited)
{
    bool changed     = false;
    Project *project = proc->getProg()->getProject();

    visited.insert(proc);
    m_callStack.push_back(proc);

    for (Function *c : proc->getCallees()) {
        if (c->isLib()) {
            continue;
        }

        UserProc *callee = static_cast<UserProc *>(c);
        if (visited.find(callee) != visited.end()) {
            continue;
        }
        else if (proc->getRecursionGroup()->find(callee) == proc->getRecursionGroup()->end()) {
            // not in recursion group any more
            continue;
        }

        // visit unvisited callees first
        changed |= decompileProcInRecursionGroup(callee, visited);
    }

    proc->setStatus(PROC_INCYCLE); // So the calls are treated as childless
    project->alertDecompiling(proc);
    earlyDecompile(proc);

    // The standard preservation analysis should automatically perform conditional preservation.
    middleDecompile(proc);
    proc->setStatus(PROC_PRESERVEDS);

    // Mark all the relevant calls as non childless (will harmlessly get done again later)
    // FIXME: why exactly do we do this?
    proc->markAsNonChildless(proc->getRecursionGroup());

    // Need to propagate into the initial arguments, since arguments are uses,
    // and we are about to remove unused statements.
    changed |= PassManager::get()->executePass(PassID::LocalAndParamMap, proc);
    changed |= PassManager::get()->executePass(PassID::CallArgumentUpdate, proc);
    changed |= PassManager::get()->executePass(PassID::Dominators, proc);
    changed |= PassManager::get()->executePass(PassID::StatementPropagation,
                                               proc); // Need to propagate into arguments

    assert(m_callStack.back() == proc);
    m_callStack.pop_back();
    return changed;
}


void ProcDecompiler::recursionGroupAnalysis(const std::shared_ptr<ProcSet> &group)
{
    /* Overall algorithm:
     *  for each proc in the group
     *          initialise
     *          earlyDecompile
     *  for each proc in the group
     *          middleDecompile
     *  mark all calls involved in cs as non-childless
     *  for each proc in cs
     *          update parameters and returns, redoing call bypass, until no change
     *  for each proc in cs
     *          remove unused statements
     *  for each proc in cs
     *          update parameters and returns, redoing call bypass, until no change
     */
    if (group->empty()) {
        return;
    }

    LOG_MSG("Performing recursion group analysis for %1 recursive procedures: ", group->size());
    for (UserProc *proc : *group) {
        LOG_MSG("    %1", proc->getName());
    }

    UserProc *entry = *group->begin();
    bool changed    = false;
    int numRepeats  = 0;

    do {
        ProcSet visited;
        changed = decompileProcInRecursionGroup(entry, visited);
    } while (changed && numRepeats++ < 2);

    // while no change
    for (int i = 0; i < 2; i++) {
        for (UserProc *proc : *group) {
            lateDecompile(proc); // Also does final parameters and arguments at present
        }
    }

    LOG_VERBOSE("=== End recursion group analysis ===");
    for (UserProc *proc : *group) {
        proc->getProg()->getProject()->alertEndDecompile(proc);
    }
}


void ProcDecompiler::lateDecompile(UserProc *proc)
{
    Project *project = proc->getProg()->getProject();
    project->alertDecompiling(proc);
    project->alertDecompileDebugPoint(proc, "Before Final");

    LOG_VERBOSE("### Removing unused statements for %1 ###", proc->getName());

    // Perform type analysis. If we are relying (as we are at present) on TA to perform ellipsis
    // processing, do the local TA pass now. Ellipsis processing often reveals additional uses (e.g.
    // additional parameters to printf/scanf), and removing unused statements is unsafe without full
    // use information
    if (proc->getStatus() < PROC_FINAL) {
        PassManager::get()->executePass(PassID::LocalTypeAnalysis, proc);

        // Now that locals are identified, redo the dataflow
        PassManager::get()->executePass(PassID::PhiPlacement, proc);

        PassManager::get()->executePass(PassID::BlockVarRename, proc); // Rename the locals
        PassManager::get()->executePass(PassID::StatementPropagation,
                                        proc); // Surely need propagation too

        if (project->getSettings()->verboseOutput) {
            proc->debugPrintAll("after propagating locals");
        }
    }

    PassManager::get()->executePass(PassID::UnusedStatementRemoval, proc);
    PassManager::get()->executePass(PassID::FinalParameterSearch, proc);

    if (project->getSettings()->nameParameters) {
        // Replace the existing temporary parameters with the final ones:
        // mapExpressionsToParameters();
        PassManager::get()->executePass(PassID::ParameterSymbolMap, proc);
        proc->debugPrintAll("after adding new parameters");
    }

    // Or just CallArgumentUpdate?
    PassManager::get()->executePass(PassID::CallDefineUpdate, proc);
    PassManager::get()->executePass(PassID::CallArgumentUpdate, proc);
    PassManager::get()->executePass(PassID::BranchAnalysis, proc);

    proc->debugPrintAll("after remove unused statements etc");
    project->alertDecompileDebugPoint(proc, "after final");
}


void ProcDecompiler::printCallStack()
{
    LOG_MSG("Call stack (most recent procedure last):");
    for (UserProc *proc : m_callStack) {
        LOG_MSG("    %1", proc->getName());
    }
}


void ProcDecompiler::saveDecodedICTs(UserProc *proc)
{
    for (BasicBlock *bb : *proc->getCFG()) {
        BasicBlock::RTLRIterator rrit;
        StatementList::reverse_iterator srit;
        Statement *last = bb->getLastStmt(rrit, srit);

        if (last == nullptr) {
            continue; // e.g. a BB with just a NOP in it
        }

        if (!last->isHL_ICT()) {
            continue;
        }

        RTL *rtl = bb->getLastRTL();

        if (proc->getProg()->getProject()->getSettings()->debugSwitch) {
            LOG_MSG("Saving high level switch statement:\n%1", rtl);
        }

        proc->getProg()->getFrontEnd()->saveDecodedRTL(bb->getHiAddr(), rtl);
    }
}
