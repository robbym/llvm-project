//===- DSPICAsmParser.cpp - Parse DSPIC assembly to MCInst instructions -===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/DSPICMCTargetDesc.h"
#include "DSPIC.h"
#include "TargetInfo/DSPICTargetInfo.h"

#include "llvm/ADT/APInt.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCParser/AsmLexer.h"
#include "llvm/MC/MCParser/MCParsedAsmOperand.h"
#include "llvm/MC/MCParser/MCTargetAsmParser.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/MC/MCValue.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "dspic-asm-parser"

using namespace llvm;

namespace {

/// Parses DSPIC assembly from a stream.
class DSPICAsmParser : public MCTargetAsmParser {
  MCAsmParser &Parser;
  const MCRegisterInfo *MRI;

  bool matchAndEmitInstruction(SMLoc IDLoc, unsigned &Opcode,
                               OperandVector &Operands, MCStreamer &Out,
                               uint64_t &ErrorInfo,
                               bool MatchingInlineAsm) override;

  bool parseRegister(MCRegister &Reg, SMLoc &StartLoc, SMLoc &EndLoc) override;
  ParseStatus tryParseRegister(MCRegister &Reg, SMLoc &StartLoc,
                               SMLoc &EndLoc) override;

  bool parseInstruction(ParseInstructionInfo &Info, StringRef Name,
                        SMLoc NameLoc, OperandVector &Operands) override;

  ParseStatus parseDirective(AsmToken DirectiveID) override;
  bool ParseDirectiveRefSym(AsmToken DirectiveID);

  unsigned validateTargetOperandClass(MCParsedAsmOperand &Op,
                                      unsigned Kind) override;

  bool parseJccInstruction(ParseInstructionInfo &Info, StringRef Name,
                           SMLoc NameLoc, OperandVector &Operands);

  bool ParseOperand(OperandVector &Operands);

  bool ParseLiteralValues(unsigned Size, SMLoc L);

  MCAsmParser &getParser() const { return Parser; }
  AsmLexer &getLexer() const { return Parser.getLexer(); }

  /// @name Auto-generated Matcher Functions
  /// {

#define GET_ASSEMBLER_HEADER
#include "DSPICGenAsmMatcher.inc"

  /// }

public:
  DSPICAsmParser(const MCSubtargetInfo &STI, MCAsmParser &Parser,
                  const MCInstrInfo &MII)
      : MCTargetAsmParser(STI, MII), Parser(Parser) {
    MCAsmParserExtension::Initialize(Parser);
    MRI = getContext().getRegisterInfo();

    setAvailableFeatures(ComputeAvailableFeatures(STI.getFeatureBits()));
  }
};

/// A parsed DSPIC assembly operand.
class DSPICOperand : public MCParsedAsmOperand {
  typedef MCParsedAsmOperand Base;

  enum KindTy {
    k_Imm,
    k_Reg,
    k_Tok,
    k_Mem,
    k_IndReg,
    k_PostIndReg
  } Kind;

  struct Memory {
    MCRegister Reg;
    const MCExpr *Offset;
  };
  union {
    const MCExpr *Imm;
    MCRegister    Reg;
    StringRef     Tok;
    Memory        Mem;
  };

  SMLoc Start, End;

public:
  DSPICOperand(StringRef Tok, SMLoc const &S)
      : Kind(k_Tok), Tok(Tok), Start(S), End(S) {}
  DSPICOperand(KindTy Kind, MCRegister Reg, SMLoc const &S, SMLoc const &E)
      : Kind(Kind), Reg(Reg), Start(S), End(E) {}
  DSPICOperand(MCExpr const *Imm, SMLoc const &S, SMLoc const &E)
      : Kind(k_Imm), Imm(Imm), Start(S), End(E) {}
  DSPICOperand(MCRegister Reg, MCExpr const *Expr, SMLoc const &S,
                SMLoc const &E)
      : Kind(k_Mem), Mem({Reg, Expr}), Start(S), End(E) {}

  void addRegOperands(MCInst &Inst, unsigned N) const {
    assert((Kind == k_Reg || Kind == k_IndReg || Kind == k_PostIndReg) &&
        "Unexpected operand kind");
    assert(N == 1 && "Invalid number of operands!");

    Inst.addOperand(MCOperand::createReg(Reg));
  }

