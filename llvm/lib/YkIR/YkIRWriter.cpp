//===- YkIR/YkIRWRiter.cpp -- Yk JIT IR Serialiaser---------------------===//
//
// Converts an LLVM module into Yk's on-disk AOT IR.
//
//===-------------------------------------------------------------------===//

#include "llvm/BinaryFormat/ELF.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCSectionELF.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;
using namespace std;

namespace {

class SerialiseInstructionException {
private:
  string S;

public:
  SerialiseInstructionException(string S) : S(S) {}
  string &what() { return S; }
};

const char *SectionName = ".yk_ir";
const uint32_t Magic = 0xedd5f00d;
const uint32_t Version = 0;

enum OpCode {
  Nop = 0,
  Load,
  Store,
  Alloca,
  Call,
  Br,
  CondBr,
  ICmp,
  BinaryOperator,
  Ret,
  InsertValue,
  PtrAdd,
  Add,
  Sub,
  Mul,
  Or,
  And,
  Xor,
  Shl,
  AShr,
  FAdd,
  FDiv,
  FMul,
  FRem,
  FSub,
  LShr,
  SDiv,
  SRem,
  UDiv,
  URem,
  UnimplementedInstruction = 255, // YKFIXME: Will eventually be deleted.
};

enum OperandKind {
  Constant = 0,
  LocalVariable,
  Type,
  Function,
  Block,
  Arg,
  Global,
  Predicate,
  UnimplementedOperand = 255,
};

enum TypeKind {
  Void = 0,
  Integer,
  Ptr,
  FunctionTy,
  Struct,
  UnimplementedType = 255, // YKFIXME: Will eventually be deleted.
};

// A predicate used in a numeric comparison.
enum CmpPredicate {
  PredEqual = 0,
  PredNotEqual,
  PredUnsignedGreater,
  PredUnsignedGreaterEqual,
  PredUnsignedLess,
  PredUnsignedLessEqual,
  PredSignedGreater,
  PredSignedGreaterEqual,
  PredSignedLess,
  PredSignedLessEqual,
};

template <class T> string toString(T *X) {
  string S;
  raw_string_ostream SS(S);
  X->print(SS);
  return S;
}

// Get the index of an element in its parent container.
template <class C, class E> size_t getIndex(C *Container, E *FindElement) {
  bool Found = false;
  size_t Idx = 0;
  for (E &AnElement : *Container) {
    if (&AnElement == FindElement) {
      Found = true;
      break;
    }
    Idx++;
  }
  assert(Found);
  return Idx;
}

// A <BBIdx, InstrIdx> pair that Uniquely identifies an Yk IR instruction within
// a function.
using InstrLoc = std::tuple<size_t, size_t>;

// Maps an LLVM instruction that generates a value to the corresponding Yk IR
// instruction.
using ValueLoweringMap = map<Instruction *, InstrLoc>;

// The class responsible for serialising our IR into the interpreter binary.
//
// It walks over the LLVM IR, lowering each function, block, instruction, etc.
// into a Yk IR equivalent.
//
// As it does this there are some invariants that must be maintained:
//
//  - The current basic block index (BBIdx) is passed down the lowering process.
//    This must be incremented each time we finish a Yk IR basic block.
//
//  - Similarly for instructions. Each time we finish a Yk IR instruction,
//    we must increment the current instruction index (InstIdx).
//
//  - When we are done lowering an LLVM instruction that generates a value, we
//    must update the `VLMap` with an entry that maps the LLVM instruction to
//    the final Yk IR instruction in the lowering. If the LLVM instruction
//    doesn't generate a value, or the LLVM instruction lowered to exactly zero
//    Yk IR instructions, then there is no need to update the `VLMap`.
//
// These invariants are required so that when we encounter a local variable as
// an operand to an LLVM instruction, we can quickly find the corresponding Yk
// IR local variable.
class YkIRWriter {
private:
  Module &M;
  MCStreamer &OutStreamer;
  DataLayout DL;

  vector<llvm::Type *> Types;
  vector<llvm::Constant *> Constants;
  vector<llvm::GlobalVariable *> Globals;

