/* pocl_llvm_wg.cc: part of pocl LLVM API dealing with parallel.bc,
   optimization passes and codegen.

   Copyright (c) 2013 Kalle Raiskila
                 2013-2019 Pekka Jääskeläinen
                 2023 Pekka Jääskeläinen / Intel Finland Oy

   Permission is hereby granted, free of charge, to any person obtaining a copy
   of this software and associated documentation files (the "Software"), to deal
   in the Software without restriction, including without limitation the rights
   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
   copies of the Software, and to permit persons to whom the Software is
   furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
   THE SOFTWARE.
*/

#include "AutomaticLocals.h"
#include "config.h"
#include "pocl.h"
#include "pocl_cache.h"
#include "pocl_file_util.h"
#include "pocl_llvm_api.h"
#include "pocl_spir.h"
#include "pocl_util.h"

#include <iostream>
#include <map>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#include "CompilerWarnings.h"
IGNORE_COMPILER_WARNING("-Wunused-parameter")

#include <llvm/Support/Casting.h>
#ifdef LLVM_OLDER_THAN_14_0
#include <llvm/Support/TargetRegistry.h>
#else
#include <llvm/MC/TargetRegistry.h>
#endif
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/CommandLine.h>

#include <llvm/ADT/Triple.h>
#include <llvm/ADT/StringRef.h>

#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>

#include <llvm/Target/TargetOptions.h>
#include <llvm/Target/TargetMachine.h>

#include <llvm/Transforms/IPO/PassManagerBuilder.h>
#include <llvm/Transforms/Utils/Cloning.h>

#include <llvm/PassRegistry.h>
#include <llvm/PassInfo.h>

#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/Analysis/TargetTransformInfo.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/CodeGen/MachineModuleInfo.h>

#define PassManager legacy::PassManager

#include "linker.h"

// Enable to get the LLVM pass execution timing report dumped to console after
// each work-group IR function generation. Requires LLVM > 7.
// #define DUMP_LLVM_PASS_TIMINGS

#include <llvm/IR/PassTimingInfo.h>
#define CODEGEN_FILE_TYPE_NS llvm

using namespace llvm;

/**
 * Prepare the kernel compiler passes.
 *
 * The passes are created only once per program run per device.
 * The returned pass manager should not be modified, only the Module
 * should be optimized using it.
 */

static std::map<cl_device_id, llvm::TargetMachine *> targetMachines;
static std::map<cl_device_id, PassManager *> kernelPasses;

/* FIXME: these options should come from the cl_device, and
 * cl_program's options. */
static llvm::TargetOptions GetTargetOptions() {
  llvm::TargetOptions Options;
#ifdef HOST_FLOAT_SOFT_ABI
  Options.FloatABIType = FloatABI::Soft;
#else
  Options.FloatABIType = FloatABI::Hard;
#endif
  return Options;
}

void clearTargetMachines() {
  for (auto i = targetMachines.begin(), e = targetMachines.end(); i != e; ++i) {
    delete (llvm::TargetMachine *)i->second;
  }
  targetMachines.clear();
}

void clearKernelPasses() {
  for (auto i = kernelPasses.begin(), e = kernelPasses.end();
       i != e; ++i) {
    PassManager *pm = (PassManager *)i->second;
    delete pm;
  }

  kernelPasses.clear();
}

// Returns the TargetMachine instance or zero if no triple is provided.
static TargetMachine *GetTargetMachine(cl_device_id device, Triple &triple) {

  if (targetMachines.find(device) != targetMachines.end())
    return targetMachines[device];

  std::string Error;
  // Triple TheTriple(device->llvm_target_triplet);

  std::string MCPU = device->llvm_cpu ? device->llvm_cpu : "";

  const Target *TheTarget = TargetRegistry::lookupTarget("", triple, Error);

  // In LLVM 3.4 and earlier, the target registry falls back to
  // the cpp backend in case a proper match was not found. In
  // that case simply do not use target info in the compilation
  // because it can be an off-tree target not registered at
  // this point (read: TCE).
  if (!TheTarget || TheTarget->getName() == std::string("cpp")) {
    return 0;
  }

  TargetMachine *TM = TheTarget->createTargetMachine(
      triple.getTriple(), MCPU, StringRef("+m,+f"), GetTargetOptions(), Reloc::PIC_,      
      CodeModel::Small, CodeGenOpt::Aggressive);

  assert(TM != NULL && "llvm target has no targetMachine constructor");
  if (device->ops->init_target_machine)
    device->ops->init_target_machine(device->data, TM);
  targetMachines[device] = TM;

  return TM;
}
/* helpers copied from LLVM opt END */

