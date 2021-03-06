//===-- MipsSEISelLowering.cpp - MipsSE DAG Lowering Interface --*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Subclass of MipsTargetLowering specialized for mips32/64.
//
//===----------------------------------------------------------------------===//
#include "MipsSEISelLowering.h"
#include "MipsRegisterInfo.h"
#include "MipsTargetMachine.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Target/TargetInstrInfo.h"

using namespace llvm;

static cl::opt<bool>
EnableMipsTailCalls("enable-mips-tail-calls", cl::Hidden,
                    cl::desc("MIPS: Enable tail calls."), cl::init(false));

MipsSETargetLowering::MipsSETargetLowering(MipsTargetMachine &TM)
  : MipsTargetLowering(TM) {
  // Set up the register classes

  clearRegisterClasses();

  addRegisterClass(MVT::i32, &Mips::CPURegsRegClass);

  if (HasMips64)
    addRegisterClass(MVT::i64, &Mips::CPU64RegsRegClass);

  if (Subtarget->hasDSP()) {
    MVT::SimpleValueType VecTys[2] = {MVT::v2i16, MVT::v4i8};

    for (unsigned i = 0; i < array_lengthof(VecTys); ++i) {
      addRegisterClass(VecTys[i], &Mips::DSPRegsRegClass);

      // Expand all builtin opcodes.
      for (unsigned Opc = 0; Opc < ISD::BUILTIN_OP_END; ++Opc)
        setOperationAction(Opc, VecTys[i], Expand);

      setOperationAction(ISD::LOAD, VecTys[i], Legal);
      setOperationAction(ISD::STORE, VecTys[i], Legal);
      setOperationAction(ISD::BITCAST, VecTys[i], Legal);
    }
  }

  if (!TM.Options.UseSoftFloat) {
    addRegisterClass(MVT::f32, &Mips::FGR32RegClass);

    // When dealing with single precision only, use libcalls
    if (!Subtarget->isSingleFloat()) {
      if (HasMips64)
        addRegisterClass(MVT::f64, &Mips::FGR64RegClass);
      else
        addRegisterClass(MVT::f64, &Mips::AFGR64RegClass);
    }
  }

  setOperationAction(ISD::SMUL_LOHI,          MVT::i32, Custom);
  setOperationAction(ISD::UMUL_LOHI,          MVT::i32, Custom);
  setOperationAction(ISD::MULHS,              MVT::i32, Custom);
  setOperationAction(ISD::MULHU,              MVT::i32, Custom);

  if (HasMips64)
    setOperationAction(ISD::MUL,              MVT::i64, Custom);

  setOperationAction(ISD::SDIVREM, MVT::i32, Custom);
  setOperationAction(ISD::UDIVREM, MVT::i32, Custom);
  setOperationAction(ISD::SDIVREM, MVT::i64, Custom);
  setOperationAction(ISD::UDIVREM, MVT::i64, Custom);
  setOperationAction(ISD::MEMBARRIER,         MVT::Other, Custom);
  setOperationAction(ISD::ATOMIC_FENCE,       MVT::Other, Custom);
  setOperationAction(ISD::LOAD,               MVT::i32, Custom);
  setOperationAction(ISD::STORE,              MVT::i32, Custom);

  setTargetDAGCombine(ISD::ADDE);
  setTargetDAGCombine(ISD::SUBE);

  computeRegisterProperties();
}

const MipsTargetLowering *
llvm::createMipsSETargetLowering(MipsTargetMachine &TM) {
  return new MipsSETargetLowering(TM);
}


bool
MipsSETargetLowering::allowsUnalignedMemoryAccesses(EVT VT, bool *Fast) const {
  MVT::SimpleValueType SVT = VT.getSimpleVT().SimpleTy;

  switch (SVT) {
  case MVT::i64:
  case MVT::i32:
    if (Fast)
      *Fast = true;
    return true;
  default:
    return false;
  }
}

SDValue MipsSETargetLowering::LowerOperation(SDValue Op,
                                             SelectionDAG &DAG) const {
  switch(Op.getOpcode()) {
  case ISD::SMUL_LOHI: return lowerMulDiv(Op, MipsISD::Mult, true, true, DAG);
  case ISD::UMUL_LOHI: return lowerMulDiv(Op, MipsISD::Multu, true, true, DAG);
  case ISD::MULHS:     return lowerMulDiv(Op, MipsISD::Mult, false, true, DAG);
  case ISD::MULHU:     return lowerMulDiv(Op, MipsISD::Multu, false, true, DAG);
  case ISD::MUL:       return lowerMulDiv(Op, MipsISD::Mult, true, false, DAG);
  case ISD::SDIVREM:   return lowerMulDiv(Op, MipsISD::DivRem, true, true, DAG);
  case ISD::UDIVREM:   return lowerMulDiv(Op, MipsISD::DivRemU, true, true, DAG);
  }

  return MipsTargetLowering::LowerOperation(Op, DAG);
}

