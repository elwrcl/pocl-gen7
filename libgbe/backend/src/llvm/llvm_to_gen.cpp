/*
 * Copyright © 2012 Intel Corporation
 *
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
 *
 * Author: Benjamin Segovia <benjamin.segovia@intel.com>
 */

/**
 * \file llvm_to_gen.cpp
 * \author Benjamin Segovia <benjamin.segovia@intel.com>
 */

#include "llvm_includes.hpp"

#include "llvm/llvm_gen_backend.hpp"
#include "llvm/llvm_to_gen.hpp"
#include <llvm/IR/DiagnosticInfo.h>
#include <llvm/IR/DiagnosticPrinter.h>
#include "sys/cvar.hpp"
#include "sys/platform.hpp"
#include "ir/unit.hpp"
#include "ir/function.hpp"
#include "ir/structurizer.hpp"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <memory>
#include "llvm/Transforms/Scalar/SimpleLoopUnswitch.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/InitializePasses.h"
#include <mutex>
#include "llvm/Transforms/IPO/Inliner.h"
#include "llvm/Analysis/InlineCost.h"

namespace gbe
{
  BVAR(OCL_OUTPUT_CFG, false);
  BVAR(OCL_OUTPUT_CFG_ONLY, false);
  BVAR(OCL_OUTPUT_CFG_GEN_IR, false);
  using namespace llvm;

#if LLVM_VERSION_MAJOR * 10 + LLVM_VERSION_MINOR >= 37
  #define TARGETLIBRARY  TargetLibraryInfoImpl
#else
  #define TARGETLIBRARY  TargetLibraryInfo
#endif

  /*! Run new pass manager passes on the module. The legacy forms of most
   *  optimization passes are gone since LLVM 17; beignet's own passes still
   *  run in the legacy pass manager, so the pipelines alternate between the
   *  two. 'build' adds passes; 'pipeline' (textual, as for opt -passes=) is
   *  appended after them. TLII: library calls the optimizer may assume
   *  (beignet disables them all), NULL for the same.
   */
  void runNewPM(Module &mod, TargetLibraryInfoImpl *TLII, StringRef pipeline,
                function_ref<void(ModulePassManager &)> build)
  {
    LoopAnalysisManager LAM;
    FunctionAnalysisManager FAM;
    CGSCCAnalysisManager CGAM;
    ModuleAnalysisManager MAM;
    PassBuilder PB;
    TargetLibraryInfoImpl noLibCalls(Triple(mod.getTargetTriple()));
    noLibCalls.disableAllFunctions();
    if (!TLII)
      TLII = &noLibCalls;
    FAM.registerPass([&] { return TargetLibraryAnalysis(*TLII); });
    PB.registerModuleAnalyses(MAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM);
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

    ModulePassManager MPM;
    if (build)
      build(MPM);
    if (!pipeline.empty()) {
      if (auto err = PB.parsePassPipeline(MPM, pipeline)) {
        errs() << "beignet: bad pass pipeline '" << pipeline << "': " << toString(std::move(err)) << "\n";
        abort();
      }
    }
    MPM.run(mod, MAM);
  }

  /*! Inline everything we can (Gen has no calls); 20000 is beignet's old
   *  legacy-inliner threshold. */
  static void runInliner(Module &mod, TargetLibraryInfoImpl *TLII)
  {
    // The Gen backend has no calls: everything must be inlined. SPIR-V built
    // at -O0 carries noinline/optnone (and the inliner never inlines into an
    // optnone caller).
    prepareFullInlining(mod, false);
    runNewPM(mod, TLII, "", [](ModulePassManager &MPM) {
      MPM.addPass(ModuleInlinerWrapperPass(getInlineParams(20000)));
    });
  }