static PassManager &kernel_compiler_passes(cl_device_id device) {

  PassManager *Passes = nullptr;
  PassRegistry *Registry = nullptr;

  if (kernelPasses.find(device) != kernelPasses.end()) {
    return *kernelPasses[device];
  }

  bool SPMDDevice = device->spmd;

  Registry = PassRegistry::getPassRegistry();

  Passes = new PassManager();

  // Need to setup the target info for target specific passes. */
  Triple triple(device->llvm_target_triplet);
  TargetMachine *Machine = GetTargetMachine(device, triple);

  if (Machine)
    Passes->add(
        createTargetTransformInfoWrapperPass(Machine->getTargetIRAnalysis()));


  /* Disables automated generation of libcalls from code patterns.
     TCE doesn't have a runtime linker which could link the libs later on.
     Also the libcalls might be harmful for WG autovectorization where we
     want to try to vectorize the code it converts to e.g. a memset or
     a memcpy */
  TargetLibraryInfoImpl TLII(triple);
  TLII.disableAllFunctions();
  Passes->add(new TargetLibraryInfoWrapperPass(TLII));

  /* The kernel compiler passes to run, in order.

     Notes about the kernel compiler phase ordering:

     -mem2reg first because we get unoptimized output from Clang where all
     variables are allocas. Avoid context saving the allocas and make them
     more readable by calling -mem2reg at the beginning.

     -implicit-cond-barriers after -implicit-loop-barriers because the latter
     can inject barriers to loops inside conditional regions after which the
     peeling should be avoided by injecting the implicit conditional barriers.

     -loop-barriers, -barriertails, and -barriers should be ran after the
     implicit barrier injection passes so they "normalize" the implicit
     barriers also.

     -phistoallocas before -workitemloops as otherwise it cannot inject context
     restore code (PHIs need to be at the beginning of the BB and so one cannot
     context restore them with non-PHI code if the value is needed in another
     PHI).

     -automatic-locals after inline and always-inline; if we have a kernel
     that calls a non-kernel, and the non-kernel uses an automatic local
     (= GlobalVariable in LLVM), the 'automatic-locals' will skip processing
     of the non-kernel function, and the kernel function appears to it as not
     having any locals. Therefore the local variable remains a GV instead of
     being transformed into a kernel argument. This can lead to surprising
     result, as the final object ELF will contain a static variable, so the
     program will work with single-threaded execution, but multiple CPU
     threads will overwrite the static variable and produce garbage results.
  */

  std::vector<std::string> passes;
  #ifdef BUILD_VORTEX
    passes.push_back("vortex-mno-riscv-attribute");
    passes.push_back("vortex-printfs");
    //passes.push_back("print-module");
  #endif 

  passes.push_back("inline-kernels");
  passes.push_back("remove-optnone");
  passes.push_back("optimize-wi-func-calls");
  passes.push_back("handle-samplers");
  passes.push_back("infer-address-spaces");
  passes.push_back("workitem-handler-chooser");
  passes.push_back("mem2reg");
  passes.push_back("domtree");

  if (SPMDDevice) {
    passes.push_back("flatten-inline-all");
    passes.push_back("always-inline");
  } else {
    passes.push_back("flatten-globals");
    passes.push_back("flatten-barrier-subs");
    passes.push_back("always-inline");
    passes.push_back("inline");
  }

  // this must be done AFTER inlining, see note above
  passes.push_back("automatic-locals");

  // It should be now safe to run -O3 over the single work-item kernel
  // as the barrier has the attributes preventing illegal motions and
  // duplication. Let's do it to clean up the code for later passes.
  // Especially the WI context structures get needlessly bloated in case there
  // is dead code lying around.
  passes.push_back("STANDARD_OPTS");

  if (!SPMDDevice) {
    passes.push_back("simplifycfg");
    passes.push_back("loop-simplify");
    passes.push_back("uniformity");
    passes.push_back("phistoallocas");
    passes.push_back("isolate-regions");
    passes.push_back("implicit-loop-barriers");
    passes.push_back("implicit-cond-barriers");
    passes.push_back("loop-barriers");
    passes.push_back("barriertails");
    passes.push_back("barriers");
    passes.push_back("isolate-regions");
    passes.push_back("wi-aa");
    passes.push_back("workitemrepl");
    //passes.push_back("print-module");
    passes.push_back("subcfgformation");
    // // subcfgformation before workitemloops, as wiloops creates the loops for
    // // kernels without barriers, but after the transformation the kernel looks
    // // like it has barriers, so subcfg would do its thing.
    passes.push_back("workitemloops");
    // // Remove the (pseudo) barriers.   They have no use anymore due to the
    // // work-item loop control taking care of them.
    #ifdef BUILD_VORTEX
      passes.push_back("vortex-barriers");
    #endif 
    passes.push_back("remove-barriers");
  }

  // IMPORTANT:
  // Add the work group launcher functions and privatize the pseudo variable
  // (local id) accesses. We have to do this late because we rely on aggressive
  // inlining to expose the _{local,group}_id accesses which will be replaced
  // with context struct accesses. TODO: A cleaner and a more robust way would
  // be to add hidden context struct parameters to the builtins that need the
  // context data and fix the calls early.
  if (device->workgroup_pass) {
    passes.push_back("workgroup");
    passes.push_back("always-inline");
  }

  // Attempt to move all allocas to the entry block to avoid the need for
  // dynamic stack which is problematic for some architectures.
  passes.push_back("allocastoentry");

  // Later passes might get confused (and expose possible bugs in them) due to
  // UNREACHABLE blocks left by repl. So let's clean up the CFG before running
  // the standard LLVM optimizations.
  passes.push_back("simplifycfg");

#if 0
  passes.push_back("print-module");
  passes.push_back("dot-cfg");
#endif

  passes.push_back("STANDARD_OPTS");

  // Due to unfortunate phase-ordering problems with store sinking,
  // loop deletion does not always apply when executing -O3 only
  // once. Cherry pick the optimization to rerun here.
  passes.push_back("loop-deletion");

  passes.push_back("remove-barriers");

  // Now actually add the listed passes to the PassManager.
  for (unsigned i = 0; i < passes.size(); ++i) {
    // This is (more or less) -O3.
    if (passes[i] == "STANDARD_OPTS") {
      PassManagerBuilder Builder;
      Builder.OptLevel = 3;
      Builder.SizeLevel = 0;

      // These need to be setup in addition to invoking the passes
      // to get the vectorizers initialized properly. Assume SPMD
      // devices do not want to vectorize intra work-item at this
      // stage.
      if ((CurrentWgMethod == "loopvec" || CurrentWgMethod == "cbs") &&
          !SPMDDevice) {
        Builder.LoopVectorize = true;
        Builder.SLPVectorize = true;
      } else {
        Builder.LoopVectorize = false;
        Builder.SLPVectorize = false;
      }
      Builder.VerifyInput = LLVM_VERIFY_MODULE_DEFAULT > 0;
      Builder.VerifyOutput = LLVM_VERIFY_MODULE_DEFAULT > 0;
      Builder.populateModulePassManager(*Passes);
      continue;
    }
    if (passes[i] == "automatic-locals") {
      Passes->add(pocl::createAutomaticLocalsPass(device->autolocals_to_args));
      continue;
    }

    const PassInfo *PIs = Registry->getPassInfo(StringRef(passes[i]));
    if (PIs) {
      // std::cout << "-"<<passes[i] << " ";
      Pass *thispass = PIs->createPass();
      Passes->add(thispass);
    } else {
      std::cerr << "Failed to create kernel compiler pass " << passes[i]
                << std::endl;
      POCL_ABORT("FAIL\n");
    }
  }

  kernelPasses[device] = Passes;
  return *Passes;
}