  // Return the index of the LLVM type `Ty`, inserting a new entry if
  // necessary.
  size_t typeIndex(llvm::Type *Ty) {
    vector<llvm::Type *>::iterator Found =
        std::find(Types.begin(), Types.end(), Ty);
    if (Found != Types.end()) {
      return std::distance(Types.begin(), Found);
    }

    // Not found. Assign it a type index.
    size_t Idx = Types.size();
    Types.push_back(Ty);

    // If the newly-registered type is an aggregate type that contains other
    // types, then assign them type indices now too.
    for (llvm::Type *STy : Ty->subtypes()) {
      typeIndex(STy);
    }

    return Idx;
  }

  // Return the index of the LLVM constant `C`, inserting a new entry if
  // necessary.
  size_t constantIndex(class Constant *C) {
    vector<class Constant *>::iterator Found =
        std::find(Constants.begin(), Constants.end(), C);
    if (Found != Constants.end()) {
      return std::distance(Constants.begin(), Found);
    }
    size_t Idx = Constants.size();
    Constants.push_back(C);
    return Idx;
  }

  // Return the index of the LLVM global `G`, inserting a new entry if
  // necessary.
  size_t globalIndex(class GlobalVariable *G) {
    vector<class GlobalVariable *>::iterator Found =
        std::find(Globals.begin(), Globals.end(), G);
    if (Found != Globals.end()) {
      return std::distance(Globals.begin(), Found);
    }
    size_t Idx = Globals.size();
    Globals.push_back(G);
    return Idx;
  }

  size_t functionIndex(llvm::Function *F) {
    // FIXME: For now we assume that function indicies in LLVM IR and our IR
    // are the same.
    return getIndex(&M, F);
  }

  // Serialises a null-terminated string.
  void serialiseString(StringRef S) {
    OutStreamer.emitBinaryData(S);
    OutStreamer.emitInt8(0); // null terminator.
  }

  void serialiseOpcode(OpCode Code) { OutStreamer.emitInt8(Code); }

  void serialiseConstantOperand(Instruction *Parent, llvm::Constant *C) {
    OutStreamer.emitInt8(OperandKind::Constant);
    OutStreamer.emitSizeT(constantIndex(C));
  }

  void serialiseLocalVariableOperand(Instruction *I, ValueLoweringMap &VLMap) {
    auto [BBIdx, InstIdx] = VLMap.at(I);
    OutStreamer.emitInt8(OperandKind::LocalVariable);
    OutStreamer.emitSizeT(BBIdx);
    OutStreamer.emitSizeT(InstIdx);
  }

  void serialiseStringOperand(const char *S) {
    OutStreamer.emitInt8(OperandKind::UnimplementedOperand);
    serialiseString(S);
  }

  void serialiseFunctionOperand(llvm::Function *F) {
    OutStreamer.emitInt8(OperandKind::Function);
    OutStreamer.emitSizeT(functionIndex(F));
  }

  void serialiseBlockOperand(BasicBlock *BB, ValueLoweringMap &VLMap) {
    OutStreamer.emitInt8(OperandKind::Block);
    // FIXME: For now we assume that basic block indices are the same in LLVM
    // IR and our IR.
    OutStreamer.emitSizeT(getIndex(BB->getParent(), BB));
  }

  // YKFIXME: This allows programs which we haven't yet defined a
  // lowering for to compile. For now We just emit a string operand containing
  // the unhandled LLVM operand in textual form.
  void serialiseUnimplementedOperand(Value *V) {
    OutStreamer.emitInt8(OperandKind::UnimplementedOperand);
    serialiseString(toString(V));
  }

  void serialiseArgOperand(ValueLoweringMap &VLMap, Argument *A) {
    // This assumes that the argument indices match in both IRs.
    OutStreamer.emitInt8(OperandKind::Arg);
    OutStreamer.emitSizeT(A->getArgNo());
  }

  void serialiseGlobalOperand(GlobalVariable *G) {
    OutStreamer.emitInt8(OperandKind::Global);
    OutStreamer.emitSizeT(globalIndex(G));
  }