  void runFuntionPass(Module &mod, TARGETLIBRARY *libraryInfo, const DataLayout &DL)
  {
#if LLVM_VERSION_MAJOR * 10 + LLVM_VERSION_MINOR >= 37
    legacy::FunctionPassManager FPM(&mod);
#else
    FunctionPassManager FPM(&mod);
#endif

#if LLVM_VERSION_MAJOR * 10 + LLVM_VERSION_MINOR >= 37
#elif LLVM_VERSION_MAJOR * 10 + LLVM_VERSION_MINOR >= 36
    FPM.add(new DataLayoutPass());
#elif LLVM_VERSION_MAJOR * 10 + LLVM_VERSION_MINOR == 35
    FPM.add(new DataLayoutPass(DL));
#else
    FPM.add(new DataLayout(DL));
#endif

#if LLVM_VERSION_MAJOR * 10 + LLVM_VERSION_MINOR >= 35
    FPM.add(createVerifierPass(true));
#else
    FPM.add(createVerifierPass());
#endif
#if LLVM_VERSION_MAJOR * 10 + LLVM_VERSION_MINOR >= 37
    FPM.add(new TargetLibraryInfoWrapperPass(*libraryInfo));
#else
    FPM.add(new TargetLibraryInfo(*libraryInfo));
#endif
#if LLVM_VERSION_MAJOR * 10 + LLVM_VERSION_MINOR >= 38
    FPM.add(createTypeBasedAAWrapperPass());
    FPM.add(createBasicAAWrapperPass());
#else
    FPM.add(createTypeBasedAliasAnalysisPass());
    FPM.add(createBasicAliasAnalysisPass());
#endif
    FPM.add(createCFGSimplificationPass());
    FPM.add(createSROAPass());
    FPM.add(createEarlyCSEPass());

    FPM.doInitialization();
    for (Module::iterator I = mod.begin(),
           E = mod.end(); I != E; ++I)
      if (!I->isDeclaration())
        FPM.run(*I);
    FPM.doFinalization();
    // The legacy LowerExpectIntrinsic pass is gone (LLVM 21); it ran last here.
    runNewPM(mod, libraryInfo, "function(lower-expect)", nullptr);
  }

  void runModulePass(Module &mod, TARGETLIBRARY *libraryInfo, const DataLayout &DL, int optLevel, bool strictMath)
  {
#if LLVM_VERSION_MAJOR * 10 + LLVM_VERSION_MINOR >= 37
    legacy::PassManager MPM;
#else
    PassManager MPM;
#endif

#if LLVM_VERSION_MAJOR * 10 + LLVM_VERSION_MINOR >= 37
#elif LLVM_VERSION_MAJOR * 10 + LLVM_VERSION_MINOR >= 36
    MPM.add(new DataLayoutPass());
#elif LLVM_VERSION_MAJOR * 10 + LLVM_VERSION_MINOR == 35
    MPM.add(new DataLayoutPass(DL));
#else
    MPM.add(new DataLayout(DL));
#endif

#if LLVM_VERSION_MAJOR * 10 + LLVM_VERSION_MINOR >= 37
    MPM.add(new TargetLibraryInfoWrapperPass(*libraryInfo));
#else
    MPM.add(new TargetLibraryInfo(*libraryInfo));
#endif
#if LLVM_VERSION_MAJOR * 10 + LLVM_VERSION_MINOR >= 38
    MPM.add(createTypeBasedAAWrapperPass());
    MPM.add(createBasicAAWrapperPass());
#else
    MPM.add(createTypeBasedAliasAnalysisPass());
    MPM.add(createBasicAliasAnalysisPass());
#endif
    MPM.add(createIntrinsicLoweringPass());
    MPM.add(createBarrierNodupPass(false));   // remove noduplicate fnAttr before inlining.
    MPM.run(mod);

    runInliner(mod, libraryInfo);

    legacy::PassManager MPM2;
    MPM2.add(new TargetLibraryInfoWrapperPass(*libraryInfo));
    MPM2.add(createBarrierNodupPass(true));    // restore noduplicate fnAttr after inlining.
    MPM2.add(createStripAttributesPass(false));     // Strip unsupported attributes and calling conventions.
    MPM2.add(createSamplerFixPass());
    MPM2.run(mod);

    // The rest of beignet's legacy pipeline, same passes and order, in the
    // new pass manager. (The legacy createLoopUnrollPass(640) passed 640 as
    // the *optimization level*; O2 is what it amounted to.)
    const std::string sroaGvn = optLevel > 0 ? "sroa<modify-cfg>,gvn" : "";
    std::string pipeline =
      "globalopt,ipsccp,deadargelim,"
      "function(instcombine<no-verify-fixpoint>,simplifycfg),"
      "cgscc(function-attrs),"
      "function(";
    if (optLevel > 0)
      pipeline += "sroa<modify-cfg>,";
    pipeline +=
        "early-cse,jump-threading,correlated-propagation,simplifycfg,instcombine<no-verify-fixpoint>,"
        "tailcallelim,simplifycfg,reassociate,"
        "loop(loop-rotate),loop-mssa(licm),loop-mssa(simple-loop-unswitch<nontrivial>),"
        "instcombine<no-verify-fixpoint>,loop(indvars,loop-idiom,loop-deletion),loop-unroll<O2>";
    if (optLevel > 0)
      pipeline += "," + sroaGvn;
    pipeline += ")";
    runNewPM(mod, libraryInfo, pipeline, nullptr);

    // FIXME Workaround: we find that CustomLoopUnroll may increase register pressure greatly,
    // and it may even make som cl kernel cannot compile because of limited scratch memory for spill.
    // As we observe this under strict math. So we disable CustomLoopUnroll if strict math is enabled.
    if (!strictMath) {
#if !defined(__ANDROID__)
      legacy::PassManager MPM3;
      MPM3.add(new TargetLibraryInfoWrapperPass(*libraryInfo));
      MPM3.add(createCustomLoopUnrollPass()); //1024, 32, 1024, 512)); //Unroll loops
      MPM3.run(mod);
#endif
      runNewPM(mod, libraryInfo, optLevel > 0 ? "function(loop-unroll<O2>," + sroaGvn + ")"
                                              : "function(loop-unroll<O2>)", nullptr);
    }

    pipeline =
      "function(memcpyopt,sccp,instcombine<no-verify-fixpoint>,jump-threading,correlated-propagation,"
      "dse,adce,simplifycfg,instcombine<no-verify-fixpoint>),strip-dead-prototypes";
    if (optLevel > 0)
      pipeline += ",globaldce,constmerge";
    runNewPM(mod, libraryInfo, pipeline, nullptr);
  }


#if LLVM_VERSION_MAJOR * 10 + LLVM_VERSION_MINOR >= 37
#define OUTPUT_BITCODE(STAGE, MOD)  do {         \
  legacy::PassManager passes__;           \
   if (OCL_OUTPUT_LLVM_##STAGE) {                \
     passes__.add(createPrintModulePass(*o));    \
     passes__.run(MOD);                          \
   }                                             \
 }while(0)