void pocl_destroy_llvm_module(void *modp, cl_context ctx) {

  PoclLLVMContextData *llvm_ctx = (PoclLLVMContextData *)ctx->llvm_context_data;
  PoclCompilerMutexGuard lockHolder(&llvm_ctx->Lock);

  llvm::Module *mod = (llvm::Module *)modp;
  if (mod) {
    delete mod;
    --llvm_ctx->number_of_IRs;
  }
}

namespace pocl {
class ProgramWithContext {

  llvm::LLVMContext LLVMCtx;
  std::unique_ptr<llvm::Module> ProgramBC;
  std::unique_ptr<llvm::Module> ProgramGVarsNonKernelsBC;
  std::mutex Lock;
  unsigned Num = 0;

public:

  bool init(const char *ProgramBcBytes,
            size_t ProgramBcSize,
            char* LinkinOutputBCPath) {
    Num = 0;
    llvm::Module *P = parseModuleIRMem(ProgramBcBytes, ProgramBcSize, &LLVMCtx);
    if (P == nullptr)
      return false;
    ProgramBC.reset(P);

    ProgramGVarsNonKernelsBC.reset(
        new llvm::Module(llvm::StringRef("program_gvars.bc"), LLVMCtx));

    ProgramGVarsNonKernelsBC->setTargetTriple(ProgramBC->getTargetTriple());
    ProgramGVarsNonKernelsBC->setDataLayout(ProgramBC->getDataLayout());

    if (!moveProgramScopeVarsOutOfProgramBc(&LLVMCtx, ProgramBC.get(),
                                            ProgramGVarsNonKernelsBC.get(),
                                            SPIR_ADDRESS_SPACE_LOCAL))
      return false;

    pocl_cache_tempname(LinkinOutputBCPath, ".bc", NULL);
    int r = pocl_write_module(ProgramGVarsNonKernelsBC.get(),
                              LinkinOutputBCPath, 0);
    if (r != 0) {
      POCL_MSG_ERR("ProgramWithContext->init: failed to write module\n");
      return false;
    }

    if (pocl_get_bool_option("POCL_LLVM_VERIFY", LLVM_VERIFY_MODULE_DEFAULT)) {
      std::string ErrorLog;
      llvm::raw_string_ostream Errs(ErrorLog);
      if (llvm::verifyModule(*ProgramGVarsNonKernelsBC.get(), &Errs)) {
        POCL_MSG_ERR("Failed to verify Program GVars Module:\n%s\n",
                     ErrorLog.c_str());
        return false;
      }
    }

    return true;
  }