  void serialiseOperand(Instruction *Parent, ValueLoweringMap &VLMap,
                        Value *V) {
    if (llvm::GlobalVariable *G = dyn_cast<llvm::GlobalVariable>(V)) {
      serialiseGlobalOperand(G);
    } else if (llvm::Function *F = dyn_cast<llvm::Function>(V)) {
      serialiseFunctionOperand(F);
    } else if (llvm::Constant *C = dyn_cast<llvm::Constant>(V)) {
      serialiseConstantOperand(Parent, C);
    } else if (llvm::Argument *A = dyn_cast<llvm::Argument>(V)) {
      serialiseArgOperand(VLMap, A);
    } else if (Instruction *I = dyn_cast<Instruction>(V)) {
      // If an instruction defines the operand, it's a local variable.
      serialiseLocalVariableOperand(I, VLMap);
    } else if (BasicBlock *BB = dyn_cast<BasicBlock>(V)) {
      serialiseBlockOperand(BB, VLMap);
    } else {
      serialiseUnimplementedOperand(V);
    }
  }

  /// Does a naiave serialisation of an LLVM instruction by iterating over its
  /// operands and serialising them in turn.
  void serialiseInstGeneric(Instruction *I, ValueLoweringMap &VLMap,
                            unsigned BBIdx, unsigned &InstIdx, OpCode Opc) {
    OutStreamer.emitSizeT(typeIndex(I->getType()));
    serialiseOpcode(Opc);
    OutStreamer.emitInt32(I->getNumOperands());
    for (Value *O : I->operands()) {
      serialiseOperand(I, VLMap, O);
    }
    if (!I->getType()->isVoidTy()) {
      VLMap[I] = {BBIdx, InstIdx};
    }
    InstIdx++;
  }

  void serialiseBinaryOperation(llvm::BinaryOperator *I,
                                ValueLoweringMap &VLMap, unsigned BBIdx,
                                unsigned &InstIdx) {
    OutStreamer.emitSizeT(typeIndex(I->getType()));
    serialiseBinOpcode(I->getOpcode());
    OutStreamer.emitInt32(I->getNumOperands());
    for (Value *O : I->operands()) {
      serialiseOperand(I, VLMap, O);
    }
    VLMap[I] = {BBIdx, InstIdx};
    InstIdx++;
  }

  void serialiseBinOpcode(Instruction::BinaryOps BO) {
    switch (BO) {
    case Instruction::BinaryOps::Add:
      OutStreamer.emitInt8(OpCode::Add);
      break;
    case Instruction::BinaryOps::Sub:
      OutStreamer.emitInt8(OpCode::Sub);
      break;
    case Instruction::BinaryOps::Mul:
      OutStreamer.emitInt8(OpCode::Mul);
      break;
    case Instruction::BinaryOps::Or:
      OutStreamer.emitInt8(OpCode::Or);
      break;
    case Instruction::BinaryOps::And:
      OutStreamer.emitInt8(OpCode::And);
      break;
    case Instruction::BinaryOps::Xor:
      OutStreamer.emitInt8(OpCode::Xor);
      break;
    case Instruction::BinaryOps::Shl:
      OutStreamer.emitInt8(OpCode::Shl);
      break;
    case Instruction::BinaryOps::AShr:
      OutStreamer.emitInt8(OpCode::AShr);
      break;
    case Instruction::BinaryOps::FAdd:
      OutStreamer.emitInt8(OpCode::FAdd);
      break;
    case Instruction::BinaryOps::FDiv:
      OutStreamer.emitInt8(OpCode::FDiv);
      break;
    case Instruction::BinaryOps::FMul:
      OutStreamer.emitInt8(OpCode::FMul);
      break;
    case Instruction::BinaryOps::FRem:
      OutStreamer.emitInt8(OpCode::FRem);
      break;
    case Instruction::BinaryOps::FSub:
      OutStreamer.emitInt8(OpCode::FSub);
      break;
    case Instruction::BinaryOps::LShr:
      OutStreamer.emitInt8(OpCode::LShr);
      break;
    case Instruction::BinaryOps::SDiv:
      OutStreamer.emitInt8(OpCode::SDiv);
      break;
    case Instruction::BinaryOps::SRem:
      OutStreamer.emitInt8(OpCode::SRem);
      break;
    case Instruction::BinaryOps::UDiv:
      OutStreamer.emitInt8(OpCode::UDiv);
      break;
    case Instruction::BinaryOps::URem:
      OutStreamer.emitInt8(OpCode::URem);
      break;
    case Instruction::BinaryOps::BinaryOpsEnd:
      break;
    }
  }

