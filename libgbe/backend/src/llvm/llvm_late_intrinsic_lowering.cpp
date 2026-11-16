/*
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * \file llvm_late_intrinsic_lowering.cpp
 *
 * InstCombine in LLVM 8+ folds plain integer code into intrinsics the Gen
 * backend has no lowering for:
 *
 *   rotate / funnel shift  (x << s) | (y >> (w - s))  -> llvm.fshl / llvm.fshr
 *   saturating add / sub   s = a + b; s < a ? ~0 : s  -> llvm.{u,s}{add,sub}.sat
 *   sign bit transfer      (a & 0x7f..) | (b & 0x80..) -> llvm.copysign  (LLVM 12+)
 *   min / max / abs        a < b ? a : b, a < 0 ? -a : a -> llvm.{s,u}{min,max}, llvm.abs (LLVM 16)
 *
 * and inserts 'freeze' instructions (LLVM 10+, used by InstCombine from 12 on),
 * which GenWriter does not know either.
 *
 * GenWriter leaves such a call without a register, so its users read garbage
 * register numbers (crash in buildRegInfo, or wrong results). This pass
 * expands them back into the shift/compare/select code they came from.
 *
 * Like FNegLowering it runs right before each scalarize pass, after the last
 * InstCombine, which would otherwise fold the expansion right back.
 */

#include "llvm_includes.hpp"

#include "llvm/llvm_gen_backend.hpp"

using namespace llvm;

namespace gbe {
#if LLVM_VERSION_MAJOR >= 8
    class LateIntrinsicLowering : public FunctionPass
    {
    public:
      static char ID;
      LateIntrinsicLowering() : FunctionPass(ID) {}

      virtual StringRef getPassName() const {
        return "Late intrinsic lowering";
      }

      // fshl(a, b, c): the top w bits of (a:b) << (c % w)
      // fshr(a, b, c): the low  w bits of (a:b) >> (c % w)
      // A shift by 0 returns a (fshl) or b (fshr) unchanged; the other half
      // would need a shift by w, which is undefined, so it is selected away.
      static Value *expandFunnelShift(IRBuilder<> &B, IntrinsicInst *CI, bool left) {
        Value *a = CI->getArgOperand(0), *b = CI->getArgOperand(1);
        Type *ty = CI->getType();
        unsigned w = ty->getScalarSizeInBits();
        Value *s = B.CreateAnd(CI->getArgOperand(2), ConstantInt::get(ty, w - 1));
        Value *rest = B.CreateSub(ConstantInt::get(ty, w), s);
        Value *res = left ? B.CreateOr(B.CreateShl(a, s), B.CreateLShr(b, rest))
                          : B.CreateOr(B.CreateShl(a, rest), B.CreateLShr(b, s));
        Value *isZero = B.CreateICmpEQ(s, ConstantInt::get(ty, 0));
        return B.CreateSelect(isZero, left ? a : b, res);
      }

      static Value *expandSaturating(IRBuilder<> &B, IntrinsicInst *CI) {
        Value *a = CI->getArgOperand(0), *b = CI->getArgOperand(1);
        Type *ty = CI->getType();
        unsigned w = ty->getScalarSizeInBits();
        Value *zero = ConstantInt::get(ty, 0);
        switch (CI->getIntrinsicID()) {
          case Intrinsic::uadd_sat: {
            Value *s = B.CreateAdd(a, b);
            return B.CreateSelect(B.CreateICmpULT(s, a), ConstantInt::get(ty, APInt::getMaxValue(w)), s);
          }
          case Intrinsic::usub_sat:
            return B.CreateSelect(B.CreateICmpUGT(a, b), B.CreateSub(a, b), zero);
          case Intrinsic::sadd_sat:
          case Intrinsic::ssub_sat: {
            bool add = CI->getIntrinsicID() == Intrinsic::sadd_sat;
            Value *s = add ? B.CreateAdd(a, b) : B.CreateSub(a, b);
            // Overflow iff the sign of the result differs from a's while the
            // operands' signs say it cannot (same signs for add, different
            // for sub).
            Value *ovf = add ? B.CreateAnd(B.CreateXor(a, s), B.CreateXor(b, s))
                             : B.CreateAnd(B.CreateXor(a, b), B.CreateXor(a, s));
            Value *sat = B.CreateSelect(B.CreateICmpSLT(a, zero),
                                        ConstantInt::get(ty, APInt::getSignedMinValue(w)),
                                        ConstantInt::get(ty, APInt::getSignedMaxValue(w)));
            return B.CreateSelect(B.CreateICmpSLT(ovf, zero), sat, s);
          }
          default:
            return NULL;
        }
      }