  bool getBitcodeForKernel(const char* KernelName,
                           char* OutputPath,
                           std::string *BuildLog) {
    std::lock_guard<std::mutex> LockGuard(Lock);

    // Create an empty Module and copy only the kernel+callgraph from
    // program.bc.
    std::unique_ptr<llvm::Module> KernelBC(
        new llvm::Module(llvm::StringRef("parallel_bc"), LLVMCtx));

    KernelBC->setTargetTriple(ProgramBC->getTargetTriple());
    KernelBC->setDataLayout(ProgramBC->getDataLayout());

    copyKernelFromBitcode(KernelName, KernelBC.get(), ProgramBC.get(), nullptr);

    if (pocl_get_bool_option("POCL_LLVM_VERIFY", LLVM_VERIFY_MODULE_DEFAULT)) {
      llvm::raw_string_ostream Errs(*BuildLog);
      if (llvm::verifyModule(*KernelBC.get(), &Errs)) {
        POCL_MSG_ERR("Failed to verify Kernel Module:\n%s\n",
                     BuildLog->c_str());
        BuildLog->append("Failed to verify Kernel Module\n");
        return false;
      }
    }

    pocl_cache_tempname(OutputPath, ".bc", NULL);
    int r = pocl_write_module(KernelBC.get(), OutputPath, 0);
    if (r != 0) {
      POCL_MSG_ERR("getBitcodeForKernel: failed to write module\n");
      BuildLog->append("getBitcodeForKernel: failed to write module\n");
      return false;
    }
    return true;
  }
};

static int convertBitcodeToSpv(char* TempBitcodePath,
                               std::string *BuildLog,
                               char **SpirvContent,
                               uint64_t *SpirvSize) {

  char TempSpirvPath[POCL_MAX_PATHNAME_LENGTH];

// max bytes in output of 'llvm-spirv'
#define MAX_OUTPUT_BYTES 65536

//   --spirv-ext=<+SPV_extenstion1_name,-SPV_extension2_name>
//   Specify list of allowed/disallowed extensions
#define ALLOW_EXTS                                                             \
  "--spirv-ext=+SPV_INTEL_subgroups,+SPV_INTEL_usm_storage_classes,+SPV_"      \
  "INTEL_arbitrary_precision_integers,+SPV_INTEL_arbitrary_precision_fixed_"   \
  "point,+SPV_INTEL_arbitrary_precision_floating_point,+SPV_INTEL_kernel_"     \
  "attributes"
  /*
  possibly useful:
    "+SPV_INTEL_unstructured_loop_controls,"
    "+SPV_INTEL_blocking_pipes,"
    "+SPV_INTEL_function_pointers,"
    "+SPV_INTEL_io_pipes,"
    "+SPV_INTEL_inline_assembly,"
    "+SPV_INTEL_optimization_hints,"
    "+SPV_INTEL_float_controls2,"
    "+SPV_INTEL_vector_compute,"
    "+SPV_INTEL_fast_composite,"
    "+SPV_INTEL_variable_length_array,"
    "+SPV_INTEL_fp_fast_math_mode,"
    "+SPV_INTEL_long_constant_composite,"
    "+SPV_INTEL_memory_access_aliasing,"
    "+SPV_INTEL_runtime_aligned,"
    "+SPV_INTEL_arithmetic_fence,"
    "+SPV_INTEL_bfloat16_conversion,"
    "+SPV_INTEL_global_variable_decorations,"
    "+SPV_INTEL_non_constant_addrspace_printf,"
    "+SPV_INTEL_hw_thread_queries,"
    "+SPV_INTEL_complex_float_mul_div,"
    "+SPV_INTEL_split_barrier,"
    "+SPV_INTEL_masked_gather_scatter"

  probably not useful:
    "+SPV_INTEL_media_block_io,+SPV_INTEL_device_side_avc_motion_estimation,"
    "+SPV_INTEL_fpga_loop_controls,+SPV_INTEL_fpga_memory_attributes,"
    "+SPV_INTEL_fpga_memory_accesses,"
    "+SPV_INTEL_fpga_reg,+SPV_INTEL_fpga_buffer_location,"
    "+SPV_INTEL_fpga_cluster_attributes,"
    "+SPV_INTEL_loop_fuse,"
    "+SPV_INTEL_optnone," // this one causes crash
    "+SPV_INTEL_fpga_dsp_control,"
    "+SPV_INTEL_fpga_invocation_pipelining_attributes,"
    "+SPV_INTEL_token_type,"
    "+SPV_INTEL_debug_module,"
    "+SPV_INTEL_joint_matrix,"
  */
  pocl_cache_tempname(TempSpirvPath, ".spirv", NULL);
  char LLVMspirv[] = LLVM_SPIRV;
  char AllowedExtOption[] = ALLOW_EXTS;
  // TODO ze_device_module_properties_t.spirvVersionSupported
  char MaxSPIRVOption[] = "--spirv-max-version=1.2";
#ifdef LLVM_OPAQUE_POINTERS
  char OpaquePtrsOption[] = "--opaque-pointers";
#endif
  char OutputOption[] = { '-', 'o', 0 };
  char *CmdArgs[] = { LLVMspirv, AllowedExtOption,
#ifdef LLVM_OPAQUE_POINTERS
                      OpaquePtrsOption,
#endif
                      MaxSPIRVOption, OutputOption,
                      TempSpirvPath, TempBitcodePath, NULL };
  char CapturedOutput[MAX_OUTPUT_BYTES];
  size_t CapturedBytes = MAX_OUTPUT_BYTES;

  int r =
      pocl_run_command_capture_output(CapturedOutput, &CapturedBytes, CmdArgs);
  if (r != 0) {
    BuildLog->append("llvm-spirv failed with output:\n");
    std::string Captured(CapturedOutput, CapturedBytes);
    BuildLog->append(Captured);
    return -1;
  }

  r = pocl_read_file(TempSpirvPath, SpirvContent, SpirvSize);
  if (r != 0) {
    BuildLog->append("failed to read output file from llvm-spirv\n");
    return -1;
  }

  if (pocl_get_bool_option("POCL_LEAVE_KERNEL_COMPILER_TEMP_FILES", 0) == 0) {
    pocl_remove(TempBitcodePath);
    pocl_remove(TempSpirvPath);
  } else {
    POCL_MSG_PRINT_GENERAL("LLVM SPIR-V conversion tempfiles: %s -> %s",
                           TempBitcodePath, TempSpirvPath);
  }
  return 0;
}

} // namespace pocl

