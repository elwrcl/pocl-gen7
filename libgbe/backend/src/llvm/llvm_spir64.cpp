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
 * \file llvm_spir64.cpp
 *
 * SYCL compilers only emit spir64 (64-bit pointers and size_t). Gen7 and
 * beignet's libocl are 32-bit, so a spir64 module is narrowed before it is
 * linked with libocl:
 *  - the data layout becomes libocl's (32-bit pointers; with opaque pointers
 *    the IR stays valid, 64-bit GEP indices are truncated by the backend);
 *  - calls to builtins whose spir64 form uses size_t are redirected to the
 *    32-bit libocl function: a size_t parameter is mangled 'm' (ulong) in
 *    spir64 and 'j' (uint) in spir, and get_global_id() & co. keep their name
 *    but return i64; arguments are truncated and results zero-extended.
 * Builtins that really take ulong exist in libocl under the 'm' name and are
 * left alone.
 */

#include "llvm_includes.hpp"

#include "llvm/llvm_gen_backend.hpp"

#include <cctype>
#include <vector>

using namespace llvm;

namespace gbe
{
  /*! Positions of the builtin-type token 'm' in the parameter part of an
   *  Itanium-mangled name (_Z<len><name><params>). Names inside the
   *  parameters (14ocl_image2d_ro, vendor qualifiers U3AS1) are skipped. */
  static std::vector<size_t> ulongParams(StringRef mangled)
  {
    std::vector<size_t> pos;
    if (!mangled.starts_with("_Z"))
      return pos;
    size_t i = 2, n = mangled.size();
    auto number = [&](size_t &k) {
      size_t v = 0;
      while (k < n && isdigit((unsigned char)mangled[k]))
        v = v * 10 + (mangled[k++] - '0');
      return v;
    };
    /* function name */
    size_t len = number(i);
    i += len;
    while (i < n) {
      char c = mangled[i];
      if (isdigit((unsigned char)c)) { /* class name */
        size_t l = number(i);
        i += l;
      } else if (c == 'U') { /* vendor qualifier, e.g. U3AS1 */
        i++;
        size_t l = number(i);
        i += l;
      } else if (c == 'D' && i + 1 < n && mangled[i + 1] == 'v') { /* Dv4_ */
        i += 2;
        number(i);
        if (i < n && mangled[i] == '_')
          i++;
      } else if (c == 'S') { /* substitution S_ / S0_ */
        while (i < n && mangled[i] != '_')
          i++;
        i++;
      } else {
        if (c == 'm')
          pos.push_back(i);
        i++;
      }
    }
    return pos;
  }

  static Value *convertInt(IRBuilder<> &B, Value *v, Type *to)
  {
    if (v->getType() == to)
      return v;
    if (v->getType()->isIntegerTy() && to->isIntegerTy())
      return B.CreateZExtOrTrunc(v, to);
    if (v->getType()->isVectorTy() && to->isVectorTy() &&
        v->getType()->getScalarType()->isIntegerTy() && to->getScalarType()->isIntegerTy())
      return B.CreateZExtOrTrunc(v, to);
    return NULL;
  }

  /*! Only integer widths differ (size_t); image/sampler and other type
   *  differences are left to restoreOpenCLHandleTypes. */
  static bool intWidthsOnly(FunctionType *from, FunctionType *to)
  {
    auto ok = [](Type *a, Type *b) {
      if (a == b)
        return true;
      return a->getScalarType()->isIntegerTy() && b->getScalarType()->isIntegerTy() &&
             (a->isVectorTy() == b->isVectorTy());
    };
    if (from->getNumParams() != to->getNumParams() ||
        !ok(from->getReturnType(), to->getReturnType()))
      return false;
    for (unsigned i = 0; i < from->getNumParams(); i++)
      if (!ok(from->getParamType(i), to->getParamType(i)))
        return false;
    return true;
  }

  bool narrowSpir64(Module &M, Module &Lib, std::string &err)
  {
    if (M.getDataLayout().getPointerSizeInBits(1) != 64 &&
        !StringRef(M.getTargetTriple().str()).starts_with("spir64"))
      return true;

    M.setDataLayout(Lib.getDataLayout());
    M.setTargetTriple(Lib.getTargetTriple());

    std::vector<Function *> decls;
    for (Function &F : M)
      if (F.isDeclaration() && !F.isIntrinsic() && !F.use_empty())
        decls.push_back(&F);

    for (Function *F : decls) {
      StringRef name = F->getName();
      Function *L = Lib.getFunction(name);
      if (!L || L->getFunctionType() == F->getFunctionType()) {
        if (!L) {
          /* try the size_t parameters as uint */
          std::vector<size_t> pos = ulongParams(name);
          if (pos.empty())
            continue;
          std::string cand = name.str();
          for (size_t p : pos)
            cand[p] = 'j';
          L = Lib.getFunction(cand);
          if (!L)
            continue; /* not a libocl builtin, or a real ulong one */
        } else
          continue; /* same name, same type: nothing to do */
      }

      /* redirect every call of F to L's signature */
      FunctionType *LT = L->getFunctionType();
      if (!intWidthsOnly(F->getFunctionType(), LT))
        continue;
      Function *ND = cast<Function>(
          M.getOrInsertFunction((L->getName() + ".gbe32").str(), LT).getCallee());
      ND->setCallingConv(L->getCallingConv());
      std::vector<CallInst *> calls;
      for (User *U : F->users())
        if (CallInst *CI = dyn_cast<CallInst>(U))
          calls.push_back(CI);
      for (CallInst *CI : calls) {
        IRBuilder<> B(CI);
        std::vector<Value *> args;
        for (unsigned i = 0; i < CI->arg_size(); i++) {
          Value *a = convertInt(B, CI->getArgOperand(i), LT->getParamType(i));
          if (!a) {
            err = ("gen7: cannot narrow argument " + Twine(i) + " of " + name).str();
            return false;
          }
          args.push_back(a);
        }
        CallInst *NC = B.CreateCall(ND, args);
        NC->setCallingConv(CI->getCallingConv());
        Value *r = NC;
        if (!CI->getType()->isVoidTy()) {
          r = convertInt(B, NC, CI->getType());
          if (!r) {
            err = ("gen7: cannot widen the result of " + name).str();
            return false;
          }
        }
        if (!CI->getType()->isVoidTy())
          CI->replaceAllUsesWith(r);
        CI->eraseFromParent();
      }
      if (F->use_empty() && F->getName() != L->getName())
        F->eraseFromParent(); /* the 'm' spelling */
    }

    /* the redeclarations take libocl's names */
    std::vector<Function *> added;
    for (Function &F : M)
      if (F.getName().ends_with(".gbe32"))
        added.push_back(&F);
    for (Function *F : added) {
      std::string base = F->getName().drop_back(6).str();
      if (Function *old = M.getFunction(base)) {
        if (!old->use_empty()) {
          err = "gen7: spir64 builtin " + base + " still used with 64-bit types";
          return false;
        }
        old->eraseFromParent();
      }
      F->setName(base);
    }
    return true;
  }
} /* namespace gbe */
