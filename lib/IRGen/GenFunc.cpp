//===--- GenFunc.cpp - Swift IR Generation for Function Types -------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  This file implements IR generation for function types in Swift.  This
//  includes creating the IR type as well as capturing variables and
//  performing calls.
//
//  Swift function types are always expanded as a struct containing
//  two opaque pointers.  The first pointer is to a function (should
//  this be a descriptor?) to which the second pointer is passed,
//  along with the formal arguments.  The function pointer is opaque
//  because the alternative would require infinite types to faithfully
//  represent, since aggregates containing function types can be
//  passed and returned by value, not necessary as first-class
//  aggregates.
//
//  There are several considerations for whether to pass the data
//  pointer as the first argument or the last:
//    - On CCs that pass anything in registers, dropping the last
//      argument is significantly more efficient than dropping the
//      first, and it's not that unlikely that the data might
//      be ignored.
//    - A specific instance of that:  we can use the address of a
//      global "data-free" function directly when taking an
//      address-of-function.
//    - Replacing a pointer argument with a different pointer is
//      quite efficient with pretty much any CC.
//    - Later arguments can be less efficient to access if they
//      actually get passed on the stack, but there's some leeway
//      with a decent CC.
//    - Passing the data pointer last inteferes with native variadic
//      arguments, but we probably don't ever want to use native
//      variadic arguments.
//  This works out to a pretty convincing argument for passing the
//  data pointer as the last argument.
//
//  On the other hand, it is not compatible with blocks.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/ASTContext.h"
#include "swift/AST/Builtins.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Expr.h"
#include "swift/AST/Module.h"
#include "swift/AST/Types.h"
#include "swift/Basic/Optional.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Function.h"
#include "llvm/Target/TargetData.h"

#include "GenType.h"
#include "IRGenFunction.h"
#include "IRGenModule.h"
#include "LValue.h"
#include "RValue.h"
#include "Explosion.h"

using namespace swift;
using namespace irgen;

namespace {
  class FuncTypeInfo : public TypeInfo {
    FunctionType *FnTy;
    mutable llvm::FunctionType *FunctionTypeWithData;
    mutable llvm::FunctionType *FunctionTypeWithoutData;
  public:
    FuncTypeInfo(FunctionType *Ty, llvm::StructType *T, Size S, Alignment A)
      : TypeInfo(T, S, A), FnTy(Ty),
        FunctionTypeWithData(0), FunctionTypeWithoutData(0) {}

    llvm::StructType *getStorageType() const {
      return cast<llvm::StructType>(TypeInfo::getStorageType());
    }

    llvm::FunctionType *getFunctionType(IRGenModule &IGM, bool NeedsData) const;

    RValueSchema getSchema() const {
      llvm::StructType *Ty = getStorageType();
      assert(Ty->getNumElements() == 2);
      return RValueSchema::forScalars(Ty->getElementType(0),
                                      Ty->getElementType(1));
    }

    RValue load(IRGenFunction &IGF, Address address) const {
      llvm::Value *addr = address.getAddress();

      // Load the function.
      llvm::Value *fnAddr =
        IGF.Builder.CreateStructGEP(addr, 0, addr->getName() + ".fn");
      llvm::LoadInst *fn =
        IGF.Builder.CreateLoad(fnAddr, address.getAlignment(),
                               fnAddr->getName() + ".load");

      // Load the data.  This load is offset by sizeof(void*) from the
      // base and so may have a lesser alignment.
      // FIXME: retains?
      llvm::Value *dataAddr =
        IGF.Builder.CreateStructGEP(addr, 1, addr->getName() + ".data");
      llvm::Value *data =
        IGF.Builder.CreateLoad(dataAddr,
                               address.getAlignment().alignmentAtOffset(
                                          Size(StorageAlignment.getValue())),
                               dataAddr->getName() + ".load");

      return RValue::forScalars(fn, data);
    }

    void store(IRGenFunction &IGF, const RValue &RV, Address address) const {
      assert(RV.isScalar() && RV.getScalars().size() == 2);
      llvm::Value *addr = address.getAddress();

      // Store the function pointer.
      llvm::Value *fnAddr =
        IGF.Builder.CreateStructGEP(addr, 0, addr->getName() + ".fn");
      IGF.Builder.CreateStore(RV.getScalars()[0], fnAddr,
                              address.getAlignment());

      // Store the data.
      // FIXME: retains?
      llvm::Value *dataAddr =
        IGF.Builder.CreateStructGEP(addr, 1, addr->getName() + ".data");
      IGF.Builder.CreateStore(RV.getScalars()[1], dataAddr,
                              address.getAlignment().alignmentAtOffset(
                                         Size(StorageAlignment.getValue())));
    }