  void serialiseAllocaInst(AllocaInst *I, ValueLoweringMap &VLMap,
                           unsigned BBIdx, unsigned &InstIdx) {
    // type_index:
    OutStreamer.emitSizeT(typeIndex(I->getType()));
    // opcode:
    serialiseOpcode(OpCode::Alloca);
    // num_operands:
    OutStreamer.emitInt32(2);

    // OPERAND 0: allocated type
    // Needs custom serialisation: not stored in the instruction's operand list.
    //
    // operand_kind:
    OutStreamer.emitInt8(OperandKind::Type);
    // type_index
    OutStreamer.emitSizeT(typeIndex(I->getAllocatedType()));

    // OPERAND 1: number of objects to allocate
    Value *Op0 = I->getOperand(0);
    assert(isa<ConstantInt>(Op0));
    serialiseOperand(I, VLMap, Op0);

    VLMap[I] = {BBIdx, InstIdx};
    InstIdx++;
  }

  void serialiseCallInst(CallInst *I, ValueLoweringMap &VLMap, unsigned BBIdx,
                         unsigned &InstIdx) {
    // type_index:
    OutStreamer.emitSizeT(typeIndex(I->getType()));
    // opcode:
    serialiseOpcode(OpCode::Call);
    // num_operands:
    unsigned NumOpers = I->getNumOperands();
    OutStreamer.emitInt32(NumOpers);

    // OPERAND 0: What to call.
    //
    // In LLVM IR this is the final operand, which is a cause of confusion.
    serialiseOperand(I, VLMap, I->getOperand(NumOpers - 1));

    // Now the rest of the operands.
    for (unsigned OI = 0; OI < NumOpers - 1; OI++) {
      serialiseOperand(I, VLMap, I->getOperand(OI));
    }

    if (!I->getType()->isVoidTy()) {
      VLMap[I] = {BBIdx, InstIdx};
    }
    InstIdx++;
  }

  void serialiseBranchInst(BranchInst *I, ValueLoweringMap &VLMap,
                           unsigned BBIdx, unsigned &InstIdx) {
    // We split LLVM's `br` into two Yk IR instructions: one for unconditional
    // branching, another for conidtional branching.
    if (!I->isConditional()) {
      // type_index:
      OutStreamer.emitSizeT(typeIndex(I->getType()));
      // opcode:
      serialiseOpcode(OpCode::Br);
      // num_operands:
      // We don't serialise any operands, because traces will guide us.
      OutStreamer.emitInt32(0);
    } else {
      // type_index:
      OutStreamer.emitSizeT(typeIndex(I->getType()));
      // opcode:
      serialiseOpcode(OpCode::CondBr);
      // We DO need operands for conditional branches, so that we can build
      // guards.
      //
      // Note that in LLVM IR, the operands are ordered (despite the order they
      // appear in the language reference): cond, if-false, if-true. We
      // use `getSuccessor()`, so as to re-order those during lowering to avoid
      // confusion.
      //
      // num_operands:
      OutStreamer.emitInt32(3);
      // OPERAND 0: condition.
      serialiseOperand(I, VLMap, I->getOperand(0));
      // OPERAND 1: block to go to if true.
      serialiseOperand(I, VLMap, I->getSuccessor(0));
      // OPERAND 2: block to go to if false.
      serialiseOperand(I, VLMap, I->getSuccessor(1));
    }
    InstIdx++;
  }