  void addExprOperand(MCInst &Inst, const MCExpr *Expr) const {
    // Add as immediate when possible
    if (!Expr)
      Inst.addOperand(MCOperand::createImm(0));
    else if (const MCConstantExpr *CE = dyn_cast<MCConstantExpr>(Expr))
      Inst.addOperand(MCOperand::createImm(CE->getValue()));
    else
      Inst.addOperand(MCOperand::createExpr(Expr));
  }

  void addImmOperands(MCInst &Inst, unsigned N) const {
    assert(Kind == k_Imm && "Unexpected operand kind");
    assert(N == 1 && "Invalid number of operands!");

    addExprOperand(Inst, Imm);
  }

  void addMemOperands(MCInst &Inst, unsigned N) const {
    assert(Kind == k_Mem && "Unexpected operand kind");
    assert(N == 2 && "Invalid number of operands");

    Inst.addOperand(MCOperand::createReg(Mem.Reg));
    addExprOperand(Inst, Mem.Offset);
  }

  bool isReg()   const override { return Kind == k_Reg; }
  bool isImm()   const override { return Kind == k_Imm; }
  bool isToken() const override { return Kind == k_Tok; }
  bool isMem()   const override { return Kind == k_Mem; }
  bool isIndReg()         const { return Kind == k_IndReg; }
  bool isPostIndReg()     const { return Kind == k_PostIndReg; }

  bool isCGImm() const {
    if (Kind != k_Imm)
      return false;

    int64_t Val;
    if (!Imm->evaluateAsAbsolute(Val))
      return false;
    
    if (Val == 0 || Val == 1 || Val == 2 || Val == 4 || Val == 8 || Val == -1)
      return true;

    return false;
  }

  StringRef getToken() const {
    assert(Kind == k_Tok && "Invalid access!");
    return Tok;
  }

  MCRegister getReg() const override {
    assert(Kind == k_Reg && "Invalid access!");
    return Reg;
  }

  void setReg(MCRegister RegNo) {
    assert(Kind == k_Reg && "Invalid access!");
    Reg = RegNo;
  }

  static std::unique_ptr<DSPICOperand> CreateToken(StringRef Str, SMLoc S) {
    return std::make_unique<DSPICOperand>(Str, S);
  }

  static std::unique_ptr<DSPICOperand> CreateReg(MCRegister Reg, SMLoc S,
                                                  SMLoc E) {
    return std::make_unique<DSPICOperand>(k_Reg, Reg, S, E);
  }

  static std::unique_ptr<DSPICOperand> CreateImm(const MCExpr *Val, SMLoc S,
                                                  SMLoc E) {
    return std::make_unique<DSPICOperand>(Val, S, E);
  }

  static std::unique_ptr<DSPICOperand>
  CreateMem(MCRegister Reg, const MCExpr *Val, SMLoc S, SMLoc E) {
    return std::make_unique<DSPICOperand>(Reg, Val, S, E);
  }

  static std::unique_ptr<DSPICOperand> CreateIndReg(MCRegister Reg, SMLoc S,
                                                     SMLoc E) {
    return std::make_unique<DSPICOperand>(k_IndReg, Reg, S, E);
  }

  static std::unique_ptr<DSPICOperand> CreatePostIndReg(MCRegister Reg,
                                                         SMLoc S, SMLoc E) {
    return std::make_unique<DSPICOperand>(k_PostIndReg, Reg, S, E);
  }

  SMLoc getStartLoc() const override { return Start; }
  SMLoc getEndLoc() const override { return End; }

  void print(raw_ostream &O, const MCAsmInfo &MAI) const override {
    switch (Kind) {
    case k_Tok:
      O << "Token " << Tok;
      break;
    case k_Reg:
      O << "Register " << Reg.id();
      break;
    case k_Imm:
      O << "Immediate ";
      MAI.printExpr(O, *Imm);
      break;
    case k_Mem:
      O << "Memory ";
      MAI.printExpr(O, *Mem.Offset);
      break;
    case k_IndReg:
      O << "RegInd " << Reg.id();
      break;
    case k_PostIndReg:
      O << "PostInc " << Reg.id();
      break;
    }
  }
};
} // end anonymous namespace