    unsigned getExplosionSize(ExplosionKind kind) const {
      return 2;
    }

    void getExplosionSchema(ExplosionSchema &schema) const {
      llvm::StructType *Ty = getStorageType();
      assert(Ty->getNumElements() == 2);
      schema.add(ExplosionSchema::Element::forScalar(Ty->getElementType(0)));
      schema.add(ExplosionSchema::Element::forScalar(Ty->getElementType(1)));
    }

    void loadExplosion(IRGenFunction &IGF, Address addr, Explosion &e) const {
      RValue rv = load(IGF, addr);
      e.add(rv.getScalars());
    }

    void storeExplosion(IRGenFunction &IGF, Explosion &e, Address addr) const {
      llvm::Value *func = e.claimNext();
      llvm::Value *data = e.claimNext();
      store(IGF, RValue::forScalars(func, data), addr);
    }
  };
}

const TypeInfo *
TypeConverter::convertFunctionType(IRGenModule &IGM, FunctionType *T) {
  Size StructSize = Size(IGM.TargetData.getPointerSize()) * 2;
  Alignment StructAlign = Alignment(IGM.TargetData.getPointerABIAlignment());
  llvm::Type *Elts[] = { IGM.Int8PtrTy, IGM.Int8PtrTy };
  llvm::StructType *StructType
    = llvm::StructType::get(IGM.getLLVMContext(), Elts, /*packed*/ false);
  return new FuncTypeInfo(T, StructType, StructSize, StructAlign);
}

/// Accumulate an argument of the given type.
static void addArgType(IRGenModule &IGM, Type Ty,
                       SmallVectorImpl<llvm::Type*> &ArgTypes) {
  RValueSchema Schema = IGM.getFragileTypeInfo(Ty).getSchema();
  if (Schema.isScalar()) {
    for (llvm::Type *Arg : Schema.getScalarTypes())
      ArgTypes.push_back(Arg);
  } else {
    ArgTypes.push_back(Schema.getAggregateType()->getPointerTo());
  }
}

llvm::FunctionType *
FuncTypeInfo::getFunctionType(IRGenModule &IGM, bool NeedsData) const {
  if (NeedsData && FunctionTypeWithData)
    return FunctionTypeWithData;
  if (!NeedsData && FunctionTypeWithoutData)
    return FunctionTypeWithoutData;

  SmallVector<llvm::Type*, 16> ArgTypes;
  llvm::Type *ResultType;

  // Compute the result-type information.
  RValueSchema ResultSchema = IGM.getFragileTypeInfo(FnTy->Result).getSchema();

  // If this is an aggregate return, return indirectly.
  if (ResultSchema.isAggregate()) {
    ResultType = llvm::Type::getVoidTy(IGM.getLLVMContext());
    ArgTypes.push_back(ResultSchema.getAggregateType()->getPointerTo());

  // If there are no results, return void.
  } else if (ResultSchema.getScalarTypes().empty()) {
    ResultType = llvm::Type::getVoidTy(IGM.getLLVMContext());

  // If there is exactly one result, return it.
  } else if (ResultSchema.getScalarTypes().size() == 1) {
    ResultType = ResultSchema.getScalarTypes()[0];

  // Otherwise, return a first-class aggregate.
  } else {
    ResultType = llvm::StructType::get(IGM.getLLVMContext(),
                                       ResultSchema.getScalarTypes());
  }

  // Drill into the first level of tuple, if present.
  if (TupleType *Tuple = FnTy->Input->getAs<TupleType>()) {
    for (const TupleTypeElt &Field : Tuple->Fields) {
      addArgType(IGM, Field.Ty, ArgTypes);
    }

  // Otherwise, just add the argument type.
  } else {
    addArgType(IGM, FnTy->Input, ArgTypes);
  }

  // If we need a data argument, add it in last.
  // See the discussion in the header comment, above.
  if (NeedsData) {
    ArgTypes.push_back(IGM.Int8PtrTy);
  }

  // Create the appropriate LLVM type.
  llvm::FunctionType *IRType =
    llvm::FunctionType::get(ResultType, ArgTypes, /*variadic*/ false);

  // Cache the type.
  if (NeedsData)
    FunctionTypeWithData = IRType;
  else
    FunctionTypeWithoutData = IRType;

  return IRType;
}

