#pragma region License
/*
 * This file is part of the Boomerang Decompiler.
 *
 * See the file "LICENSE.TERMS" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL
 * WARRANTIES.
 */
#pragma endregion License
#include "IndirectJumpAnalyzer.h"

#include "boomerang/core/Project.h"
#include "boomerang/core/Settings.h"
#include "boomerang/db/Prog.h"
#include "boomerang/db/proc/UserProc.h"
#include "boomerang/ssl/RTL.h"
#include "boomerang/ssl/exp/Binary.h"
#include "boomerang/ssl/exp/Const.h"
#include "boomerang/ssl/exp/Location.h"
#include "boomerang/ssl/statements/BranchStatement.h"
#include "boomerang/ssl/statements/CallStatement.h"
#include "boomerang/ssl/statements/CaseStatement.h"
#include "boomerang/ssl/statements/PhiAssign.h"
#include "boomerang/ssl/type/FuncType.h"
#include "boomerang/ssl/type/PointerType.h"
#include "boomerang/util/log/Log.h"
#include "boomerang/visitor/expmodifier/ConstGlobalConverter.h"


// Switch High Level patterns

// With array processing, we get a new form, call it form 'a' (don't confuse with form 'A'):
// Pattern: <base>{}[<index>]{} where <index> could be <var> - <Kmin>
// TODO: use initializer lists
static const SharedConstExp form_a = RefExp::get(
    Binary::get(opArrayIndex, RefExp::get(Terminal::get(opWild), STMT_WILD), Terminal::get(opWild)),
    STMT_WILD);

// Pattern: m[<expr> * 4 + T ]
static const SharedConstExp form_A = Location::memOf(
    Binary::get(opPlus, Binary::get(opMult, Terminal::get(opWild), Const::get(4)),
                Terminal::get(opWildIntConst)));

// With array processing, we get a new form, call it form 'o' (don't confuse with form 'O'):
// Pattern: <base>{}[<index>]{} where <index> could be <var> - <Kmin>
// NOT COMPLETED YET!
static const SharedConstExp form_o = RefExp::get(
    Binary::get(opArrayIndex, RefExp::get(Terminal::get(opWild), STMT_WILD), Terminal::get(opWild)),
    STMT_WILD);

// Pattern: m[<expr> * 4 + T ] + T
static const SharedConstExp form_O = Binary::get(
    opPlus,
    Location::memOf(Binary::get(opPlus, Binary::get(opMult, Terminal::get(opWild), Const::get(4)),
                                Terminal::get(opWildIntConst))),
    Terminal::get(opWildIntConst));

// Pattern: %pc + m[%pc     + (<expr> * 4) + k]
// where k is a small constant, typically 28 or 20
static const SharedConstExp form_R = Binary::get(
    opPlus, Terminal::get(opPC),
    Location::memOf(
        Binary::get(opPlus, Terminal::get(opPC),
                    Binary::get(opPlus, Binary::get(opMult, Terminal::get(opWild), Const::get(4)),
                                Const::get(opWildIntConst)))));

// Pattern: %pc + m[%pc + ((<expr> * 4) - k)] - k
// where k is a smallish constant, e.g. 288 (/usr/bin/vi 2.6, 0c4233c).
static const SharedConstExp form_r = Binary::get(
    opPlus, Terminal::get(opPC),
    Location::memOf(
        Binary::get(opPlus, Terminal::get(opPC),
                    Binary::get(opMinus, Binary::get(opMult, Terminal::get(opWild), Const::get(4)),
                                Terminal::get(opWildIntConst)))));


struct SwitchForm
{
    SharedConstExp pattern;
    SwitchType type;
};

// clang-format off
static const SwitchForm hlForms[] = {
    { form_a, SwitchType::a },
    { form_A, SwitchType::A },
    { form_o, SwitchType::o },
    { form_O, SwitchType::O },
    { form_R, SwitchType::R },
    { form_r, SwitchType::r }
};
// clang-format on


// Vcall high level patterns
// Pattern 0: global<wild>[0]
static const SharedConstExp vfc_funcptr = Binary::get(
    opArrayIndex, Location::get(opGlobal, Terminal::get(opWildStrConst), nullptr), Const::get(0));