void *pocl_llvm_create_context_for_program(const char *ProgramBcBytes,
                                           size_t ProgramBcSize,
                                           char **LinkinSpirvContent,
                                           uint64_t *LinkinSpirvSize) {
  assert(ProgramBcBytes);
  assert(ProgramBcSize > 0);

  char TempBitcodePath[POCL_MAX_PATHNAME_LENGTH];

  pocl::ProgramWithContext *P = new pocl::ProgramWithContext;
  // parse the program's bytes into a llvm::Module
  if (P == nullptr ||
      !P->init(ProgramBcBytes, ProgramBcSize, TempBitcodePath)) {
    POCL_MSG_ERR("failed to create program for context");
    return nullptr;
  }

  std::string BuildLog;
  if (pocl::convertBitcodeToSpv(TempBitcodePath, &BuildLog,
                                LinkinSpirvContent, LinkinSpirvSize) != 0) {
    POCL_MSG_ERR("failed to create program for context, log:%s\n",
                 BuildLog.c_str());
    return nullptr;
  }

  return (void *)P;
}

void pocl_llvm_release_context_for_program(void *ProgCtx) {
  if (ProgCtx == nullptr)
    return;
  pocl::ProgramWithContext *P = (pocl::ProgramWithContext *)ProgCtx;
  delete P;
}

// extract SPIRV of a single Kernel from a program
int pocl_llvm_extract_kernel_spirv(void* ProgCtx,
                                   const char* KernelName,
                                   void* BuildLogStr,
                                   char **SpirvContent,
                                   uint64_t *SpirvSize) {

  POCL_MEASURE_START(extractKernel);

  std::string *BuildLog = (std::string *)BuildLogStr;

  char TempBitcodePath[POCL_MAX_PATHNAME_LENGTH];
  pocl::ProgramWithContext *P = (pocl::ProgramWithContext *)ProgCtx;
  if (!P->getBitcodeForKernel(KernelName, TempBitcodePath, BuildLog)) {
    return -1;
  }

  int r = pocl::convertBitcodeToSpv(TempBitcodePath, BuildLog,
                                    SpirvContent, SpirvSize);

  POCL_MEASURE_FINISH(extractKernel);
  return r;
}