/// Form an r-value which refers to the given global function.
void IRGenFunction::emitExplodedRValueForFunction(FuncDecl *Fn,
                                                  Explosion &explosion) {
  if (!Fn->getDeclContext()->isLocalContext()) {
    explosion.add(IGM.getAddrOfGlobalFunction(Fn));
    explosion.add(llvm::UndefValue::get(IGM.Int8PtrTy));
    return;
  }

  unimplemented(Fn->getLocStart(), "local function emission");
  llvm::Value *undef = llvm::UndefValue::get(IGM.Int8PtrTy);
  explosion.add(undef);
  explosion.add(undef);
  return;
}

llvm::FunctionType *IRGenModule::getFunctionType(Type type, bool withData) {
  const FuncTypeInfo &fnTypeInfo = getFragileTypeInfo(type).as<FuncTypeInfo>();
  return fnTypeInfo.getFunctionType(*this, withData);
}

namespace {
  struct ArgList {
    ArgList(ExplosionKind kind) : Values(kind) {}

    Explosion Values;
    llvm::SmallVector<llvm::AttributeWithIndex, 4> Attrs;

    void addArg(const RValue &arg) {
      if (arg.isScalar()) {
        Values.add(arg.getScalars());
      } else {
        Values.add(arg.getAggregateAddress());
      }
    }
  };
}