// Pattern 1: m[ m[ <expr> + K1 ] + K2 ]
// K1 is vtable offset, K2 is virtual function offset (could come from m[A2], if A2 is in read-only
// memory
static const SharedConstExp vfc_both = Location::memOf(Binary::get(
    opPlus,
    Location::memOf(Binary::get(opPlus, Terminal::get(opWild), Terminal::get(opWildIntConst))),
    Terminal::get(opWildIntConst)));

// Pattern 2: m[ m[ <expr> ] + K2]
static const SharedConstExp vfc_vto = Location::memOf(
    Binary::get(opPlus, Location::memOf(Terminal::get(opWild)), Terminal::get(opWildIntConst)));

// Pattern 3: m[ m[ <expr> + K1] ]
static const SharedConstExp vfc_vfo = Location::memOf(
    Location::memOf(Binary::get(opPlus, Terminal::get(opWild), Terminal::get(opWildIntConst))));

// Pattern 4: m[ m[ <expr> ] ]
static const SharedConstExp vfc_none = Location::memOf(Location::memOf(Terminal::get(opWild)));

static const SharedConstExp hlVfc[] = { vfc_funcptr, vfc_both, vfc_vto, vfc_vfo, vfc_none };


/// Find all the possible constant values that the location defined by s could be assigned with
static void findConstantValues(const Statement *s, std::list<int> &dests)
{
    if (s == nullptr) {
        return;
    }

    if (s->isPhi()) {
        // For each definition, recurse
        for (const auto &it : *static_cast<const PhiAssign *>(s)) {
            findConstantValues(it.getDef(), dests);
        }
    }
    else if (s->isAssign()) {
        SharedExp rhs = static_cast<const Assign *>(s)->getRight();

        if (rhs->isIntConst()) {
            dests.push_back(rhs->access<Const>()->getInt());
        }
    }
}


void findSwParams(SwitchType form, SharedExp e, SharedExp &expr, Address &T)
{
    switch (form) {
    case SwitchType::a: {
        // Pattern: <base>{}[<index>]{}
        if (e->isSubscript()) {
            e = e->getSubExp1();
        }

        assert(e->getOper() == opArrayIndex);
        SharedExp base = e->getSubExp1();

        if (base->isSubscript()) {
            base = base->getSubExp1();
        }

        assert(base->isGlobal());
        assert(base->getSubExp1()->isStrConst());

        QString gloName = base->access<Const, 1>()->getStr();
        UserProc *p     = std::static_pointer_cast<Location>(base)->getProc();
        Prog *prog      = p->getProg();
        T               = prog->getGlobalAddrByName(gloName);
        expr            = e->getSubExp2();
        break;
    }

    case SwitchType::A: {
        // Pattern: m[<expr> * 4 + T ]
        if (e->isSubscript()) {
            e = e->getSubExp1();
        }

        // b will be (<expr> * 4) + T
        SharedExp b = e->getSubExp1();
        T           = b->access<Const, 2>()->getAddr();
        expr        = b->access<Exp, 1, 1>();
        break;
    }

    case SwitchType::O: { // Form O
        // Pattern: m[<expr> * 4 + T ] + T
        T = e->access<Const, 2>()->getAddr();
        // l = m[<expr> * 4 + T ]:
        SharedExp l = e->getSubExp1();

        if (l->isSubscript()) {
            l = l->getSubExp1();
        }

        // b = <expr> * 4 + T:
        SharedExp b = l->getSubExp1();
        // b = <expr> * 4:
        b = b->getSubExp1();
        // expr = <expr>:
        expr = b->getSubExp1();
        break;
    }

    case SwitchType::R: {
        // Pattern: %pc + m[%pc     + (<expr> * 4) + k]
        T = Address::ZERO; // ?
        // l = m[%pc  + (<expr> * 4) + k]:
        SharedExp l = e->getSubExp2();

        if (l->isSubscript()) {
            l = l->getSubExp1();
        }

        // b = %pc    + (<expr> * 4) + k:
        SharedExp b = l->getSubExp1();
        // b = (<expr> * 4) + k:
        b = b->getSubExp2();
        // b = <expr> * 4:
        b = b->getSubExp1();
        // expr = <expr>:
        expr = b->getSubExp1();
        break;
    }

    case SwitchType::r: {
        // Pattern: %pc + m[%pc + ((<expr> * 4) - k)] - k
        T = Address::ZERO; // ?
        // b = %pc + m[%pc + ((<expr> * 4) - k)]:
        SharedExp b = e->getSubExp1();
        // l = m[%pc + ((<expr> * 4) - k)]:
        SharedExp l = b->getSubExp2();

        if (l->isSubscript()) {
            l = l->getSubExp1();
        }

        // b = %pc + ((<expr> * 4) - k)
        b = l->getSubExp1();
        // b = ((<expr> * 4) - k):
        b = b->getSubExp2();
        // b = <expr> * 4:
        b = b->getSubExp1();
        // expr = <expr>
        expr = b->getSubExp1();
        break;
    }

    default: expr = nullptr; T = Address::INVALID;
    }

    // normalize address to native
    T = T.native();
}


