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
 * \file llvm_spirv_handles.cpp
 *
 * Modules translated back from SPIR-V by llvm-spirv carry images and
 * samplers as plain pointers (ptr addrspace(1) / ptr addrspace(2)), while
 * clang 17+ and beignet's libocl use target("spirv.Image", ...) and
 * target("spirv.Sampler"). This rewrites such a module to the libocl types:
 * kernel arguments whose kernel_arg_type is an image or sampler_t get the
 * handle type, __translate_sampler_initializer returns a sampler, and every
 * builtin call that receives one of these values is redeclared with the
 * types libocl declares.
 *
 * Handles must reach the builtins directly, so everything is inlined into
 * the kernels and allocas are promoted first (SPIR-V built at -O0 passes the
 * image through a helper function and a stack slot).
 */

#include "llvm_includes.hpp"

#include "llvm/llvm_gen_backend.hpp"

#include <map>
#include <set>
#include <vector>

using namespace llvm;

namespace gbe
{
  static Type *imageTypeFromName(LLVMContext &C, StringRef ty, StringRef access)
  {
    unsigned dim, depth = 0, arrayed = 0;
    if (ty.starts_with("image1d_buffer_t")) dim = 5;
    else if (ty.starts_with("image1d_array_t")) { dim = 0; arrayed = 1; }
    else if (ty.starts_with("image1d_t")) dim = 0;
    else if (ty.starts_with("image2d_array_depth_t")) { dim = 1; arrayed = 1; depth = 1; }
    else if (ty.starts_with("image2d_array_t")) { dim = 1; arrayed = 1; }
    else if (ty.starts_with("image2d_depth_t")) { dim = 1; depth = 1; }
    else if (ty.starts_with("image2d_t")) dim = 1;
    else if (ty.starts_with("image3d_t")) dim = 2;
    else return NULL;
    unsigned acc = access == "write_only" ? 1 : access == "read_write" ? 2 : 0;
    return TargetExtType::get(C, "spirv.Image", {Type::getVoidTy(C)},
                              {dim, depth, arrayed, 0, 0, 0, acc});
  }

  static StringRef mdString(MDNode *md, unsigned i)
  {
    if (!md || i >= md->getNumOperands())
      return "";
    if (MDString *s = dyn_cast<MDString>(md->getOperand(i)))
      return s->getString();
    return "";
  }

  /*! The handle type kernel argument i should have, or NULL */
  static Type *handleTypeForArg(Function &F, unsigned i)
  {
    if (!F.getArg(i)->getType()->isPointerTy())
      return NULL;
    StringRef ty = mdString(F.getMetadata("kernel_arg_type"), i);
    StringRef base = mdString(F.getMetadata("kernel_arg_base_type"), i);
    StringRef access = mdString(F.getMetadata("kernel_arg_access_qual"), i);
    if (ty.empty())
      ty = base;
    if (ty == "sampler_t")
      return TargetExtType::get(F.getContext(), "spirv.Sampler");
    return imageTypeFromName(F.getContext(), ty, access);
  }

  static bool needsRewrite(Module &M)
  {
    if (Function *tsi = M.getFunction("__translate_sampler_initializer"))
      if (tsi->getReturnType()->isPointerTy())
        return true;
    for (Function &F : M)
      if (!F.isDeclaration() && isKernelFunction(F))
        for (unsigned i = 0; i < F.arg_size(); i++)
          if (handleTypeForArg(F, i))
            return true;
    return false;
  }

  namespace
  {
    struct Rewriter
    {
      Module &M;
      Module &Lib;
      std::string &err;
      /*! (original declaration, new type) -> redeclaration. A call with an
       *  image and a sampler is rewritten twice; the first redeclaration
       *  (image typed, sampler still a pointer) ends up unused. */
      std::map<std::pair<Function *, FunctionType *>, Function *> redecl;
      /*! redeclaration -> original declaration */
      std::map<Function *, Function *> origOf;
      std::vector<std::pair<Value *, Value *>> work;
      std::vector<Instruction *> dead;
      std::set<Instruction *> deadSet;
      /*! every pointer handle rewritten so far -> its handle-typed value */
      std::map<Value *, Value *> newOf;