bool DSPICAsmParser::matchAndEmitInstruction(SMLoc Loc, unsigned &Opcode,
                                              OperandVector &Operands,
                                              MCStreamer &Out,
                                              uint64_t &ErrorInfo,
                                              bool MatchingInlineAsm) {
  MCInst Inst;
  unsigned MatchResult =
      MatchInstructionImpl(Operands, Inst, ErrorInfo, MatchingInlineAsm);

  switch (MatchResult) {
  case Match_Success:
    Inst.setLoc(Loc);
    Out.emitInstruction(Inst, *STI);
    return false;
  case Match_MnemonicFail:
    return Error(Loc, "invalid instruction mnemonic");
  case Match_InvalidOperand: {
    SMLoc ErrorLoc = Loc;
    if (ErrorInfo != ~0U) {
      if (ErrorInfo >= Operands.size())
        return Error(ErrorLoc, "too few operands for instruction");

      ErrorLoc = ((DSPICOperand &)*Operands[ErrorInfo]).getStartLoc();
      if (ErrorLoc == SMLoc())
        ErrorLoc = Loc;
    }
    return Error(ErrorLoc, "invalid operand for instruction");
  }
  default:
    return true;
  }
}

// Auto-generated by TableGen
static MCRegister MatchRegisterName(StringRef Name);
static MCRegister MatchRegisterAltName(StringRef Name);

bool DSPICAsmParser::parseRegister(MCRegister &Reg, SMLoc &StartLoc,
                                    SMLoc &EndLoc) {
  ParseStatus Res = tryParseRegister(Reg, StartLoc, EndLoc);
  if (Res.isFailure())
    return Error(StartLoc, "invalid register name");
  if (Res.isSuccess())
    return false;
  if (Res.isNoMatch())
    return true;

  llvm_unreachable("unknown parse status");
}

ParseStatus DSPICAsmParser::tryParseRegister(MCRegister &Reg, SMLoc &StartLoc,
                                              SMLoc &EndLoc) {
  if (getLexer().getKind() == AsmToken::Identifier) {
    auto Name = getLexer().getTok().getIdentifier().lower();
    Reg = MatchRegisterName(Name);
    if (Reg == DSPIC::NoRegister) {
      Reg = MatchRegisterAltName(Name);
      if (Reg == DSPIC::NoRegister)
        return ParseStatus::NoMatch;
    }

    AsmToken const &T = getParser().getTok();
    StartLoc = T.getLoc();
    EndLoc = T.getEndLoc();
    getLexer().Lex(); // eat register token

    return ParseStatus::Success;
  }

  return ParseStatus::Failure;
}

bool DSPICAsmParser::parseJccInstruction(ParseInstructionInfo &Info,
                                          StringRef Name, SMLoc NameLoc,
                                          OperandVector &Operands) {
  if (!Name.starts_with_insensitive("j"))
    return true;

  auto CC = Name.drop_front().lower();
  unsigned CondCode;
  if (CC == "ne" || CC == "nz")
    CondCode = DSPICCC::COND_NE;
  else if (CC == "eq" || CC == "z")
    CondCode = DSPICCC::COND_E;
  else if (CC == "lo" || CC == "nc")
    CondCode = DSPICCC::COND_LO;
  else if (CC == "hs" || CC == "c")
    CondCode = DSPICCC::COND_HS;
  else if (CC == "n")
    CondCode = DSPICCC::COND_N;
  else if (CC == "ge")
    CondCode = DSPICCC::COND_GE;
  else if (CC == "l")
    CondCode = DSPICCC::COND_L;
  else if (CC == "mp")
    CondCode = DSPICCC::COND_NONE;
  else
    return Error(NameLoc, "unknown instruction");

  if (CondCode == (unsigned)DSPICCC::COND_NONE)
    Operands.push_back(DSPICOperand::CreateToken("jmp", NameLoc));
  else {
    Operands.push_back(DSPICOperand::CreateToken("j", NameLoc));
    const MCExpr *CCode = MCConstantExpr::create(CondCode, getContext());
    Operands.push_back(DSPICOperand::CreateImm(CCode, SMLoc(), SMLoc()));
  }

  // Skip optional '$' sign.
  (void)parseOptionalToken(AsmToken::Dollar);

  const MCExpr *Val;
  SMLoc ExprLoc = getLexer().getLoc();
  if (getParser().parseExpression(Val))
    return Error(ExprLoc, "expected expression operand");

  int64_t Res;
  if (Val->evaluateAsAbsolute(Res))
    if (Res < -512 || Res > 511)
      return Error(ExprLoc, "invalid jump offset");

  Operands.push_back(DSPICOperand::CreateImm(Val, ExprLoc,
    getLexer().getLoc()));

  if (getLexer().isNot(AsmToken::EndOfStatement)) {
    SMLoc Loc = getLexer().getLoc();
    getParser().eatToEndOfStatement();
    return Error(Loc, "unexpected token");
  }

  getParser().Lex(); // Consume the EndOfStatement.
  return false;
}