  void serialiseGetElementPtr(GetElementPtrInst *I, ValueLoweringMap &VLMap,
                              unsigned BBIdx, unsigned &InstIdx) {
    unsigned BitWidth = 64;
    MapVector<Value *, APInt> Offsets;
    APInt Offset(BitWidth, 0);

    bool Res = I->collectOffset(DL, BitWidth, Offsets, Offset);
    assert(Res);

    // type_index:
    OutStreamer.emitSizeT(typeIndex(I->getType()));
    // opcode:
    serialiseOpcode(OpCode::PtrAdd);
    // num_operands:
    OutStreamer.emitInt32(2);
    // pointer:
    serialiseOperand(I, VLMap, I->getPointerOperand());
    // offset:
    serialiseOperand(I, VLMap, ConstantInt::get(I->getContext(), Offset));

    VLMap[I] = {BBIdx, InstIdx};
    InstIdx++;
  }

  // Serialise an LLVM predicate.
  //
  // Note that this can't be handled by `serialiseOperand()` as in LLVM a
  // `Predicate` isn't a `Value`.
  void serialisePredicateOperand(llvm::CmpInst::Predicate P) {
    std::optional<CmpPredicate> LP = std::nullopt;
    switch (P) {
    case llvm::CmpInst::ICMP_EQ:
      LP = PredEqual;
      break;
    case llvm::CmpInst::ICMP_NE:
      LP = PredNotEqual;
      break;
    case llvm::CmpInst::ICMP_UGT:
      LP = PredUnsignedGreater;
      break;
    case llvm::CmpInst::ICMP_UGE:
      LP = PredUnsignedGreaterEqual;
      break;
    case llvm::CmpInst::ICMP_ULT:
      LP = PredUnsignedLess;
      break;
    case llvm::CmpInst::ICMP_ULE:
      LP = PredUnsignedLessEqual;
      break;
    case llvm::CmpInst::ICMP_SGT:
      LP = PredSignedGreater;
      break;
    case llvm::CmpInst::ICMP_SGE:
      LP = PredSignedGreaterEqual;
      break;
    case llvm::CmpInst::ICMP_SLT:
      LP = PredSignedLess;
      break;
    case llvm::CmpInst::ICMP_SLE:
      LP = PredSignedLessEqual;
      break;
    default:
      abort(); // TODO: floating point predicates.
    }
    OutStreamer.emitInt8(OperandKind::Predicate);
    OutStreamer.emitInt8(LP.value());
  }

  // We use a custom lowering for ICmp, as a generic lowering misses
  // the predicate.
  void serialiseICmpInst(ICmpInst *I, ValueLoweringMap &VLMap, unsigned BBIdx,
                         unsigned &InstIdx) {
    // type_index:
    OutStreamer.emitSizeT(typeIndex(I->getType()));
    // opcode:
    serialiseOpcode(OpCode::ICmp);
    // num_operands:
    OutStreamer.emitInt32(3);
    // op1:
    serialiseOperand(I, VLMap, I->getOperand(0));
    // predicate:
    serialisePredicateOperand(I->getPredicate());
    // op2:
    serialiseOperand(I, VLMap, I->getOperand(1));

    VLMap[I] = {BBIdx, InstIdx};
    InstIdx++;
  }