      void kill(Instruction *I)
      {
        if (deadSet.insert(I).second)
          dead.push_back(I);
      }

      void push(Value *O, Value *N)
      {
        newOf[O] = N;
        work.push_back({O, N});
      }

      Rewriter(Module &m, Module &lib, std::string &e) : M(m), Lib(lib), err(e) {}

      bool fail(const Twine &msg)
      {
        err = msg.str();
        return false;
      }

      /*! Rebuild kernel K with handle-typed arguments */
      void retypeKernel(Function *K)
      {
        std::vector<Type *> params;
        bool changed = false;
        for (unsigned i = 0; i < K->arg_size(); i++) {
          Type *h = handleTypeForArg(*K, i);
          params.push_back(h ? h : K->getArg(i)->getType());
          changed |= h != NULL;
        }
        if (!changed)
          return;
        FunctionType *FT = FunctionType::get(K->getReturnType(), params, false);
        Function *NF = Function::Create(FT, K->getLinkage(), K->getAddressSpace(), "", &M);
        NF->copyAttributesFrom(K);
        NF->copyMetadata(K, 0);
        NF->setCallingConv(K->getCallingConv());
        /* pointer parameter attributes (align, noundef, ...) do not fit handles */
        AttributeList AL = NF->getAttributes();
        for (unsigned i = 0; i < K->arg_size(); i++)
          if (params[i] != K->getArg(i)->getType())
            AL = AL.removeParamAttributes(M.getContext(), i);
        NF->setAttributes(AL);
        NF->splice(NF->begin(), K);
        for (unsigned i = 0; i < K->arg_size(); i++) {
          Argument *O = K->getArg(i), *N = NF->getArg(i);
          N->takeName(O);
          if (params[i] == O->getType())
            O->replaceAllUsesWith(N);
          else
            push(O, N);
        }
        NF->takeName(K);
        /* K stays until its handle arguments have no users left */
        K->setName(NF->getName() + ".gbe_old");
        oldKernels.push_back(K);
      }
      std::vector<Function *> oldKernels;

      /*! Redeclaration of callee F taking the argument types of args */
      Function *redeclare(Function *F, Type *ret, ArrayRef<Value *> args)
      {
        if (origOf.count(F))
          F = origOf[F];
        std::vector<Type *> params;
        for (Value *a : args)
          params.push_back(a->getType());
        FunctionType *FT = FunctionType::get(ret, params, F->isVarArg());
        auto key = std::make_pair(F, FT);
        auto it = redecl.find(key);
        if (it != redecl.end())
          return it->second;
        Function *NF = Function::Create(FT, GlobalValue::ExternalLinkage,
                                        F->getName() + ".gbe_handle", &M);
        NF->setCallingConv(F->getCallingConv());
        redecl[key] = NF;
        origOf[NF] = F;
        return NF;
      }

      Function *original(Function *F) { return origOf.count(F) ? origOf[F] : F; }

      bool rewriteCall(CallInst *CI, Value *O, Value *N)
      {
        Function *F = CI->getCalledFunction();
        if (!F || !F->isDeclaration())
          return fail("gen7: image/sampler passed to a non-builtin call in " +
                      CI->getFunction()->getName());
        std::vector<Value *> args;
        for (Value *a : CI->args())
          args.push_back(newOf.count(a) ? newOf[a] : a);
        Type *ret = CI->getType();
        if (Function *L = Lib.getFunction(original(F)->getName()))
          ret = L->getReturnType();
        Function *NF = redeclare(F, ret, args);
        if (NF->getFunctionType()->getNumParams() != args.size())
          return fail("gen7: inconsistent builtin call " + F->getName());
        for (unsigned i = 0; i < args.size(); i++)
          if (NF->getFunctionType()->getParamType(i) != args[i]->getType())
            return fail("gen7: builtin " + F->getName() +
                        " called with different image/sampler types");
        CallInst *NC = CallInst::Create(NF, args, "", CI->getIterator());
        NC->setCallingConv(CI->getCallingConv());
        NC->setDebugLoc(CI->getDebugLoc());
        NC->takeName(CI);
        if (NC->getType() == CI->getType())
          CI->replaceAllUsesWith(NC);
        else if (!CI->use_empty())
          /* __translate_sampler_initializer: the result is a handle too */
          push(CI, NC);
        kill(CI);
        return true;
      }