bool DSPICAsmParser::parseInstruction(ParseInstructionInfo &Info,
                                       StringRef Name, SMLoc NameLoc,
                                       OperandVector &Operands) {
  // Drop .w suffix
  if (Name.ends_with_insensitive(".w"))
    Name = Name.drop_back(2);

  if (!parseJccInstruction(Info, Name, NameLoc, Operands))
    return false;

  // First operand is instruction mnemonic
  Operands.push_back(DSPICOperand::CreateToken(Name, NameLoc));

  // If there are no more operands, then finish
  if (getLexer().is(AsmToken::EndOfStatement))
    return false;

  // Parse first operand
  if (ParseOperand(Operands))
    return true;

  // Parse second operand if any
  if (parseOptionalToken(AsmToken::Comma) && ParseOperand(Operands))
    return true;

  if (getLexer().isNot(AsmToken::EndOfStatement)) {
    SMLoc Loc = getLexer().getLoc();
    getParser().eatToEndOfStatement();
    return Error(Loc, "unexpected token");
  }

  getParser().Lex(); // Consume the EndOfStatement.
  return false;
}

bool DSPICAsmParser::ParseDirectiveRefSym(AsmToken DirectiveID) {
  StringRef Name;
  if (getParser().parseIdentifier(Name))
    return TokError("expected identifier in directive");

  MCSymbol *Sym = getContext().getOrCreateSymbol(Name);
  getStreamer().emitSymbolAttribute(Sym, MCSA_Global);
  return parseEOL();
}

ParseStatus DSPICAsmParser::parseDirective(AsmToken DirectiveID) {
  StringRef IDVal = DirectiveID.getIdentifier();
  if (IDVal.lower() == ".long")
    return ParseLiteralValues(4, DirectiveID.getLoc());
  if (IDVal.lower() == ".word" || IDVal.lower() == ".short")
    return ParseLiteralValues(2, DirectiveID.getLoc());
  if (IDVal.lower() == ".byte")
    return ParseLiteralValues(1, DirectiveID.getLoc());
  if (IDVal.lower() == ".refsym")
    return ParseDirectiveRefSym(DirectiveID);
  return ParseStatus::NoMatch;
}

