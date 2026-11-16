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
 * \file llvm_fneg_lowering.cpp
 *
 * LLVM 8 added a unary fneg instruction and Clang emits it for "-x" from
 * LLVM 9/10 on. Before that, negation was "fsub -0.0, x", which is all the
 * Gen backend (scalarize, GenWriter) knows about: an fneg went through
 * GenWriter without ever getting a register, so its users read garbage
 * register numbers. This pass turns every fneg back into the old form,
 * which has the same semantics (sign flip, also for zeros and NaNs).
 *
 * It runs right before each scalarize pass, since InstCombine in between
 * canonicalizes "fsub -0.0, x" back into fneg.
 */

#include "llvm_includes.hpp"

#include "llvm/llvm_gen_backend.hpp"

using namespace llvm;

namespace gbe {
#if LLVM_VERSION_MAJOR >= 8
    class FNegLowering : public FunctionPass
    {
    public:
      static char ID;
      FNegLowering() : FunctionPass(ID) {}

      virtual StringRef getPassName() const {
        return "FNeg lowering";
      }

      virtual bool runOnFunction(Function &F) {
        bool changed = false;
        for (BasicBlock &BB : F) {
          for (auto it = BB.begin(); it != BB.end(); ) {
            Instruction &I = *it++;
            if (I.getOpcode() != Instruction::FNeg)
              continue;
            Value *x = I.getOperand(0);
            Value *negZero = ConstantFP::getNegativeZero(x->getType());
            Instruction *sub = BinaryOperator::CreateFSub(negZero, x, "", &I);
            sub->copyIRFlags(&I);
            sub->setDebugLoc(I.getDebugLoc());
            sub->takeName(&I);
            I.replaceAllUsesWith(sub);
            I.eraseFromParent();
            changed = true;
          }
        }
        return changed;
      }
    };

    char FNegLowering::ID = 0;

    FunctionPass *createFNegLoweringPass() {
      return new FNegLowering();
    }
#endif
} // end namespace