int pocl_llvm_generate_workgroup_function_nowrite(
    unsigned DeviceI, cl_device_id Device, cl_kernel Kernel,
    _cl_command_node *Command, void **Output, int Specialize) {

  _cl_command_run *RunCommand = &Command->command.run;
  cl_program Program = Kernel->program;
  cl_context ctx = Program->context;
  PoclLLVMContextData *PoCLLLVMContext =
      (PoclLLVMContextData *)ctx->llvm_context_data;
  PoclCompilerMutexGuard lockHolder(&PoCLLLVMContext->Lock);
  llvm::LLVMContext *LLVMContext = PoCLLLVMContext->Context;

#ifdef DEBUG_POCL_LLVM_API
  printf("### calling the kernel compiler for kernel %s local_x %zu "
         "local_y %zu local_z %zu parallel_filename: %s\n",
         kernel->name, local_x, local_y, local_z, parallel_bc_path);
#endif
  llvm::Module *ProgramBC = (llvm::Module *)Program->llvm_irs[DeviceI];

  // Create an empty Module and copy only the kernel+callgraph from
  // program.bc.
  llvm::Module *ParallelBC =
      new llvm::Module(StringRef("parallel_bc"), *LLVMContext);

  ParallelBC->setTargetTriple(ProgramBC->getTargetTriple());
  ParallelBC->setDataLayout(ProgramBC->getDataLayout());

  copyKernelFromBitcode(Kernel->name, ParallelBC, ProgramBC,
                        Device->device_aux_functions);

  // Set to true to generate a global offset 0 specialized WG function.
  bool WGAssumeZeroGlobalOffset;
  // If set to true, the next 3 parameters define the local size to specialize
  // for.
  bool WGDynamicLocalSize;
  size_t WGLocalSizeX;
  size_t WGLocalSizeY;
  size_t WGLocalSizeZ;
  // If set to non-zero, assume each grid dimension is at most this
  // work-items wide.
  size_t WGMaxGridDimWidth;

  // Set the specialization properties.
  if (Specialize) {
    WGLocalSizeX = RunCommand->pc.local_size[0];
    WGLocalSizeY = RunCommand->pc.local_size[1];
    WGLocalSizeZ = RunCommand->pc.local_size[2];
    WGDynamicLocalSize =
        WGLocalSizeX == 0 && WGLocalSizeY == 0 && WGLocalSizeZ == 0;
    WGAssumeZeroGlobalOffset = RunCommand->pc.global_offset[0] == 0 &&
                               RunCommand->pc.global_offset[1] == 0 &&
                               RunCommand->pc.global_offset[2] == 0;
    // Compile a smallgrid version or a generic one?
    if (RunCommand->force_large_grid_wg_func ||
        pocl_cmd_max_grid_dim_width(RunCommand) >=
            Device->grid_width_specialization_limit) {
      WGMaxGridDimWidth = 0; // The generic / large / unlimited size one.
    } else {
      // Limited grid dimension width by the device specific limit.
      WGMaxGridDimWidth = Device->grid_width_specialization_limit;
    }
  } else {
    WGDynamicLocalSize = true;
    WGLocalSizeX = WGLocalSizeY = WGLocalSizeZ = 0;
    WGAssumeZeroGlobalOffset = false;
    WGMaxGridDimWidth = 0;
  }

  if (Device->device_aux_functions) {
    std::string concat;
    const char **tmp = Device->device_aux_functions;
    while (*tmp != nullptr) {
      concat.append(*tmp);
      ++tmp;
      if (*tmp)
        concat.append(";");
    }
    setModuleStringMetadata(ParallelBC, "device_aux_functions", concat.c_str());
  }

  setModuleIntMetadata(ParallelBC, "device_address_bits", Device->address_bits);
  setModuleBoolMetadata(ParallelBC, "device_arg_buffer_launcher",
                        Device->arg_buffer_launcher);
  setModuleBoolMetadata(ParallelBC, "device_grid_launcher",
                        Device->grid_launcher);
  setModuleBoolMetadata(ParallelBC, "device_is_spmd", Device->spmd);

  setModuleStringMetadata(ParallelBC, "KernelName", Kernel->name);
  setModuleIntMetadata(ParallelBC, "WGMaxGridDimWidth", WGMaxGridDimWidth);
  setModuleIntMetadata(ParallelBC, "WGLocalSizeX", WGLocalSizeX);
  setModuleIntMetadata(ParallelBC, "WGLocalSizeY", WGLocalSizeY);
  setModuleIntMetadata(ParallelBC, "WGLocalSizeZ", WGLocalSizeZ);
  setModuleBoolMetadata(ParallelBC, "WGDynamicLocalSize", WGDynamicLocalSize);
  setModuleBoolMetadata(ParallelBC, "WGAssumeZeroGlobalOffset",
                        WGAssumeZeroGlobalOffset);

  setModuleIntMetadata(ParallelBC, "device_global_as_id", Device->global_as_id);
  setModuleIntMetadata(ParallelBC, "device_local_as_id", Device->local_as_id);
  setModuleIntMetadata(ParallelBC, "device_constant_as_id",
                       Device->constant_as_id);
  setModuleIntMetadata(ParallelBC, "device_args_as_id", Device->args_as_id);
  setModuleIntMetadata(ParallelBC, "device_context_as_id",
                       Device->context_as_id);

  setModuleBoolMetadata(ParallelBC, "device_side_printf",
                        Device->device_side_printf);
  setModuleBoolMetadata(ParallelBC, "device_alloca_locals",
                        Device->device_alloca_locals);

  setModuleIntMetadata(ParallelBC, "device_max_witem_dim",
                       Device->max_work_item_dimensions);
  setModuleIntMetadata(ParallelBC, "device_max_witem_sizes_0",
                       Device->max_work_item_sizes[0]);
  setModuleIntMetadata(ParallelBC, "device_max_witem_sizes_1",
                       Device->max_work_item_sizes[1]);
  setModuleIntMetadata(ParallelBC, "device_max_witem_sizes_2",
                       Device->max_work_item_sizes[2]);

#ifdef DUMP_LLVM_PASS_TIMINGS
  llvm::TimePassesIsEnabled = true;
#endif
  POCL_MEASURE_START(llvm_workgroup_ir_func_gen);
  kernel_compiler_passes(Device).run(*ParallelBC);
  POCL_MEASURE_FINISH(llvm_workgroup_ir_func_gen);
#ifdef DUMP_LLVM_PASS_TIMINGS
  llvm::reportAndResetTimings();
#endif

  // Print loop vectorizer remarks if enabled.
  if (pocl_get_bool_option("POCL_VECTORIZER_REMARKS", 0) == 1) {
    std::cout << getDiagString(ctx);
  }

  std::string FinalizerCommand =
      pocl_get_string_option("POCL_BITCODE_FINALIZER", "");
  if (FinalizerCommand != "") {
    // Run a user-defined command on the final bitcode.
    char TempParallelBCFileName[POCL_MAX_PATHNAME_LENGTH];
    int FD = -1, Err = 0;

    Err = pocl_mk_tempname(TempParallelBCFileName, "/tmp/pocl-parallel", ".bc",
                           &FD);
    pocl_write_module((char *)ParallelBC, TempParallelBCFileName, 0);

    std::string Command = std::regex_replace(
        FinalizerCommand, std::regex(R"(%\(bc\))"), TempParallelBCFileName);
    system(Command.c_str());

    delete ParallelBC;
    ParallelBC = parseModuleIR(TempParallelBCFileName, LLVMContext);
  }

  assert(Output != NULL);
  *Output = (void *)ParallelBC;
  ++PoCLLLVMContext->number_of_IRs;
  return 0;
}

