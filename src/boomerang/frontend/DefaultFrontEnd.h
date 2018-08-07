#pragma region License
/*
 * This file is part of the Boomerang Decompiler.
 *
 * See the file "LICENSE.TERMS" for information on usage and
 * redistribution of this file, and for a DISCLAIMER OF ALL
 * WARRANTIES.
 */
#pragma endregion License
#pragma once


#include "boomerang/frontend/SigEnum.h"
#include "boomerang/frontend/TargetQueue.h"
#include "boomerang/ifc/IFrontEnd.h"

#include <memory>


class Function;
class UserProc;
class RTL;
class IDecoder;
class Exp;
class Prog;
struct DecodeResult;
class Signature;
class Statement;
class CallStatement;
class BinaryFile;

class QString;


/**
 * Contains the default implementation of the source
 * indendent parts of the front end:
 * Decoding machine instructions into a control flow graph
 * populated with low and high level RTLs.
 */
class DefaultFrontEnd : public IFrontEnd
{
public:
    /**
     * \param prog   program being decoded
     */
    DefaultFrontEnd(BinaryFile *binaryFile, Prog *prog);
    DefaultFrontEnd(const DefaultFrontEnd&) = delete;
    DefaultFrontEnd(DefaultFrontEnd&&) = default;

    virtual ~DefaultFrontEnd();

    DefaultFrontEnd& operator=(const DefaultFrontEnd&) = delete;
    DefaultFrontEnd& operator=(DefaultFrontEnd&&) = default;

public:
    /// \copydoc IFrontEnd::isNoReturnCallDest
    virtual bool isNoReturnCallDest(const QString& procName) const override;

    /// \copydoc IFrontEnd::getDecoder
    IDecoder *getDecoder() override { return m_decoder.get(); }
    const IDecoder *getDecoder() const override { return m_decoder.get(); }

    /// \copydoc IFrontEnd::getRegName
    QString getRegName(int idx) const override;

    /// \copydoc IFrontEnd::getRegSize
    int getRegSize(int idx) const override;

    /// \copydoc IFrontEnd::addRefHint
    void addRefHint(Address addr, const QString& name) override { m_refHints[addr] = name; }

    /// \copydoc IFrontEnd::decodeInstruction
    virtual bool decodeInstruction(Address pc, DecodeResult& result) override;

    /// \copydoc IFrontEnd::extraProcessCall
    virtual void extraProcessCall(CallStatement * /*call*/, const RTLList& /*BB_rtls*/) override {}

    /// \copydoc IFrontEnd::decodeEntryPointsRecursive
    bool decodeEntryPointsRecursive(bool decodeMain = true) override;

    /// \copydoc IFrontEnd::decodeRecursive
    bool decodeRecursive(Address addr) override;

    /// \copydoc IFrontEnd::decodeUndecoded
    bool decodeUndecoded() override;

    /// \copydoc IFrontEnd::decodeOnly
    bool decodeOnly(Address addr) override;

    /// \copydoc IFrontEnd::decodeFragment
    bool decodeFragment(UserProc *proc, Address addr) override;

    /// \copydoc IFrontEnd::processProc
    virtual bool processProc(UserProc *proc, Address addr) override;

    /// \copydoc IFrontEnd::isHelperFunc
    virtual bool isHelperFunc(Address /*dest*/, Address /*addr*/, RTLList& /*lrtl*/) override { return false; }

    /// \copydoc IFrontEnd::getEntryPoints
    std::vector<Address> findEntryPoints() override;

    /// \copydoc IFrontEnd::createReturnBlock
    BasicBlock *createReturnBlock(UserProc *proc,
        std::unique_ptr<RTLList> BB_rtls, std::unique_ptr<RTL> returnRTL) override;

    /// \copydoc IFrontEnd::saveDecodedRTL
    void saveDecodedRTL(Address a, RTL *rtl) override { m_previouslyDecoded[a] = rtl; }

private:
    bool refersToImportedFunction(const SharedExp& exp);

    /**
     * Add a synthetic return instruction and basic block (or a branch to the existing return instruction).
     *
     * \note the call BB should be created with one out edge (the return or branch BB)
     * \param callBB  the call BB that will be followed by the return or jump
     * \param proc    the enclosing UserProc
     * \param callRTL the current RTL with the call instruction
     */
    void appendSyntheticReturn(BasicBlock *callBB, UserProc *proc, RTL *callRTL);

    void preprocessProcGoto(std::list<Statement *>::iterator ss, Address dest, const std::list<Statement *>& sl, RTL *originalRTL);
    void checkEntryPoint(std::vector<Address>& entrypoints, Address addr, const char *type);

protected:
    std::unique_ptr<IDecoder> m_decoder;
    BinaryFile *m_binaryFile;
    Prog *m_program;

    TargetQueue m_targetQueue; ///< Holds the addresses that still need to be processed

    /// Map from address to meaningful name
    std::map<Address, QString> m_refHints;

    /// Map from address to previously decoded RTLs for decoded indirect control transfer instructions
    std::map<Address, RTL *> m_previouslyDecoded;
};