// selectMADD -
// Transforms a subgraph in CurDAG if the following pattern is found:
//  (addc multLo, Lo0), (adde multHi, Hi0),
// where,
//  multHi/Lo: product of multiplication
//  Lo0: initial value of Lo register
//  Hi0: initial value of Hi register
// Return true if pattern matching was successful.
static bool selectMADD(SDNode *ADDENode, SelectionDAG *CurDAG) {
  // ADDENode's second operand must be a flag output of an ADDC node in order
  // for the matching to be successful.
  SDNode *ADDCNode = ADDENode->getOperand(2).getNode();

  if (ADDCNode->getOpcode() != ISD::ADDC)
    return false;

  SDValue MultHi = ADDENode->getOperand(0);
  SDValue MultLo = ADDCNode->getOperand(0);
  SDNode *MultNode = MultHi.getNode();
  unsigned MultOpc = MultHi.getOpcode();

  // MultHi and MultLo must be generated by the same node,
  if (MultLo.getNode() != MultNode)
    return false;

  // and it must be a multiplication.
  if (MultOpc != ISD::SMUL_LOHI && MultOpc != ISD::UMUL_LOHI)
    return false;

  // MultLo amd MultHi must be the first and second output of MultNode
  // respectively.
  if (MultHi.getResNo() != 1 || MultLo.getResNo() != 0)
    return false;

  // Transform this to a MADD only if ADDENode and ADDCNode are the only users
  // of the values of MultNode, in which case MultNode will be removed in later
  // phases.
  // If there exist users other than ADDENode or ADDCNode, this function returns
  // here, which will result in MultNode being mapped to a single MULT
  // instruction node rather than a pair of MULT and MADD instructions being
  // produced.
  if (!MultHi.hasOneUse() || !MultLo.hasOneUse())
    return false;

  DebugLoc DL = ADDENode->getDebugLoc();

  // Initialize accumulator.
  SDValue ACCIn = CurDAG->getNode(MipsISD::InsertLOHI, DL, MVT::Untyped,
                                  ADDCNode->getOperand(1),
                                  ADDENode->getOperand(1));

  // create MipsMAdd(u) node
  MultOpc = MultOpc == ISD::UMUL_LOHI ? MipsISD::MAddu : MipsISD::MAdd;

  SDValue MAdd = CurDAG->getNode(MultOpc, DL, MVT::Untyped,
                                 MultNode->getOperand(0),// Factor 0
                                 MultNode->getOperand(1),// Factor 1
                                 ACCIn);

  // replace uses of adde and addc here
  if (!SDValue(ADDCNode, 0).use_empty()) {
    SDValue LoIdx = CurDAG->getConstant(Mips::sub_lo, MVT::i32);
    SDValue LoOut = CurDAG->getNode(MipsISD::ExtractLOHI, DL, MVT::i32, MAdd,
                                    LoIdx);
    CurDAG->ReplaceAllUsesOfValueWith(SDValue(ADDCNode, 0), LoOut);
  }
  if (!SDValue(ADDENode, 0).use_empty()) {
    SDValue HiIdx = CurDAG->getConstant(Mips::sub_hi, MVT::i32);
    SDValue HiOut = CurDAG->getNode(MipsISD::ExtractLOHI, DL, MVT::i32, MAdd,
                                    HiIdx);
    CurDAG->ReplaceAllUsesOfValueWith(SDValue(ADDENode, 0), HiOut);
  }

  return true;
}