bool IndirectJumpAnalyzer::decodeIndirectJmp(BasicBlock *bb, UserProc *proc)
{
#if CHECK_REAL_PHI_LOOPS
    rtlit rit;
    StatementList::iterator sit;
    Statement *s = getFirstStmt(rit, sit);

    for (s = getFirstStmt(rit, sit); s; s = getNextStmt(rit, sit)) {
        if (!s->isPhi()) {
            continue;
        }

        Statement *originalPhi = s;
        InstructionSet workSet, seenSet;
        workSet.insert(s);
        seenSet.insert(s);

        do {
            PhiAssign *pi = (PhiAssign *)*workSet.begin();
            workSet.remove(pi);
            PhiAssign::Definitions::iterator it;

            for (it = pi->begin(); it != pi->end(); ++it) {
                if (it->def == nullptr) {
                    continue;
                }

                if (!it->def->isPhi()) {
                    continue;
                }

                if (seenSet.exists(it->def)) {
                    LOG_VERBOSE("Real phi loop involving statements %1 and %2",
                                originalPhi->getNumber(), pi->getNumber());
                    break;
                }
                else {
                    workSet.insert(it->def);
                    seenSet.insert(it->def);
                }
            }
        } while (workSet.size());
    }
#endif

    if (bb->isType(BBType::CompJump)) {
        assert(!bb->getRTLs()->empty());
        RTL *lastRTL = bb->getLastRTL();

        if (proc->getProg()->getProject()->getSettings()->debugSwitch) {
            LOG_MSG("decodeIndirectJmp: %1", lastRTL->prints());
        }

        assert(!lastRTL->empty());
        CaseStatement *lastStmt = static_cast<CaseStatement *>(lastRTL->back());

        // Note: some programs might not have the case expression propagated to, because of the -l
        // switch (?) We used to use ordinary propagation here to get the memory expression, but now
        // it refuses to propagate memofs because of the alias safety issue. Eventually, we should
        // use an alias-safe incremental propagation, but for now we'll assume no alias problems and
        // force the propagation
        bool convert = false;
        lastStmt->propagateTo(convert, proc->getProg()->getProject()->getSettings(), nullptr,
                              nullptr, true /* force */);
        SharedExp jumpDest = lastStmt->getDest();

        SwitchType switchType = SwitchType::Invalid;

        for (auto &val : hlForms) {
            if (*jumpDest *= *val.pattern) { // *= compare ignores subscripts
                switchType = val.type;

                if (proc->getProg()->getProject()->getSettings()->debugSwitch) {
                    LOG_MSG("Indirect jump matches form %1", static_cast<char>(switchType));
                }

                break;
            }
        }

        if (switchType != SwitchType::Invalid) {
            SwitchInfo *swi = new SwitchInfo;
            swi->switchType = switchType;
            Address T       = Address::INVALID;
            SharedExp expr;
            findSwParams(switchType, jumpDest, expr, T);

            if (expr) {
                swi->tableAddr       = T;
                swi->numTableEntries = findNumCases(bb);

                // TMN: Added actual control of the array members, to possibly truncate what
                // findNumCases() thinks is the number of cases, when finding the first array
                // element not pointing to code.
                if (switchType == SwitchType::A) {
                    const Prog *prog = proc->getProg();

                    for (int entryIdx = 0; entryIdx < swi->numTableEntries; ++entryIdx) {
                        Address switchEntryAddr = Address(
                            prog->readNative4(swi->tableAddr + entryIdx * 4));

                        if (!Util::inRange(switchEntryAddr, prog->getLimitTextLow(),
                                           prog->getLimitTextHigh())) {
                            if (proc->getProg()->getProject()->getSettings()->debugSwitch) {
                                LOG_MSG("Truncating type A indirect jump array to %1 entries "
                                        "due to finding an array entry pointing outside valid "
                                        "code; %2 isn't in %3..%4",
                                        entryIdx, switchEntryAddr, prog->getLimitTextLow(),
                                        prog->getLimitTextHigh());
                            }

                            // Found an array that isn't a pointer-to-code. Assume array has ended.
                            swi->numTableEntries = entryIdx;
                            break;
                        }
                    }
                }

                if (swi->numTableEntries <= 0) {
                    LOG_WARN("Switch analysis failure at address %1", bb->getLowAddr());
                    return false;
                }

                // TODO: missing switchType = 'R' offset is not being set
                swi->upperBound = swi->numTableEntries - 1;
                swi->lowerBound = 0;

                if ((expr->getOper() == opMinus) && expr->getSubExp2()->isIntConst()) {
                    swi->lowerBound = std::static_pointer_cast<Const>(expr->getSubExp2())->getInt();
                    swi->upperBound += swi->lowerBound;
                    expr = expr->getSubExp1();
                }

                swi->switchExp = expr;
                lastStmt->setDest(nullptr);
                lastStmt->setSwitchInfo(swi);
                return swi->numTableEntries != 0;
            }
        }
        else {
            // Did not match a switch pattern. Perhaps it is a Fortran style goto with constants at
            // the leaves of the phi tree. Basically, a location with a reference, e.g. m[r28{-} -
            // 16]{87}
            if (jumpDest->isSubscript()) {
                SharedExp sub = jumpDest->getSubExp1();

                if (sub->isLocation()) {
                    // Yes, we have <location>{ref}. Follow the tree and store the constant values
                    // that <location> could be assigned to in dests
                    std::list<int> dests;
                    findConstantValues(std::static_pointer_cast<RefExp>(jumpDest)->getDef(), dests);
                    // The switch info wants an array of native addresses
                    size_t num_dests = dests.size();

                    if (num_dests > 0) {
                        int *destArray = new int[num_dests];
                        std::copy(dests.begin(), dests.end(), destArray);
                        SwitchInfo *swi = new SwitchInfo;
                        swi->switchType = SwitchType::F; // The "Fortran" form
                        swi->switchExp  = jumpDest;
                        swi->tableAddr  = Address(
                            HostAddress(destArray).value()); // WARN: HACK HACK HACK Abuse the
                                                              // tableAddr member as a pointer
                        swi->lowerBound      = 1;             // Not used, except to compute
                        swi->upperBound      = static_cast<int>(num_dests); // the number of options
                        swi->numTableEntries = static_cast<int>(num_dests);
                        lastStmt->setDest(nullptr);
                        lastStmt->setSwitchInfo(swi);
                        return true;
                    }
                }
            }
        }

        return false;
    }
    else if (bb->isType(BBType::CompCall)) {
        assert(!bb->getRTLs()->empty());
        RTL *lastRTL = bb->getLastRTL();

        if (proc->getProg()->getProject()->getSettings()->debugSwitch) {
            LOG_MSG("decodeIndirectJmp: COMPCALL:");
            LOG_MSG("%1", lastRTL->prints());
        }

        assert(!lastRTL->empty());
        CallStatement *lastStmt = static_cast<CallStatement *>(lastRTL->back());
        SharedExp e             = lastStmt->getDest();
        // Indirect calls may sometimes not be propagated to, because of limited propagation (-l
        // switch). Propagate to e, but only keep the changes if the expression matches (don't want
        // excessive propagation to a genuine function pointer expression, even though it's hard to
        // imagine).
        e = e->propagateAll();

        // We also want to replace any m[K]{-} with the actual constant from the (presumably)
        // read-only data section
        ConstGlobalConverter cgc(proc->getProg());
        e = e->acceptModifier(&cgc);
        // Simplify the result, e.g. for m[m[(r24{16} + m[0x8048d74]{-}) + 12]{-}]{-} get
        // m[m[(r24{16} + 20) + 12]{-}]{-}, want m[m[r24{16} + 32]{-}]{-}. Note also that making the
        // ConstGlobalConverter a simplifying expression modifier won't work in this case, since the
        // simplifying converter will only simplify the direct parent of the changed expression
        // (which is r24{16} + 20).
        e = e->simplify();

        if (proc->getProg()->getProject()->getSettings()->debugSwitch) {
            LOG_MSG("decodeIndirect: propagated and const global converted call expression is %1",
                    e);
        }

        int n           = sizeof(hlVfc) / sizeof(SharedExp);
        bool recognised = false;
        int i;

        for (i = 0; i < n; i++) {
            if (*e *= *hlVfc[i]) { // *= compare ignores subscripts
                recognised = true;

                if (proc->getProg()->getProject()->getSettings()->debugSwitch) {
                    LOG_MSG("Indirect call matches form %1", i);
                }

                break;
            }
        }

        if (!recognised) {
            return false;
        }

        lastStmt->setDest(e); // Keep the changes to the indirect call expression
        int K1, K2;
        SharedExp vtExp, t1;
        Prog *prog = proc->getProg();

        switch (i) {
        case 0: {
            // This is basically an indirection on a global function pointer.  If it is initialised,
            // we have a decodable entry point.  Note: it could also be a library function (e.g.
            // Windows) Pattern 0: global<name>{0}[0]{0}
            K2 = 0;

            if (e->isSubscript()) {
                e = e->getSubExp1();
            }

            e             = e->getSubExp1(); // e is global<name>{0}[0]
            t1            = e->getSubExp2();
            auto t1_const = std::static_pointer_cast<Const>(t1);

            if (e->isArrayIndex() && (t1->isIntConst()) && (t1_const->getInt() == 0)) {
                e = e->getSubExp1(); // e is global<name>{0}
            }

            if (e->isSubscript()) {
                e = e->getSubExp1(); // e is global<name>
            }

            std::shared_ptr<Const> con = std::static_pointer_cast<Const>(
                e->getSubExp1()); // e is <name>
            Global *global = prog->getGlobalByName(con->getStr());
            assert(global);
            // Set the type to pointer to function, if not already
            SharedType ty = global->getType();

            if (!ty->isPointer() &&
                !std::static_pointer_cast<PointerType>(ty)->getPointsTo()->isFunc()) {
                global->setType(PointerType::get(FuncType::get()));
            }

            Address addr = global->getAddress();
            // FIXME: not sure how to find K1 from here. I think we need to find the earliest(?)
            // entry in the data map that overlaps with addr For now, let K1 = 0:
            K1    = 0;
            vtExp = Const::get(addr);
            break;
        }

        case 1: {
            // Example pattern: e = m[m[r27{25} + 8]{-} + 8]{-}
            if (e->isSubscript()) {
                e = e->getSubExp1();
            }

            e             = e->getSubExp1(); // e = m[r27{25} + 8]{-} + 8
            SharedExp rhs = e->getSubExp2(); // rhs = 8
            K2            = std::static_pointer_cast<Const>(rhs)->getInt();
            SharedExp lhs = e->getSubExp1(); // lhs = m[r27{25} + 8]{-}

            if (lhs->isSubscript()) {
                lhs = lhs->getSubExp1(); // lhs = m[r27{25} + 8]
            }

            vtExp         = lhs;
            lhs           = lhs->getSubExp1(); // lhs =   r27{25} + 8
            SharedExp CK1 = lhs->getSubExp2();
            K1            = std::static_pointer_cast<Const>(CK1)->getInt();
            break;
        }

        case 2: {
            // Example pattern: e = m[m[r27{25}]{-} + 8]{-}
            if (e->isSubscript()) {
                e = e->getSubExp1();
            }

            e             = e->getSubExp1(); // e = m[r27{25}]{-} + 8
            SharedExp rhs = e->getSubExp2(); // rhs = 8
            K2            = std::static_pointer_cast<Const>(rhs)->getInt();
            SharedExp lhs = e->getSubExp1(); // lhs = m[r27{25}]{-}

            if (lhs->isSubscript()) {
                lhs = lhs->getSubExp1(); // lhs = m[r27{25}]
            }

            vtExp = lhs;
            K1    = 0;
            break;
        }

        case 3: {
            // Example pattern: e = m[m[r27{25} + 8]{-}]{-}
            if (e->isSubscript()) {
                e = e->getSubExp1();
            }

            e  = e->getSubExp1(); // e = m[r27{25} + 8]{-}
            K2 = 0;

            if (e->isSubscript()) {
                e = e->getSubExp1(); // e = m[r27{25} + 8]
            }

            vtExp         = e;
            SharedExp lhs = e->getSubExp1(); // lhs =   r27{25} + 8
            // Exp* object = ((Binary*)lhs)->getSubExp1();
            SharedExp CK1 = lhs->getSubExp2();
            K1            = std::static_pointer_cast<Const>(CK1)->getInt();
            break;
        }

        case 4:

            // Example pattern: e = m[m[r27{25}]{-}]{-}
            if (e->isSubscript()) {
                e = e->getSubExp1();
            }

            e  = e->getSubExp1(); // e = m[r27{25}]{-}
            K2 = 0;

            if (e->isSubscript()) {
                e = e->getSubExp1(); // e = m[r27{25}]
            }

            vtExp = e;
            K1    = 0;
            // Exp* object = ((Unary*)e)->getSubExp1();
            break;

        default:
            K1 = K2 = -1; // Suppress warnings
            vtExp   = nullptr;
        }

        if (proc->getProg()->getProject()->getSettings()->debugSwitch) {
            LOG_MSG("Form %1: from statement %2 get e = %3, K1 = %4, K2 = %5, vtExp = %6", i,
                    lastStmt->getNumber(), lastStmt->getDest(), K1, K2, vtExp);
        }

        // The vt expression might not be a constant yet, because of expressions not fully
        // propagated, or because of m[K] in the expression (fixed with the ConstGlobalConverter).
        // If so, look it up in the defCollector in the call
        vtExp = lastStmt->findDefFor(vtExp);

        if (vtExp && proc->getProg()->getProject()->getSettings()->debugSwitch) {
            LOG_MSG("VT expression boils down to this: %1", vtExp);
        }

        // Danger. For now, only do if -ic given
        const bool decodeThru = proc->getProg()->getProject()->getSettings()->decodeThruIndCall;

        if (decodeThru && vtExp && vtExp->isIntConst()) {
            Address addr  = std::static_pointer_cast<Const>(vtExp)->getAddr();
            Address pfunc = Address(prog->readNative4(addr));

            if (prog->getFunctionByAddr(pfunc) == nullptr) {
                // A new, undecoded procedure
                if (!prog->getProject()->getSettings()->decodeChildren) {
                    return false;
                }

                prog->decodeEntryPoint(pfunc);
                // Since this was not decoded, this is a significant change, and we want to redecode
                // the current function now that the callee has been decoded
                return true;
            }
        }
    }

    return false;
}