  void serialiseInst(Instruction *I, ValueLoweringMap &VLMap, unsigned BBIdx,
                     unsigned &InstIdx) {
// Macros to help dispatch to serialisers.
//
// Note that this is unhygenic so as to make the call-sites readable.
#define GENERIC_INST_SERIALISE(LLVM_INST, LLVM_INST_TYPE, YKIR_OPCODE)         \
  if (isa<LLVM_INST_TYPE>(LLVM_INST)) {                                        \
    serialiseInstGeneric(LLVM_INST, VLMap, BBIdx, InstIdx, YKIR_OPCODE);       \
    return;                                                                    \
  }
#define CUSTOM_INST_SERIALISE(LLVM_INST, LLVM_INST_TYPE, SERIALISER)           \
  if (LLVM_INST_TYPE *II = dyn_cast<LLVM_INST_TYPE>(LLVM_INST)) {              \
    SERIALISER(II, VLMap, BBIdx, InstIdx);                                     \
    return;                                                                    \
  }

    GENERIC_INST_SERIALISE(I, LoadInst, Load)
    GENERIC_INST_SERIALISE(I, StoreInst, Store)
    GENERIC_INST_SERIALISE(I, ReturnInst, Ret)
    GENERIC_INST_SERIALISE(I, llvm::InsertValueInst, InsertValue)
    GENERIC_INST_SERIALISE(I, StoreInst, Store)

    CUSTOM_INST_SERIALISE(I, AllocaInst, serialiseAllocaInst)
    CUSTOM_INST_SERIALISE(I, CallInst, serialiseCallInst)
    CUSTOM_INST_SERIALISE(I, BranchInst, serialiseBranchInst)
    CUSTOM_INST_SERIALISE(I, GetElementPtrInst, serialiseGetElementPtr)
    CUSTOM_INST_SERIALISE(I, llvm::BinaryOperator, serialiseBinaryOperation)
    CUSTOM_INST_SERIALISE(I, ICmpInst, serialiseICmpInst)

    // GENERIC_INST_SERIALISE and CUSTOM_INST_SERIALISE do an early return upon
    // a match, so if we get here then the instruction wasn't handled.
    serialiseUnimplementedInstruction(I, VLMap, BBIdx, InstIdx);
  }

  // An unimplemented instruction is lowered to an instruction with one
  // unimplemented operand containing the textual LLVM IR we couldn't handle.
  void serialiseUnimplementedInstruction(Instruction *I,
                                         ValueLoweringMap &VLMap,
                                         unsigned BBIdx, unsigned &InstIdx) {
    // type_index:
    OutStreamer.emitSizeT(typeIndex(I->getType()));
    // opcode:
    serialiseOpcode(UnimplementedInstruction);
    // num_operands:
    OutStreamer.emitInt32(1);
    // problem instruction:
    serialiseUnimplementedOperand(I);

    if (!I->getType()->isVoidTy()) {
      VLMap[I] = {BBIdx, InstIdx};
    }
    InstIdx++;
  }

  void serialiseBlock(BasicBlock &BB, ValueLoweringMap &VLMap,
                      unsigned &BBIdx) {
    // Keep the instruction skipping logic in one place.
    auto ShouldSkipInstr = [](Instruction *I) {
      // Skip non-semantic instrucitons for now.
      //
      // We may come back to them later if we need better debugging
      // facilities, but for now they just clutter up our AOT module.
      return I->isDebugOrPseudoInst();
    };

    // Count instructions.
    //
    // FIXME: I don't like this much:
    //
    //  - Assumes one LLVM instruction becomes exactly one Yk IR instruction.
    //  - Requires a second loop to count ahead of time.
    //
    // Can we emit the instrucitons into a temp buffer and keep a running count
    // of how many instructions we generated instead?
    size_t NumInstrs = 0;
    for (Instruction &I : BB) {
      if (ShouldSkipInstr(&I)) {
        continue;
      }
      NumInstrs++;
    }

    // num_instrs:
    OutStreamer.emitSizeT(NumInstrs);
    // instrs:
    unsigned InstIdx = 0;
    for (Instruction &I : BB) {
      if (ShouldSkipInstr(&I)) {
        continue;
      }
      serialiseInst(&I, VLMap, BBIdx, InstIdx);
    }

    // Check we emitted the number of instructions that we promised.
    assert(InstIdx == NumInstrs);

    BBIdx++;
  }

  void serialiseArg(Argument *A) {
    // type_index:
    OutStreamer.emitSizeT(typeIndex(A->getType()));
  }

  void serialiseFunc(llvm::Function &F) {
    // name:
    serialiseString(F.getName());
    // type_idx:
    OutStreamer.emitSizeT(typeIndex(F.getFunctionType()));
    // num_blocks:
    OutStreamer.emitSizeT(F.size());
    // blocks:
    unsigned BBIdx = 0;
    ValueLoweringMap VLMap;
    for (BasicBlock &BB : F) {
      serialiseBlock(BB, VLMap, BBIdx);
    }
  }