int pocl_llvm_generate_workgroup_function(unsigned DeviceI, cl_device_id Device,
                                          cl_kernel Kernel,
                                          _cl_command_node *Command,
                                          int Specialize) {
  cl_context ctx = Kernel->context;
  void *Module = NULL;

  char ParallelBCPath[POCL_MAX_PATHNAME_LENGTH];
  pocl_cache_work_group_function_path(ParallelBCPath, Kernel->program, DeviceI,
                                      Kernel, Command, Specialize);

  if (pocl_exists(ParallelBCPath))
    return CL_SUCCESS;

  char FinalBinaryPath[POCL_MAX_PATHNAME_LENGTH];
  pocl_cache_final_binary_path(FinalBinaryPath, Kernel->program, DeviceI,
                               Kernel, Command, Specialize);

  if (pocl_exists(FinalBinaryPath))
    return CL_SUCCESS;

  int Error = pocl_llvm_generate_workgroup_function_nowrite(
      DeviceI, Device, Kernel, Command, &Module, Specialize);
  if (Error)
    return Error;

  Error = pocl_cache_write_kernel_parallel_bc(Module, Kernel->program, DeviceI,
                                              Kernel, Command, Specialize);

  if (Error) {
    POCL_MSG_ERR("pocl_cache_write_kernel_parallel_bc() failed with %i\n",
                 Error);
    return Error;
  }

  pocl_destroy_llvm_module(Module, ctx);
  return Error;
}

/* Reads LLVM IR module from program->binaries[i], if prog_data->llvm_ir is
 * NULL */
int pocl_llvm_read_program_llvm_irs(cl_program program, unsigned device_i,
                                    const char *program_bc_path) {
  cl_context ctx = program->context;
  PoclLLVMContextData *llvm_ctx = (PoclLLVMContextData *)ctx->llvm_context_data;
  PoclCompilerMutexGuard lockHolder(&llvm_ctx->Lock);
  cl_device_id dev = program->devices[device_i];

  if (program->llvm_irs[device_i] != nullptr)
    return CL_SUCCESS;

  llvm::Module *M;
  if (program->binaries[device_i])
    M = parseModuleIRMem((char *)program->binaries[device_i],
                         program->binary_sizes[device_i], llvm_ctx->Context);
  else {
    // TODO
    assert(program_bc_path);
    M = parseModuleIR(program_bc_path, llvm_ctx->Context);
  }
  assert(M);
  program->llvm_irs[device_i] = M;
  if (dev->program_scope_variables_pass)
    parseModuleGVarSize(program, device_i, M);
  ++llvm_ctx->number_of_IRs;
  return CL_SUCCESS;
}