// selectMSUB -
// Transforms a subgraph in CurDAG if the following pattern is found:
//  (addc Lo0, multLo), (sube Hi0, multHi),
// where,
//  multHi/Lo: product of multiplication
//  Lo0: initial value of Lo register
//  Hi0: initial value of Hi register
// Return true if pattern matching was successful.
static bool selectMSUB(SDNode *SUBENode, SelectionDAG *CurDAG) {
  // SUBENode's second operand must be a flag output of an SUBC node in order
  // for the matching to be successful.
  SDNode *SUBCNode = SUBENode->getOperand(2).getNode();

  if (SUBCNode->getOpcode() != ISD::SUBC)
    return false;

  SDValue MultHi = SUBENode->getOperand(1);
  SDValue MultLo = SUBCNode->getOperand(1);
  SDNode *MultNode = MultHi.getNode();
  unsigned MultOpc = MultHi.getOpcode();

  // MultHi and MultLo must be generated by the same node,
  if (MultLo.getNode() != MultNode)
    return false;

  // and it must be a multiplication.
  if (MultOpc != ISD::SMUL_LOHI && MultOpc != ISD::UMUL_LOHI)
    return false;

  // MultLo amd MultHi must be the first and second output of MultNode
  // respectively.
  if (MultHi.getResNo() != 1 || MultLo.getResNo() != 0)
    return false;

  // Transform this to a MSUB only if SUBENode and SUBCNode are the only users
  // of the values of MultNode, in which case MultNode will be removed in later
  // phases.
  // If there exist users other than SUBENode or SUBCNode, this function returns
  // here, which will result in MultNode being mapped to a single MULT
  // instruction node rather than a pair of MULT and MSUB instructions being
  // produced.
  if (!MultHi.hasOneUse() || !MultLo.hasOneUse())
    return false;

  DebugLoc DL = SUBENode->getDebugLoc();

  // Initialize accumulator.
  SDValue ACCIn = CurDAG->getNode(MipsISD::InsertLOHI, DL, MVT::Untyped,
                                  SUBCNode->getOperand(0),
                                  SUBENode->getOperand(0));

  // create MipsSub(u) node
  MultOpc = MultOpc == ISD::UMUL_LOHI ? MipsISD::MSubu : MipsISD::MSub;

  SDValue MSub = CurDAG->getNode(MultOpc, DL, MVT::Glue,
                                 MultNode->getOperand(0),// Factor 0
                                 MultNode->getOperand(1),// Factor 1
                                 ACCIn);

  // replace uses of sube and subc here
  if (!SDValue(SUBCNode, 0).use_empty()) {
    SDValue LoIdx = CurDAG->getConstant(Mips::sub_lo, MVT::i32);
    SDValue LoOut = CurDAG->getNode(MipsISD::ExtractLOHI, DL, MVT::i32, MSub,
                                    LoIdx);
    CurDAG->ReplaceAllUsesOfValueWith(SDValue(SUBCNode, 0), LoOut);
  }
  if (!SDValue(SUBENode, 0).use_empty()) {
    SDValue HiIdx = CurDAG->getConstant(Mips::sub_hi, MVT::i32);
    SDValue HiOut = CurDAG->getNode(MipsISD::ExtractLOHI, DL, MVT::i32, MSub,
                                    HiIdx);
    CurDAG->ReplaceAllUsesOfValueWith(SDValue(SUBENode, 0), HiOut);
  }

  return true;
}

static SDValue performADDECombine(SDNode *N, SelectionDAG &DAG,
                                  TargetLowering::DAGCombinerInfo &DCI,
                                  const MipsSubtarget *Subtarget) {
  if (DCI.isBeforeLegalize())
    return SDValue();

  if (Subtarget->hasMips32() && N->getValueType(0) == MVT::i32 &&
      selectMADD(N, &DAG))
    return SDValue(N, 0);

  return SDValue();
}

static SDValue performSUBECombine(SDNode *N, SelectionDAG &DAG,
                                  TargetLowering::DAGCombinerInfo &DCI,
                                  const MipsSubtarget *Subtarget) {
  if (DCI.isBeforeLegalize())
    return SDValue();

  if (Subtarget->hasMips32() && N->getValueType(0) == MVT::i32 &&
      selectMSUB(N, &DAG))
    return SDValue(N, 0);

  return SDValue();
}

SDValue
MipsSETargetLowering::PerformDAGCombine(SDNode *N, DAGCombinerInfo &DCI) const {
  SelectionDAG &DAG = DCI.DAG;

  switch (N->getOpcode()) {
  case ISD::ADDE:
    return performADDECombine(N, DAG, DCI, Subtarget);
  case ISD::SUBE:
    return performSUBECombine(N, DAG, DCI, Subtarget);
  default:
    return MipsTargetLowering::PerformDAGCombine(N, DCI);
  }
}

MachineBasicBlock *
MipsSETargetLowering::EmitInstrWithCustomInserter(MachineInstr *MI,
                                                  MachineBasicBlock *BB) const {
  switch (MI->getOpcode()) {
  default:
    return MipsTargetLowering::EmitInstrWithCustomInserter(MI, BB);
  case Mips::BPOSGE32_PSEUDO:
    return emitBPOSGE32(MI, BB);
  }
}

bool MipsSETargetLowering::
isEligibleForTailCallOptimization(const MipsCC &MipsCCInfo,
                                  unsigned NextStackOffset,
                                  const MipsFunctionInfo& FI) const {
  if (!EnableMipsTailCalls)
    return false;

  // Return false if either the callee or caller has a byval argument.
  if (MipsCCInfo.hasByValArg() || FI.hasByvalArg())
    return false;

  // Return true if the callee's argument area is no larger than the
  // caller's.
  return NextStackOffset <= FI.getIncomingArgSize();
}