  void serialiseFunctionType(FunctionType *Ty) {
    OutStreamer.emitInt8(TypeKind::FunctionTy);
    // num_args:
    OutStreamer.emitSizeT(Ty->getNumParams());
    // arg_tys:
    for (llvm::Type *SubTy : Ty->params()) {
      OutStreamer.emitSizeT(typeIndex(SubTy));
    }
    // ret_ty:
    OutStreamer.emitSizeT(typeIndex(Ty->getReturnType()));
    // is_vararg:
    OutStreamer.emitInt8(Ty->isVarArg());
  }

  void serialiseStructType(StructType *STy) {
    OutStreamer.emitInt8(TypeKind::Struct);
    unsigned NumFields = STy->getNumElements();
    DataLayout DL(&M);
    const StructLayout *SL = DL.getStructLayout(STy);
    // num_fields:
    OutStreamer.emitSizeT(NumFields);
    // field_tys:
    for (unsigned I = 0; I < NumFields; I++) {
      OutStreamer.emitSizeT(typeIndex(STy->getElementType(I)));
    }
    // field_bit_offs:
    for (unsigned I = 0; I < NumFields; I++) {
      OutStreamer.emitSizeT(SL->getElementOffsetInBits(I));
    }
  }

  void serialiseType(llvm::Type *Ty) {
    if (Ty->isVoidTy()) {
      OutStreamer.emitInt8(TypeKind::Void);
    } else if (PointerType *PT = dyn_cast<PointerType>(Ty)) {
      // FIXME: The Yk runtime assumes all pointers are void-ptr-sized.
      assert(DL.getPointerSize(PT->getAddressSpace()) == sizeof(void *));
      OutStreamer.emitInt8(TypeKind::Ptr);
    } else if (IntegerType *ITy = dyn_cast<IntegerType>(Ty)) {
      OutStreamer.emitInt8(TypeKind::Integer);
      OutStreamer.emitInt32(ITy->getBitWidth());
    } else if (FunctionType *FTy = dyn_cast<FunctionType>(Ty)) {
      serialiseFunctionType(FTy);
    } else if (StructType *STy = dyn_cast<StructType>(Ty)) {
      serialiseStructType(STy);
    } else {
      OutStreamer.emitInt8(TypeKind::UnimplementedType);
      serialiseString(toString(Ty));
    }
  }

  void serialiseConstantInt(ConstantInt *CI) {
    OutStreamer.emitSizeT(typeIndex(CI->getType()));
    OutStreamer.emitSizeT(CI->getBitWidth() / 8);
    for (size_t I = 0; I < CI->getBitWidth(); I += 8) {
      uint64_t Byte = CI->getValue().extractBitsAsZExtValue(8, I);
      OutStreamer.emitInt8(Byte);
    }
  }

  void serialiseUnimplementedConstant(class Constant *C) {
    // type_index:
    OutStreamer.emitSizeT(typeIndex(C->getType()));
    // num_bytes:
    // Just report zero for now.
    OutStreamer.emitSizeT(0);
  }

  void serialiseConstant(class Constant *C) {
    if (ConstantInt *CI = dyn_cast<ConstantInt>(C)) {
      serialiseConstantInt(CI);
    } else {
      serialiseUnimplementedConstant(C);
    }
  }

  void serialiseGlobal(class GlobalVariable *G) {
    OutStreamer.emitInt8(G->isThreadLocal());
    serialiseString(G->getName());
  }

public:
  YkIRWriter(Module &M, MCStreamer &OutStreamer)
      : M(M), OutStreamer(OutStreamer), DL(&M) {}