void pocl_llvm_free_llvm_irs(cl_program program, unsigned device_i) {
  cl_context ctx = program->context;
  PoclLLVMContextData *llvm_ctx = (PoclLLVMContextData *)ctx->llvm_context_data;
  PoclCompilerMutexGuard lockHolder(&llvm_ctx->Lock);

  if (program->llvm_irs[device_i]) {
    llvm::Module *mod = (llvm::Module *)program->llvm_irs[device_i];
    delete mod;
    --llvm_ctx->number_of_IRs;
    program->llvm_irs[device_i] = nullptr;
  }
}


static void initPassManagerForCodeGen(PassManager& PM, cl_device_id Device) {

  llvm::Triple Triple(Device->llvm_target_triplet);

  llvm::TargetLibraryInfoWrapperPass *TLIPass =
      new TargetLibraryInfoWrapperPass(Triple);
  PM.add(TLIPass);
}

/* Run LLVM codegen on input file (parallel-optimized).
 * modp = llvm::Module* of parallel.bc
 * Output native object file (<kernel>.so.o). */
int pocl_llvm_codegen(cl_device_id Device, cl_program program, void *Modp,
                      char **Output, uint64_t *OutputSize) {

  cl_context ctx = program->context;
  PoclLLVMContextData *llvm_ctx = (PoclLLVMContextData *)ctx->llvm_context_data;
  PoclCompilerMutexGuard lockHolder(&llvm_ctx->Lock);

  llvm::Module *Input = (llvm::Module *)Modp;
  assert(Input);
  *Output = nullptr;

  PassManager PMObj;
  initPassManagerForCodeGen(PMObj, Device);

  llvm::Triple Triple(Device->llvm_target_triplet);
  llvm::TargetMachine *Target = GetTargetMachine(Device, Triple);

  SmallVector<char, 4096> Data;
  llvm::raw_svector_ostream SOS(Data);
  bool cannotEmitFile;

  cannotEmitFile = Target->addPassesToEmitFile(PMObj, SOS, nullptr,
                                  CODEGEN_FILE_TYPE_NS::CGFT_ObjectFile);

  // First try direct object code generation from LLVM, 
  // if supported by the LLVM backend for the target.  
#ifndef CROSS_COMPILATION
  bool LLVMGeneratesObjectFiles = !cannotEmitFile;
#else
  // This optimization doesn't work when using LLVM as cross-compiler
  bool LLVMGeneratesObjectFiles = 0;  
#endif
  
  if (LLVMGeneratesObjectFiles) {
    POCL_MSG_PRINT_LLVM("Generating an object file directly.\n");
#ifdef DUMP_LLVM_PASS_TIMINGS
    llvm::TimePassesIsEnabled = true;
#endif
    PMObj.run(*Input);
#ifdef DUMP_LLVM_PASS_TIMINGS
    llvm::reportAndResetTimings();
#endif
    auto O = SOS.str(); // flush
    const char *Cstr = O.data();
    size_t S = O.size();
    *Output = (char *)malloc(S);
    *OutputSize = S;
    memcpy(*Output, Cstr, S);
    return 0;
  }

  PassManager PMAsm;
  initPassManagerForCodeGen(PMAsm, Device);

  POCL_MSG_PRINT_LLVM("Generating assembly text.\n");

  // The LLVM target does not implement support for emitting object file directly.
  // Have to emit the text first and then call the assembler from the command line
  // to produce the binary.

  if (Target->addPassesToEmitFile(PMAsm, SOS, nullptr,
                                  CODEGEN_FILE_TYPE_NS::CGFT_AssemblyFile)) {
    POCL_ABORT("The target supports neither obj nor asm emission!");
  }

#ifdef DUMP_LLVM_PASS_TIMINGS
  llvm::TimePassesIsEnabled = true;
#endif
  // This produces the assembly text:
  PMAsm.run(*Input);
#ifdef DUMP_LLVM_PASS_TIMINGS
  llvm::reportAndResetTimings();
#endif

  // Next call the target's assembler via the Toolchain API indirectly through
  // the Driver API.

  char AsmFileName[POCL_MAX_PATHNAME_LENGTH];
  char ObjFileName[POCL_MAX_PATHNAME_LENGTH];

  std::string AsmStr = SOS.str().str();
  pocl_write_tempfile(AsmFileName, "/tmp/pocl-asm", ".s", AsmStr.c_str(),
                      AsmStr.size(), nullptr);
  pocl_mk_tempname(ObjFileName, "/tmp/pocl-obj", ".o", nullptr);

  const char *Args[] = {CLANG, AsmFileName, "-c", "-o", ObjFileName, nullptr};
  int Res = pocl_invoke_clang(Device, Args);
  if (Res == 0) {
    if (pocl_read_file(ObjFileName, Output, OutputSize))
      POCL_ABORT("Could not read the object file.");
  }

  pocl_remove(AsmFileName);
  pocl_remove(ObjFileName);
  return Res;

}
/* vim: set ts=4 expandtab: */