void MipsSETargetLowering::
getOpndList(SmallVectorImpl<SDValue> &Ops,
            std::deque< std::pair<unsigned, SDValue> > &RegsToPass,
            bool IsPICCall, bool GlobalOrExternal, bool InternalLinkage,
            CallLoweringInfo &CLI, SDValue Callee, SDValue Chain) const {
  // T9 should contain the address of the callee function if
  // -reloction-model=pic or it is an indirect call.
  if (IsPICCall || !GlobalOrExternal) {
    unsigned T9Reg = IsN64 ? Mips::T9_64 : Mips::T9;
    RegsToPass.push_front(std::make_pair(T9Reg, Callee));
  } else
    Ops.push_back(Callee);

  MipsTargetLowering::getOpndList(Ops, RegsToPass, IsPICCall, GlobalOrExternal,
                                  InternalLinkage, CLI, Callee, Chain);
}

SDValue MipsSETargetLowering::lowerMulDiv(SDValue Op, unsigned NewOpc,
                                          bool HasLo, bool HasHi,
                                          SelectionDAG &DAG) const {
  EVT Ty = Op.getOperand(0).getValueType();
  DebugLoc DL = Op.getDebugLoc();
  SDValue Mult = DAG.getNode(NewOpc, DL, MVT::Untyped,
                             Op.getOperand(0), Op.getOperand(1));
  SDValue Lo, Hi;

  if (HasLo)
    Lo = DAG.getNode(MipsISD::ExtractLOHI, DL, Ty, Mult,
                     DAG.getConstant(Mips::sub_lo, MVT::i32));
  if (HasHi)
    Hi = DAG.getNode(MipsISD::ExtractLOHI, DL, Ty, Mult,
                     DAG.getConstant(Mips::sub_hi, MVT::i32));

  if (!HasLo || !HasHi)
    return HasLo ? Lo : Hi;

  SDValue Vals[] = { Lo, Hi };
  return DAG.getMergeValues(Vals, 2, DL);
}

MachineBasicBlock * MipsSETargetLowering::
emitBPOSGE32(MachineInstr *MI, MachineBasicBlock *BB) const{
  // $bb:
  //  bposge32_pseudo $vr0
  //  =>
  // $bb:
  //  bposge32 $tbb
  // $fbb:
  //  li $vr2, 0
  //  b $sink
  // $tbb:
  //  li $vr1, 1
  // $sink:
  //  $vr0 = phi($vr2, $fbb, $vr1, $tbb)

  MachineRegisterInfo &RegInfo = BB->getParent()->getRegInfo();
  const TargetInstrInfo *TII = getTargetMachine().getInstrInfo();
  const TargetRegisterClass *RC = &Mips::CPURegsRegClass;
  DebugLoc DL = MI->getDebugLoc();
  const BasicBlock *LLVM_BB = BB->getBasicBlock();
  MachineFunction::iterator It = llvm::next(MachineFunction::iterator(BB));
  MachineFunction *F = BB->getParent();
  MachineBasicBlock *FBB = F->CreateMachineBasicBlock(LLVM_BB);
  MachineBasicBlock *TBB = F->CreateMachineBasicBlock(LLVM_BB);
  MachineBasicBlock *Sink  = F->CreateMachineBasicBlock(LLVM_BB);
  F->insert(It, FBB);
  F->insert(It, TBB);
  F->insert(It, Sink);

  // Transfer the remainder of BB and its successor edges to Sink.
  Sink->splice(Sink->begin(), BB, llvm::next(MachineBasicBlock::iterator(MI)),
               BB->end());
  Sink->transferSuccessorsAndUpdatePHIs(BB);

  // Add successors.
  BB->addSuccessor(FBB);
  BB->addSuccessor(TBB);
  FBB->addSuccessor(Sink);
  TBB->addSuccessor(Sink);

  // Insert the real bposge32 instruction to $BB.
  BuildMI(BB, DL, TII->get(Mips::BPOSGE32)).addMBB(TBB);

  // Fill $FBB.
  unsigned VR2 = RegInfo.createVirtualRegister(RC);
  BuildMI(*FBB, FBB->end(), DL, TII->get(Mips::ADDiu), VR2)
    .addReg(Mips::ZERO).addImm(0);
  BuildMI(*FBB, FBB->end(), DL, TII->get(Mips::B)).addMBB(Sink);

  // Fill $TBB.
  unsigned VR1 = RegInfo.createVirtualRegister(RC);
  BuildMI(*TBB, TBB->end(), DL, TII->get(Mips::ADDiu), VR1)
    .addReg(Mips::ZERO).addImm(1);

  // Insert phi function to $Sink.
  BuildMI(*Sink, Sink->begin(), DL, TII->get(Mips::PHI),
          MI->getOperand(0).getReg())
    .addReg(VR2).addMBB(FBB).addReg(VR1).addMBB(TBB);

  MI->eraseFromParent();   // The pseudo instruction is gone now.
  return Sink;
}