int IndirectJumpAnalyzer::findNumCases(const BasicBlock *bb)
{
    // should actually search from the statement to i
    for (const BasicBlock *pred : bb->getPredecessors()) { // For each in-edge
        if (!pred->isType(BBType::Twoway)) {               // look for a two-way BB
            continue;                                      // Ignore all others
        }

        const BranchStatement *lastStmt = dynamic_cast<const BranchStatement *>(
            pred->getLastStmt());
        assert(lastStmt != nullptr);
        SharedConstExp lastCondition = lastStmt->getCondExpr();
        if (lastCondition->getArity() != 2) {
            continue;
        }

        SharedConstExp rhs = lastCondition->getSubExp2();

        if (!rhs->isIntConst()) {
            continue;
        }

        const int k   = std::static_pointer_cast<const Const>(rhs)->getInt();
        const OPER op = lastCondition->getOper();

        switch (op) {
        case opGtr:
        case opGtrUns:
        case opLessEq:
        case opLessEqUns: return k + 1;

        case opGtrEq:
        case opGtrEqUns:
        case opLess:
        case opLessUns: return k;

        default: break;
        }
    }

    LOG_WARN("Could not find number of cases for n-way at address %1", bb->getLowAddr());
    return 1; // Bald faced guess if all else fails
}