  // Entry point for IR serialisation.
  //
  // The order of serialisation matters.
  //
  // - Serialising functions can introduce new types and constants.
  // - Serialising constants can introduce new types.
  //
  // So we must serialise functions, then constants, then types.
  void serialise() {
    // header:
    OutStreamer.emitInt32(Magic);
    OutStreamer.emitInt32(Version);

    // num_funcs:
    OutStreamer.emitSizeT(M.size());
    // funcs:
    for (llvm::Function &F : M) {
      serialiseFunc(F);
    }

    // num_constants:
    OutStreamer.emitSizeT(Constants.size());
    // constants:
    for (class Constant *&C : Constants) {
      serialiseConstant(C);
    }

    // num_globals:
    OutStreamer.emitSizeT(Globals.size());
    // globals:
    for (class GlobalVariable *&G : Globals) {
      serialiseGlobal(G);
    }

    // Now that we've finished serialising globals, add a global (immutable, for
    // now) array to the LLVM module containing pointers to all the global
    // variables. We will use this to find the addresses of globals at runtime.
    // The indices of the array correspond with `GlobalDeclIdx`s in the AOT IR.
    vector<llvm::Constant *> GlobalsAsConsts;
    for (llvm::GlobalVariable *G : Globals) {
      GlobalsAsConsts.push_back(cast<llvm::Constant>(G));
    }
    ArrayType *GlobalsArrayTy =
        ArrayType::get(PointerType::get(M.getContext(), 0), Globals.size());
    GlobalVariable *GlobalsArray = new GlobalVariable(
        M, GlobalsArrayTy, true, GlobalValue::LinkageTypes::ExternalLinkage,
        ConstantArray::get(GlobalsArrayTy, GlobalsAsConsts));
    GlobalsArray->setName("__yk_globalvar_ptrs");
    IntegerType *Int64Ty = Type::getInt64Ty(M.getContext());
    GlobalVariable *GlobalsArrayLen = new GlobalVariable(
        M, Int64Ty, true, GlobalValue::LinkageTypes::ExternalLinkage,
        ConstantInt::get(Int64Ty, Globals.size()));
    GlobalsArrayLen->setName("__yk_globalvar_len");

    // num_types:
    OutStreamer.emitSizeT(Types.size());
    // types:
    for (llvm::Type *&Ty : Types) {
      serialiseType(Ty);
    }
  }
};
} // anonymous namespace

// Create an ELF section for storing Yk IR into.
MCSection *createYkIRSection(MCContext &Ctx, const MCSection *TextSec) {
  if (Ctx.getObjectFileType() != MCContext::IsELF)
    return nullptr;

  const MCSectionELF *ElfSec = static_cast<const MCSectionELF *>(TextSec);
  unsigned Flags = ELF::SHF_LINK_ORDER;
  StringRef GroupName;

  // Ensure the loader loads it.
  Flags |= ELF::SHF_ALLOC;

  return Ctx.getELFSection(SectionName, ELF::SHT_LLVM_BB_ADDR_MAP, Flags, 0,
                           GroupName, true, ElfSec->getUniqueID(),
                           cast<MCSymbolELF>(TextSec->getBeginSymbol()));
}

// Emit a start/end IR marker.
//
// The JIT uses a start and end marker to make a Rust slice of the IR.
void emitStartOrEndSymbol(MCContext &MCtxt, MCStreamer &OutStreamer,
                          bool Start) {
  std::string SymName("ykllvm.yk_ir.");
  if (Start)
    SymName.append("start");
  else
    SymName.append("stop");

  MCSymbol *Sym = MCtxt.getOrCreateSymbol(SymName);
  OutStreamer.emitSymbolAttribute(Sym, llvm::MCSA_Global);
  OutStreamer.emitLabel(Sym);
}

namespace llvm {

// Emit Yk IR into the resulting ELF binary.
void embedYkIR(MCContext &Ctx, MCStreamer &OutStreamer, Module &M) {
  MCSection *YkIRSec =
      createYkIRSection(Ctx, std::get<0>(OutStreamer.getCurrentSection()));

  OutStreamer.pushSection();
  OutStreamer.switchSection(YkIRSec);
  emitStartOrEndSymbol(Ctx, OutStreamer, true);
  YkIRWriter(M, OutStreamer).serialise();
  emitStartOrEndSymbol(Ctx, OutStreamer, false);
  OutStreamer.popSection();
}
} // namespace llvm