#elif LLVM_VERSION_MAJOR * 10 + LLVM_VERSION_MINOR >= 35
#define OUTPUT_BITCODE(STAGE, MOD)  do {         \
   PassManager passes__;           \
   if (OCL_OUTPUT_LLVM_##STAGE) {                \
     passes__.add(createPrintModulePass(*o));    \
     passes__.run(MOD);                          \
   }                                             \
 }while(0)
#else
#define OUTPUT_BITCODE(STAGE, MOD)  do {         \
   PassManager passes__;           \
   if (OCL_OUTPUT_LLVM_##STAGE) {                \
     passes__.add(createPrintModulePass(&*o));   \
     passes__.run(MOD);                          \
   }                                             \
 }while(0)
#endif

  BVAR(OCL_OUTPUT_LLVM_BEFORE_LINK, false);
  BVAR(OCL_OUTPUT_LLVM_AFTER_LINK, false);
  BVAR(OCL_OUTPUT_LLVM_AFTER_GEN, false);

  class gbeDiagnosticContext
  {
  public:
    gbeDiagnosticContext() : _str(""), messages(_str), printer(messages), _has_errors(false) {}
    void process(const llvm::DiagnosticInfo &diagnostic)
    {
      if (diagnostic.getSeverity() != DS_Remark) { // avoid noise from function inlining remarks
        diagnostic.print(printer);
      }
      if (diagnostic.getSeverity() == DS_Error) {
        _has_errors = true;
      }
    }
    std::string str(){return messages.str();}
    bool has_errors(){return _has_errors;}
  private:
    std::string _str;
    llvm::raw_string_ostream messages;
    llvm::DiagnosticPrinterRawOStream printer;
    bool _has_errors;
  };
  
  void gbeDiagnosticHandler(const llvm::DiagnosticInfo *diagnostic, void *context)
  {
    gbeDiagnosticContext *dc = reinterpret_cast<gbeDiagnosticContext*>(context);
    dc->process(*diagnostic);
  }

  bool llvmToGen(ir::Unit &unit, const void* module,
                 int optLevel, bool strictMath, int profiling, std::string &errors)
  {
    // beignet's own passes run in the legacy pass manager, which looks their
    // required analyses (LoopInfo, LoopSimplify, LCSSA, SCEV, ...) up in the
    // global PassRegistry. The legacy standard passes used to register those
    // as a side effect; since they moved to the new pass manager (LLVM 17)
    // nothing does, and a second compile in the same process failed with
    // "Pass ... is not initialized".
    static std::once_flag passesRegistered;
    std::call_once(passesRegistered, [] {
      PassRegistry &registry = *PassRegistry::getPassRegistry();
      initializeCore(registry);
      initializeAnalysis(registry);
      initializeTransformUtils(registry);
      initializeScalarOpts(registry);
    });

    std::string errInfo;
    std::unique_ptr<llvm::raw_fd_ostream> o = NULL;
    if (OCL_OUTPUT_LLVM_BEFORE_LINK || OCL_OUTPUT_LLVM_AFTER_LINK || OCL_OUTPUT_LLVM_AFTER_GEN)
      o = std::unique_ptr<llvm::raw_fd_ostream>(new llvm::raw_fd_ostream(fileno(stdout), false));

    Module* cl_mod = NULL;
    if (module) {
      cl_mod = reinterpret_cast<Module*>(const_cast<void*>(module));
    }

    if (!cl_mod) return false;

    OUTPUT_BITCODE(BEFORE_LINK, (*cl_mod));
#if LLVM_VERSION_MAJOR * 10 + LLVM_VERSION_MINOR >= 37
    legacy::PassManager passes__;
#else
    PassManager passes__;
#endif
    //run ExpandConstantExprPass before collectDeviceEnqueueInfo
    //to simplify the analyze of block.
    passes__.add(createExpandConstantExprPass());    // constant prop may generate ConstantExpr
    passes__.run(*cl_mod);
    /* Must call before materialize when link */
    collectDeviceEnqueueInfo(cl_mod, unit);

    std::unique_ptr<Module> M;

    /* runBitCodeLinker returns NULL both for a program without kernels,
     * which is valid, and when linking beignet's library fails, which
     * used to build an empty program reported as a success. */
    bool hasKernels = false;
    for (const Function &F : *cl_mod)
      if (isKernelFunction(F)) {
        hasKernels = true;
        break;
      }

    /* Before do any thing, we first filter in all CL functions in bitcode. */
    /* Also set unit's pointer size in runBitCodeLinker */
    M.reset(runBitCodeLinker(cl_mod, strictMath, unit));

    if (M.get() == 0) {
      if (!hasKernels)
        return true;
      errors = "error: linking with the OpenCL C builtin library failed "
               "(an undefined or unsupported builtin function)\n";
      return false;
    }

    Module &mod = *M.get();
    const DataLayout &DL = mod.getDataLayout();
    
    gbeDiagnosticContext dc;
#if LLVM_VERSION_MAJOR >= 6
    mod.getContext().setDiagnosticHandlerCallBack(&gbeDiagnosticHandler,&dc);
#else
    mod.getContext().setDiagnosticHandler(&gbeDiagnosticHandler,&dc);
#endif

#if LLVM_VERSION_MAJOR * 10 + LLVM_VERSION_MINOR >= 37
    mod.setDataLayout(DL);
#endif
    Triple TargetTriple(mod.getTargetTriple());
    TARGETLIBRARY *libraryInfo = new TARGETLIBRARY(TargetTriple);
    libraryInfo->disableAllFunctions();

    OUTPUT_BITCODE(AFTER_LINK, mod);

    runFuntionPass(mod, libraryInfo, DL);
    runModulePass(mod, libraryInfo, DL, optLevel, strictMath);
#if LLVM_VERSION_MAJOR * 10 + LLVM_VERSION_MINOR >= 37
    legacy::PassManager passes;
#else
    PassManager passes;
#endif
#if LLVM_VERSION_MAJOR * 10 + LLVM_VERSION_MINOR >= 37
#elif LLVM_VERSION_MAJOR * 10 + LLVM_VERSION_MINOR >= 36
    passes.add(new DataLayoutPass());
#elif LLVM_VERSION_MAJOR * 10 + LLVM_VERSION_MINOR == 35
    passes.add(new DataLayoutPass(DL));
#else
    passes.add(new DataLayout(DL));
#endif
    // Print the code before further optimizations
    {
      legacy::PassManager before;
      before.add(createIntrinsicLoweringPass());
      before.add(createStripAttributesPass(true));     // Strip unsupported attributes and calling conventions.
      before.run(mod);
    }
    runInliner(mod, libraryInfo);
#if LLVM_VERSION_MAJOR * 10 + LLVM_VERSION_MINOR >= 37
    passes.add(createSROAPass());
#else
    passes.add(createScalarReplAggregatesPass(64, true, -1, -1, 64));
#endif
    passes.add(createLoadStoreOptimizationPass());
    passes.add(createInstSimplifyLegacyPass());
    passes.add(createPromoteMemoryToRegisterPass());
    if(optLevel > 0)
      passes.add(createGVNPass());                 // Remove redundancies
    passes.add(createPrintfParserPass(unit));
    passes.add(createExpandConstantExprPass());    // expand ConstantExpr
#if LLVM_VERSION_MAJOR >= 8
    passes.add(createFNegLoweringPass());          // scalarize/GenWriter only know fsub -0.0
    passes.add(createLateIntrinsicLoweringPass()); // fshl/fshr, *_sat from InstCombine
#endif
    passes.add(createScalarizePass());             // Expand all vector ops
    passes.add(createExpandLargeIntegersPass());   // legalize large integer operation
    passes.add(createInstructionCombiningPass());  // legalize will generate some silly instructions
    passes.add(createInstSimplifyLegacyPass());   // propagate constant after scalarize/legalize
    passes.add(createExpandConstantExprPass());    // constant prop may generate ConstantExpr
    passes.add(createPromoteIntegersPass());       // align integer size to power of two
    passes.add(createRemoveGEPPass(unit));         // Constant prop may generate gep
    passes.add(createDeadCodeEliminationPass());   // Remove simplified instructions
    passes.add(createCFGSimplificationPass());     // Merge & remove BBs
    passes.add(createLowerSwitchPass());           // simplify cfg will generate switch-case instruction
    if (profiling) {
      passes.add(createProfilingInserterPass(profiling, unit));     // insert the time stamp for profiling.
    }
#if LLVM_VERSION_MAJOR >= 8
    passes.add(createFNegLoweringPass());          // InstCombine above turns fsub -0.0 back into fneg
    passes.add(createLateIntrinsicLoweringPass()); // ...and re-forms fshl/fshr, *_sat
#endif
    passes.add(createScalarizePass());             // Expand all vector ops

    passes.run(mod);
    // CFG dumps (debug): the legacy printer passes are gone, so they run in
    // the new pass manager between beignet's last pass and GenWriter.
    if (OCL_OUTPUT_CFG || OCL_OUTPUT_CFG_ONLY)
      runNewPM(mod, libraryInfo, OCL_OUTPUT_CFG ? "function(dot-cfg)" : "function(dot-cfg-only)", nullptr);
    legacy::PassManager genPasses;
    genPasses.add(createGenPass(unit));
    genPasses.run(mod);
    errors = dc.str();
    if(dc.has_errors()){
      unit.setValid(false);
      delete libraryInfo;
      return true;
    }

    // Print the code extra optimization passes
    OUTPUT_BITCODE(AFTER_GEN, mod);

    const ir::Unit::FunctionSet& fs = unit.getFunctionSet();
    ir::Unit::FunctionSet::const_iterator iter = fs.begin();
    while(iter != fs.end())
    {
      ir::CFGStructurizer *structurizer = new ir::CFGStructurizer(iter->second);
      structurizer->StructurizeBlocks();
      delete structurizer;
      if (OCL_OUTPUT_CFG_GEN_IR)
        iter->second->outputCFG();
      iter++;
    }


    delete libraryInfo;
    return true;
  }
} /* namespace gbe */