void IndirectJumpAnalyzer::processSwitch(BasicBlock *bb, UserProc *proc)
{
    RTL *lastRTL   = bb->getLastRTL();
    SwitchInfo *si = static_cast<CaseStatement *>(lastRTL->getHlStmt())->getSwitchInfo();

    if (proc->getProg()->getProject()->getSettings()->debugSwitch) {
        LOG_MSG("Processing switch statement type %1 with table at %2, %3 entries, lo=%4, hi=%5",
                static_cast<char>(si->switchType), si->tableAddr, si->numTableEntries,
                si->lowerBound, si->upperBound);
    }

    Address switchDestination;
    const int numCases = si->upperBound - si->lowerBound + 1;

    // Emit an NWAY BB instead of the COMPJUMP. Also update the number of out edges.
    bb->setType(BBType::Nway);

    Prog *prog   = proc->getProg();
    ProcCFG *cfg = proc->getCFG();

    // Where there are repeated switch cases, we have repeated out-edges from the BB. Example:
    // switch (x) {
    //   case 3: case 5:
    //        do something;
    //        break;
    //     case 4: case 10:
    //        do something else
    // ... }
    // The switch statement is emitted assuming one out-edge for each switch value, which is assumed
    // to be lowerBound+i for the ith zero-based case. It may be that the code for case 5 above will
    // be a goto to the code for case 3, but a smarter back end could group them
    std::list<Address> dests;

    for (int i = 0; i < numCases; i++) {
        // Get the destination address from the switch table.
        if (si->switchType == SwitchType::H) {
            const int switchValue = prog->readNative4(si->tableAddr + i * 2);

            if (switchValue == -1) {
                continue;
            }

            switchDestination = Address(prog->readNative4(si->tableAddr + i * 8 + 4));
        }
        else if (si->switchType == SwitchType::F) {
            Address::value_type *entry = reinterpret_cast<Address::value_type *>(
                si->tableAddr.value());
            switchDestination = Address(entry[i]);
        }
        else {
            switchDestination = Address(prog->readNative4(si->tableAddr + i * 4));
        }

        if ((si->switchType == SwitchType::O) || (si->switchType == SwitchType::R) ||
            (si->switchType == SwitchType::r)) {
            // Offset: add table address to make a real pointer to code.  For type R, the table is
            // relative to the branch, so take offsetFromJumpTbl. For others, offsetFromJumpTbl is
            // 0, so no harm
            if (si->switchType != SwitchType::R) {
                assert(si->offsetFromJumpTbl == 0);
            }

            switchDestination += si->tableAddr - si->offsetFromJumpTbl;
        }

        if (switchDestination < prog->getLimitTextHigh()) {
            cfg->addEdge(bb, switchDestination);

            // Remember to decode the newly discovered switch code arms, if necessary
            // Don't do it right now, in case there are recursive switch statements (e.g.
            // app7win.exe from hackthissite.org)
            dests.push_back(switchDestination);
        }
        else {
            LOG_MSG("Switch table entry branches to past end of text section %1",
                    switchDestination);

            // TMN: If we reached an array entry pointing outside the program text, we can be quite
            // confident the array has ended. Don't try to pull any more data from it.
            LOG_MSG("Assuming the end of the pointer-array has been reached at index %1", i);

            // TODO: Elevate this logic to the code calculating iNumTable, but still leave this code
            // as a safeguard. Q: Should iNumOut and m_iNumOutEdges really be adjusted (iNum - i) ?
            int numToRemove = std::max(numCases - i, 0);

            // remove all table elements at index i and above
            while (numToRemove > 0) {
                BasicBlock *succ = bb->getSuccessor(i);
                if (succ) {
                    bb->removeSuccessor(succ);
                    succ->removePredecessor(bb);
                }
                numToRemove--;
            }
            break;
        }
    }

    // Decode the newly discovered switch code arms, if any, and if not already decoded
    int count = 0;

    for (Address addr : dests) {
        char tmp[1024];
        count++;
        snprintf(tmp, 1024, "before decoding fragment %i of %zu (%s)", count, dests.size(),
                 qPrintable(addr.toString()));
        proc->getProg()->getProject()->alertDecompileDebugPoint(proc, tmp);
        prog->decodeFragment(proc, addr);
    }
}