/// emitBuiltinCall - Emit a call to a builtin function.
static RValue emitBuiltinCall(IRGenFunction &IGF, FuncDecl *Fn, Expr *Arg,
                              const TypeInfo &resultType) {
  assert(resultType.getSchema().isScalar() && "builtin type with agg return");

  // Emit the arguments.  Maybe we'll get builtins that are more
  // complex than this.
  ArgList args(ExplosionKind::Minimal);
  IGF.emitExplodedRValue(Arg, args.Values);

  Type BuiltinType;
  switch (isBuiltinValue(IGF.IGM.Context, Fn->getName().str(), BuiltinType)) {
  case BuiltinValueKind::None: llvm_unreachable("not a builtin after all!");

/// A macro which expands to the emission of a simple unary operation
/// or predicate.
#define UNARY_OPERATION(Op) {                                               \
    llvm::Value *op = args.Values.claimNext();                              \
    assert(args.Values.empty() && "wrong operands to unary operation");     \
    return RValue::forScalars(IGF.Builder.Create##Op(op));                  \
  }

/// A macro which expands to the emission of a simple binary operation
/// or predicate.
#define BINARY_OPERATION(Op) {                                              \
    llvm::Value *lhs = args.Values.claimNext();                             \
    llvm::Value *rhs = args.Values.claimNext();                             \
    assert(args.Values.empty() && "wrong operands to binary operation");    \
    return RValue::forScalars(IGF.Builder.Create##Op(lhs, rhs));            \
  }

/// A macro which expands to the emission of a simple binary operation
/// or predicate defined over both floating-point and integer types.
#define BINARY_ARITHMETIC_OPERATION(IntOp, FPOp) {                          \
    llvm::Value *lhs = args.Values.claimNext();                             \
    llvm::Value *rhs = args.Values.claimNext();                             \
    assert(args.Values.empty() && "wrong operands to binary operation");    \
    if (lhs->getType()->isFloatingPointTy()) {                              \
      return RValue::forScalars(IGF.Builder.Create##FPOp(lhs, rhs));        \
    } else {                                                                \
      return RValue::forScalars(IGF.Builder.Create##IntOp(lhs, rhs));       \
    }                                                                       \
  }

  case BuiltinValueKind::Neg:       UNARY_OPERATION(Neg)
  case BuiltinValueKind::Not:       UNARY_OPERATION(Not)
  case BuiltinValueKind::Add:       BINARY_ARITHMETIC_OPERATION(Add, FAdd)
  case BuiltinValueKind::And:       BINARY_OPERATION(And)
  case BuiltinValueKind::FDiv:      BINARY_OPERATION(FDiv)
  case BuiltinValueKind::Mul:       BINARY_ARITHMETIC_OPERATION(Mul, FMul)
  case BuiltinValueKind::Or:        BINARY_OPERATION(Or)
  case BuiltinValueKind::SDiv:      BINARY_OPERATION(SDiv)
  case BuiltinValueKind::SDivExact: BINARY_OPERATION(ExactSDiv)
  case BuiltinValueKind::SRem:      BINARY_OPERATION(SRem)
  case BuiltinValueKind::Sub:       BINARY_ARITHMETIC_OPERATION(Sub, FSub)
  case BuiltinValueKind::UDiv:      BINARY_OPERATION(UDiv)
  case BuiltinValueKind::UDivExact: BINARY_OPERATION(ExactUDiv)
  case BuiltinValueKind::URem:      BINARY_OPERATION(URem)
  case BuiltinValueKind::Xor:       BINARY_OPERATION(Xor)
  case BuiltinValueKind::CmpEQ:     BINARY_OPERATION(ICmpEQ)
  case BuiltinValueKind::CmpNE:     BINARY_OPERATION(ICmpNE)
  case BuiltinValueKind::CmpSLE:    BINARY_OPERATION(ICmpSLE)
  case BuiltinValueKind::CmpSLT:    BINARY_OPERATION(ICmpSLT)
  case BuiltinValueKind::CmpSGE:    BINARY_OPERATION(ICmpSGE)
  case BuiltinValueKind::CmpSGT:    BINARY_OPERATION(ICmpSGT)
  case BuiltinValueKind::CmpULE:    BINARY_OPERATION(ICmpULE)
  case BuiltinValueKind::CmpULT:    BINARY_OPERATION(ICmpULT)
  case BuiltinValueKind::CmpUGE:    BINARY_OPERATION(ICmpUGE)
  case BuiltinValueKind::CmpUGT:    BINARY_OPERATION(ICmpUGT)
  case BuiltinValueKind::FCmpOEQ:   BINARY_OPERATION(FCmpOEQ)
  case BuiltinValueKind::FCmpOGT:   BINARY_OPERATION(FCmpOGT)
  case BuiltinValueKind::FCmpOGE:   BINARY_OPERATION(FCmpOGE)
  case BuiltinValueKind::FCmpOLT:   BINARY_OPERATION(FCmpOLT)
  case BuiltinValueKind::FCmpOLE:   BINARY_OPERATION(FCmpOLE)
  case BuiltinValueKind::FCmpONE:   BINARY_OPERATION(FCmpONE)
  case BuiltinValueKind::FCmpORD:   BINARY_OPERATION(FCmpORD)
  case BuiltinValueKind::FCmpUEQ:   BINARY_OPERATION(FCmpUEQ)
  case BuiltinValueKind::FCmpUGT:   BINARY_OPERATION(FCmpUGT)
  case BuiltinValueKind::FCmpUGE:   BINARY_OPERATION(FCmpUGE)
  case BuiltinValueKind::FCmpULT:   BINARY_OPERATION(FCmpULT)
  case BuiltinValueKind::FCmpULE:   BINARY_OPERATION(FCmpULE)
  case BuiltinValueKind::FCmpUNE:   BINARY_OPERATION(FCmpUNE)
  case BuiltinValueKind::FCmpUNO:   BINARY_OPERATION(FCmpUNO)
  }
  llvm_unreachable("bad builtin kind!");
}

void IRGenFunction::emitExplodedApplyExpr(ApplyExpr *E, Explosion &explosion) {
  const TypeInfo &type = getFragileTypeInfo(E->getType());
  RValue rvalue = emitApplyExpr(E, type);
  return type.explode(*this, rvalue, explosion);
}

Optional<Address>
IRGenFunction::tryEmitApplyAsAddress(ApplyExpr *E, const TypeInfo &resultType) {
  RValueSchema resultSchema = resultType.getSchema();
  if (!resultSchema.isAggregate())
    return Nothing;

  RValue result = emitApplyExpr(E, resultType);
  assert(result.isAggregate());
  return Address(result.getAggregateAddress(), resultType.StorageAlignment);
}

/// Emit a function call.
RValue IRGenFunction::emitApplyExpr(ApplyExpr *E, const TypeInfo &resultType) {
  // Check for a call to a builtin.
  if (ValueDecl *Fn = E->getCalledValue())
    if (Fn->getDeclContext() == IGM.Context.TheBuiltinModule)
      return emitBuiltinCall(*this, cast<FuncDecl>(Fn), E->getArg(),
                             resultType);

  Explosion fnValues(ExplosionKind::Maximal);
  emitExplodedRValue(E->getFn(), fnValues);
  llvm::Value *fn = fnValues.claimNext();
  llvm::Value *data = fnValues.claimNext();
  assert(fnValues.empty());

  // Unless special-cased, calls are done with minimal explosion.
  // TODO: detect special cases.
  ArgList args(ExplosionKind::Minimal);

  // The first argument is the implicit aggregate return slot, if required.
  RValueSchema resultSchema = resultType.getSchema();
  if (resultSchema.isAggregate()) {
    Address resultSlot =
      createFullExprAlloca(resultSchema.getAggregateType(),
                           resultSchema.getAggregateAlignment(),
                           "call.aggresult");
    args.Values.add(resultSlot.getAddress());
    args.Attrs.push_back(llvm::AttributeWithIndex::get(1,
                                llvm::Attribute::StructRet |
                                llvm::Attribute::NoAlias));
  }

  // Emit the arguments, drilling into the first level of tuple, if
  // present.
  emitExplodedRValue(E->getArg(), args.Values);

  // Don't bother passing a data argument if the r-value says it's
  // undefined.
  bool needsData = !isa<llvm::UndefValue>(data);
  if (needsData) args.Values.add(data);

  const FuncTypeInfo &fnType =
    IGM.getFragileTypeInfo(E->getFn()->getType()).as<FuncTypeInfo>();
  llvm::FunctionType *fnLLVMType = fnType.getFunctionType(IGM, needsData);

  fn = Builder.CreateBitCast(fn, fnLLVMType->getPointerTo(), "fn.cast");

  // TODO: exceptions, calling conventions
  llvm::CallInst *call = Builder.CreateCall(fn, args.Values.getAll());
  call->setAttributes(llvm::AttrListPtr::get(args.Attrs.data(),
                                             args.Attrs.size()));

  // Build an RValue result.
  if (resultSchema.isAggregate()) {
    return RValue::forAggregate(args.Values.claimNext());
  } else if (resultSchema.getScalarTypes().size() == 1) {
    return RValue::forScalars(call);
  } else {
    // This does the right thing for void returns as well.
    llvm::SmallVector<llvm::Value*, RValue::MaxScalars> result;
    for (unsigned I = 0, E = resultSchema.getScalarTypes().size(); I != E; ++I){
      llvm::Value *scalar = Builder.CreateExtractValue(call, I);
      result.push_back(scalar);
    }
    return RValue::forScalars(result);
  }
}

/// Emit the prologue for the function.
void IRGenFunction::emitPrologue() {
  // Set up the IRBuilder.
  llvm::BasicBlock *EntryBB = createBasicBlock("entry");
  assert(CurFn->getBasicBlockList().empty() && "prologue already emitted?");
  CurFn->getBasicBlockList().push_back(EntryBB);
  Builder.SetInsertPoint(EntryBB);

  // Set up the alloca insertion point.
  AllocaIP = Builder.CreateAlloca(IGM.Int1Ty, /*array size*/ nullptr,
                                  "alloca point");

  // Set up the return block and insert it.  This creates a second
  // insertion point that most blocks should be inserted before.
  ReturnBB = createBasicBlock("return");
  CurFn->getBasicBlockList().push_back(ReturnBB);

  FunctionType *FnTy = CurFuncExpr->getType()->getAs<FunctionType>();
  assert(FnTy && "emitting a declaration that's not a function?");

  llvm::Function::arg_iterator CurParm = CurFn->arg_begin();

  // Set up the result slot.
  const TypeInfo &resultType = IGM.getFragileTypeInfo(FnTy->Result);
  RValueSchema resultSchema = resultType.getSchema();
  if (resultSchema.isAggregate()) {
    ReturnSlot = Address(CurParm++, resultType.StorageAlignment);
  } else if (resultSchema.isScalar(0)) {
    assert(!ReturnSlot.isValid());
  } else {
    ReturnSlot = createScopeAlloca(resultType.getStorageType(),
                                   resultType.StorageAlignment,
                                   "return_value");
  }

  // Set up the parameters.
  for (ArgDecl *Parm : CurFuncExpr->NamedArgs) {
    const TypeInfo &ParmInfo = IGM.getFragileTypeInfo(Parm->getType());
    RValueSchema ParmSchema = ParmInfo.getSchema();

    // Make an l-value for the parameter.
    Address parmAddr;
    if (ParmSchema.isAggregate()) {
      parmAddr = Address(CurParm++, ParmInfo.StorageAlignment);
    } else {
      parmAddr = createScopeAlloca(ParmInfo.getStorageType(),
                                   ParmInfo.StorageAlignment,
                                   Parm->getName().str());
    }

    // If the parameter was scalar, form an r-value from the
    // parameters and store that.
    if (ParmSchema.isScalar()) {
      SmallVector<llvm::Value*, RValue::MaxScalars> Scalars;
      for (llvm::Type *ParmType : ParmSchema.getScalarTypes()) {
        llvm::Value *V = CurParm++;
        assert(V->getType() == ParmType);
        (void) ParmType;
        Scalars.push_back(V);
      }

      RValue ParmRV = RValue::forScalars(Scalars);
      ParmInfo.store(*this, ParmRV, parmAddr);
    }

    assert(!Locals.count(Parm));
    Locals.insert(std::make_pair(Parm, parmAddr));
  }

  // TODO: data pointer

  assert(CurParm == CurFn->arg_end() && "didn't exhaust all parameters?");
}

/// Emit the epilogue for the function.
void IRGenFunction::emitEpilogue() {
  // Destroy the alloca insertion point.
  AllocaIP->eraseFromParent();

  // If there are no edges to the return block, we never want to emit it.
  if (ReturnBB->use_empty()) {
    ReturnBB->eraseFromParent();

    // Normally this means that we'll just insert the epilogue in the
    // current block, but if the current IP is unreachable then so is
    // the entire epilogue.
    if (!Builder.hasValidIP()) return;

  // Otherwise, branch to it if the current IP is reachable.
  } else if (Builder.hasValidIP()) {
    Builder.CreateBr(ReturnBB);
    Builder.SetInsertPoint(ReturnBB);

  // Otherwise, if there is exactly one use of the return block, merge
  // it into its predecessor.
  } else if (ReturnBB->hasOneUse()) {
    // return statements are never emitted as conditional branches.
    llvm::BranchInst *Br = cast<llvm::BranchInst>(*ReturnBB->use_begin());
    assert(Br->isUnconditional());
    Builder.SetInsertPoint(Br->getParent());
    Br->eraseFromParent();
    ReturnBB->eraseFromParent();

  // Otherwise, just move the IP to the return block.
  } else {
    Builder.SetInsertPoint(ReturnBB);
  }

  FunctionType *FnTy = CurFuncExpr->getType()->getAs<FunctionType>();
  assert(FnTy && "emitting a declaration that's not a function?");

  const TypeInfo &resultType = IGM.getFragileTypeInfo(FnTy->Result);
  RValueSchema resultSchema = resultType.getSchema();
  if (resultSchema.isAggregate()) {
    assert(isa<llvm::Argument>(ReturnSlot.getAddress()));
    Builder.CreateRetVoid();
  } else if (resultSchema.isScalar(0)) {
    assert(!ReturnSlot.isValid());
    Builder.CreateRetVoid();
  } else {
    RValue RV = resultType.load(*this, ReturnSlot);
    if (RV.isScalar(1)) {
      Builder.CreateRet(RV.getScalars()[0]);
    } else {
      llvm::Value *Result = llvm::UndefValue::get(CurFn->getReturnType());
      for (unsigned I = 0, E = RV.getScalars().size(); I != E; ++I)
        Result = Builder.CreateInsertValue(Result, RV.getScalars()[I], I);
      Builder.CreateRet(Result);
    }
  }
}

/// Emit the definition for the given global function.
void IRGenModule::emitGlobalFunction(FuncDecl *FD) {
  // Nothing to do if the function has no body.
  if (!FD->getInit()) return;

  llvm::Function *addr = getAddrOfGlobalFunction(FD);

  FuncExpr *func = cast<FuncExpr>(FD->getInit());
  IRGenFunction(*this, func, addr).emitFunctionTopLevel(func->getBody());
}

void IRGenFunction::emitFunctionTopLevel(BraceStmt *S) {
  emitBraceStmt(S);
}