      bool run()
      {
        std::vector<Function *> kernels;
        for (Function &F : M)
          if (!F.isDeclaration() && isKernelFunction(F))
            kernels.push_back(&F);
        for (Function *K : kernels)
          retypeKernel(K);

        /* sampler initializers: ptr-returning calls become sampler handles */
        if (Function *tsi = M.getFunction("__translate_sampler_initializer"))
          if (tsi->getReturnType()->isPointerTy()) {
            std::vector<CallInst *> calls;
            for (User *U : tsi->users())
              if (CallInst *CI = dyn_cast<CallInst>(U))
                calls.push_back(CI);
            Type *S = TargetExtType::get(M.getContext(), "spirv.Sampler");
            for (CallInst *CI : calls) {
              std::vector<Value *> args(CI->arg_begin(), CI->arg_end());
              Function *NF = redeclare(tsi, S, args);
              CallInst *NC = CallInst::Create(NF, args, "", CI->getIterator());
              push(CI, NC);
              kill(CI);
            }
          }

        while (!work.empty()) {
          auto [O, N] = work.back();
          work.pop_back();
          std::vector<User *> users(O->user_begin(), O->user_end());
          for (User *U : users) {
            if (Instruction *I = dyn_cast<Instruction>(U))
              if (deadSet.count(I))
                continue; /* already rewritten for another handle */
            CallInst *CI = dyn_cast<CallInst>(U);
            if (!CI) {
              std::string s;
              raw_string_ostream os(s);
              U->print(os);
              return fail("gen7: unsupported use of an image/sampler: " + os.str());
            }
            if (!rewriteCall(CI, O, N))
              return false;
          }
        }

        for (Instruction *I : dead)
          I->eraseFromParent();
        for (Function *K : oldKernels)
          K->eraseFromParent();
        /* per original: drop unused redeclarations, the used one (if any)
         * takes the original name */
        std::map<Function *, std::vector<Function *>> byOrig;
        for (auto &p : redecl)
          byOrig[p.first.first].push_back(p.second);
        for (auto &p : byOrig) {
          Function *F = p.first;
          if (!F->use_empty())
            return fail("gen7: " + F->getName() + " still used with pointer handles");
          Function *keep = NULL;
          for (Function *NF : p.second) {
            if (NF->use_empty()) {
              NF->eraseFromParent();
              continue;
            }
            if (keep)
              return fail("gen7: " + F->getName() + " used with conflicting handle types");
            keep = NF;
          }
          std::string name = F->getName().str();
          F->eraseFromParent();
          if (keep)
            keep->setName(name);
        }
        return true;
      }
    };
  } // anonymous namespace

  void prepareFullInlining(Module &M, bool internalize)
  {
    for (Function &F : M) {
      if (F.isDeclaration())
        continue;
      F.removeFnAttr(Attribute::OptimizeNone);
      F.removeFnAttr(Attribute::NoInline);
      for (BasicBlock &B : F)
        for (Instruction &I : B)
          if (CallBase *CB = dyn_cast<CallBase>(&I))
            CB->removeFnAttr(Attribute::NoInline);
      if (!isKernelFunction(F)) {
        F.addFnAttr(Attribute::AlwaysInline);
        if (internalize)
          F.setLinkage(GlobalValue::InternalLinkage);
      }
    }
  }

  bool restoreOpenCLHandleTypes(Module &M, Module &Lib, std::string &err)
  {
    if (!needsRewrite(M))
      return true;

    /* inline everything into the kernels, promote allocas, drop helpers */
    prepareFullInlining(M, true);
    runNewPM(M, NULL, "always-inline,function(sroa<modify-cfg>,mem2reg),globaldce", nullptr);

    Rewriter R(M, Lib, err);
    return R.run();
  }
} /* namespace gbe */