bool DSPICAsmParser::ParseOperand(OperandVector &Operands) {
  switch (getLexer().getKind()) {
    default: return true;
    case AsmToken::Identifier: {
      // try rN
      MCRegister RegNo;
      SMLoc StartLoc, EndLoc;
      if (!parseRegister(RegNo, StartLoc, EndLoc)) {
        Operands.push_back(DSPICOperand::CreateReg(RegNo, StartLoc, EndLoc));
        return false;
      }
      [[fallthrough]];
    }
    case AsmToken::Integer:
    case AsmToken::Plus:
    case AsmToken::Minus: {
      SMLoc StartLoc = getParser().getTok().getLoc();
      const MCExpr *Val;
      // Try constexpr[(rN)]
      if (!getParser().parseExpression(Val)) {
        MCRegister RegNo = DSPIC::PC;
        SMLoc EndLoc = getParser().getTok().getLoc();
        // Try (rN)
        if (parseOptionalToken(AsmToken::LParen)) {
          SMLoc RegStartLoc;
          if (parseRegister(RegNo, RegStartLoc, EndLoc))
            return true;
          EndLoc = getParser().getTok().getEndLoc();
          if (!parseOptionalToken(AsmToken::RParen))
            return true;
        }
        Operands.push_back(DSPICOperand::CreateMem(RegNo, Val, StartLoc,
          EndLoc));
        return false;
      }
      return true;
    }
    case AsmToken::Amp: {
      // Try &constexpr
      SMLoc StartLoc = getParser().getTok().getLoc();
      getLexer().Lex(); // Eat '&'
      const MCExpr *Val;
      if (!getParser().parseExpression(Val)) {
        SMLoc EndLoc = getParser().getTok().getLoc();
        Operands.push_back(DSPICOperand::CreateMem(DSPIC::SR, Val, StartLoc,
          EndLoc));
        return false;
      }
      return true;
    }
    case AsmToken::At: {
      // Try @rN[+]
      SMLoc StartLoc = getParser().getTok().getLoc();
      getLexer().Lex(); // Eat '@'
      MCRegister RegNo;
      SMLoc RegStartLoc, EndLoc;
      if (parseRegister(RegNo, RegStartLoc, EndLoc))
        return true;
      if (parseOptionalToken(AsmToken::Plus)) {
        Operands.push_back(DSPICOperand::CreatePostIndReg(RegNo, StartLoc, EndLoc));
        return false;
      }
      if (Operands.size() > 1) // Emulate @rd in destination position as 0(rd)
        Operands.push_back(DSPICOperand::CreateMem(RegNo,
            MCConstantExpr::create(0, getContext()), StartLoc, EndLoc));
      else
        Operands.push_back(DSPICOperand::CreateIndReg(RegNo, StartLoc, EndLoc));
      return false;
    }
    case AsmToken::Hash:
      // Try #constexpr
      SMLoc StartLoc = getParser().getTok().getLoc();
      getLexer().Lex(); // Eat '#'
      const MCExpr *Val;
      if (!getParser().parseExpression(Val)) {
        SMLoc EndLoc = getParser().getTok().getLoc();
        Operands.push_back(DSPICOperand::CreateImm(Val, StartLoc, EndLoc));
        return false;
      }
      return true;
  }
}

bool DSPICAsmParser::ParseLiteralValues(unsigned Size, SMLoc L) {
  auto parseOne = [&]() -> bool {
    const MCExpr *Value;
    if (getParser().parseExpression(Value))
      return true;
    getParser().getStreamer().emitValue(Value, Size, L);
    return false;
  };
  return (parseMany(parseOne));
}

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeDSPICAsmParser() {
  RegisterMCAsmParser<DSPICAsmParser> X(getTheDSPICTarget());
}

#define GET_REGISTER_MATCHER
#define GET_MATCHER_IMPLEMENTATION
#include "DSPICGenAsmMatcher.inc"

static MCRegister convertGR16ToGR8(MCRegister Reg) {
  switch (Reg.id()) {
  default:
    llvm_unreachable("Unknown GR16 register");
  case DSPIC::PC:  return DSPIC::PCB;
  case DSPIC::SP:  return DSPIC::SPB;
  case DSPIC::SR:  return DSPIC::SRB;
  case DSPIC::CG:  return DSPIC::CGB;
  case DSPIC::R4:  return DSPIC::R4B;
  case DSPIC::R5:  return DSPIC::R5B;
  case DSPIC::R6:  return DSPIC::R6B;
  case DSPIC::R7:  return DSPIC::R7B;
  case DSPIC::R8:  return DSPIC::R8B;
  case DSPIC::R9:  return DSPIC::R9B;
  case DSPIC::R10: return DSPIC::R10B;
  case DSPIC::R11: return DSPIC::R11B;
  case DSPIC::R12: return DSPIC::R12B;
  case DSPIC::R13: return DSPIC::R13B;
  case DSPIC::R14: return DSPIC::R14B;
  case DSPIC::R15: return DSPIC::R15B;
  }
}

unsigned DSPICAsmParser::validateTargetOperandClass(MCParsedAsmOperand &AsmOp,
                                                     unsigned Kind) {
  DSPICOperand &Op = static_cast<DSPICOperand &>(AsmOp);

  if (!Op.isReg())
    return Match_InvalidOperand;

  MCRegister Reg = Op.getReg();
  bool isGR16 = getDSPICMCRegisterClass(DSPIC::GR16RegClassID).contains(Reg);

  if (isGR16 && (Kind == MCK_GR8)) {
    Op.setReg(convertGR16ToGR8(Reg));
    return Match_Success;
  }

  return Match_InvalidOperand;
}