      // copysign(a, b): magnitude of a, sign bit of b, done on the bits.
      static Value *expandCopySign(IRBuilder<> &B, IntrinsicInst *CI) {
        Type *ty = CI->getType();
        unsigned w = ty->getScalarSizeInBits();
        Type *ity = B.getIntNTy(w);
        if (auto *vty = dyn_cast<FixedVectorType>(ty))
          ity = FixedVectorType::get(ity, vty->getNumElements());
        Value *a = B.CreateBitCast(CI->getArgOperand(0), ity);
        Value *b = B.CreateBitCast(CI->getArgOperand(1), ity);
        Value *sign = ConstantInt::get(ity, APInt::getSignMask(w));
        Value *mag = ConstantInt::get(ity, APInt::getSignedMaxValue(w));
        Value *res = B.CreateOr(B.CreateAnd(a, mag), B.CreateAnd(b, sign));
        return B.CreateBitCast(res, ty);
      }

      // Integer min/max/abs back into compare + select.
      static Value *expandMinMaxAbs(IRBuilder<> &B, IntrinsicInst *CI) {
        Value *a = CI->getArgOperand(0);
        switch (CI->getIntrinsicID()) {
          case Intrinsic::abs: {
            // The second operand only says whether abs(INT_MIN) is poison;
            // either way the wrapped -INT_MIN is a valid result.
            Value *zero = ConstantInt::get(a->getType(), 0);
            return B.CreateSelect(B.CreateICmpSLT(a, zero), B.CreateNeg(a), a);
          }
          case Intrinsic::smin: return B.CreateSelect(B.CreateICmpSLT(a, CI->getArgOperand(1)), a, CI->getArgOperand(1));
          case Intrinsic::smax: return B.CreateSelect(B.CreateICmpSGT(a, CI->getArgOperand(1)), a, CI->getArgOperand(1));
          case Intrinsic::umin: return B.CreateSelect(B.CreateICmpULT(a, CI->getArgOperand(1)), a, CI->getArgOperand(1));
          case Intrinsic::umax: return B.CreateSelect(B.CreateICmpUGT(a, CI->getArgOperand(1)), a, CI->getArgOperand(1));
          default: return NULL;
        }
      }

      virtual bool runOnFunction(Function &F) {
        bool changed = false;
        for (BasicBlock &BB : F) {
          for (auto it = BB.begin(); it != BB.end(); ) {
            Instruction *I = &*it++;
            // freeze only pins down poison/undef; the hardware has neither,
            // so at codegen it is just its operand.
            if (auto *FI = dyn_cast<FreezeInst>(I)) {
              FI->replaceAllUsesWith(FI->getOperand(0));
              FI->eraseFromParent();
              changed = true;
              continue;
            }
            IntrinsicInst *CI = dyn_cast<IntrinsicInst>(I);
            if (!CI)
              continue;
            IRBuilder<> B(CI);
            Value *res = NULL;
            switch (CI->getIntrinsicID()) {
              case Intrinsic::fshl: res = expandFunnelShift(B, CI, true); break;
              case Intrinsic::fshr: res = expandFunnelShift(B, CI, false); break;
              case Intrinsic::uadd_sat:
              case Intrinsic::usub_sat:
              case Intrinsic::sadd_sat:
              case Intrinsic::ssub_sat: res = expandSaturating(B, CI); break;
              case Intrinsic::copysign: res = expandCopySign(B, CI); break;
              case Intrinsic::abs:
              case Intrinsic::smin:
              case Intrinsic::smax:
              case Intrinsic::umin:
              case Intrinsic::umax: res = expandMinMaxAbs(B, CI); break;
              default: break;
            }
            if (!res)
              continue;
            res->takeName(CI);
            CI->replaceAllUsesWith(res);
            CI->eraseFromParent();
            changed = true;
          }
        }
        return changed;
      }
    };

    char LateIntrinsicLowering::ID = 0;

    FunctionPass *createLateIntrinsicLoweringPass() {
      return new LateIntrinsicLowering();
    }
#endif
} // end namespace
