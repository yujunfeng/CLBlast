
// =================================================================================================
// This file is part of the CLBlast project. The project is licensed under Apache Version 2.0. This
// project loosely follows the Google C++ styleguide and uses a tab-size of two spaces and a max-
// width of 100 characters per line.
//
// Author(s):
//   Cedric Nugteren <www.cedricnugteren.nl>
//
// This file uses the auto-tuner to tune the direct xgemm kernels. There are two variations:
// - V==1: This tests some limited set of tuning parameters exhaustively.
// - V==2: This tests a much larger set of tuning parameters by randomly sampling a subset.
//
// =================================================================================================

#include <string>
#include <vector>

#include "utilities/utilities.hpp"
#include "tuning/tuning.hpp"

namespace clblast {
// =================================================================================================

// Settings for this kernel (default command-line arguments)
TunerDefaults GetTunerDefaults(const int V) {
  auto settings = TunerDefaults();
  settings.options = {kArgM, kArgN, kArgK, kArgAlpha, kArgBeta, kArgFraction,
                      kArgHeuristicSelection, kArgPsoSwarmSize,
                      kArgPsoInfGlobal, kArgPsoInfLocal, kArgPsoInfRandom};
  settings.default_m = 256;
  settings.default_n = 256;
  settings.default_k = 256;
  settings.default_fraction = (V==1) ? 1.0 : 64.0; // test all or sample randomly
  settings.default_num_runs = 4;
  return settings;
}

// Settings for this kernel (general)
template <typename T>
TunerSettings GetTunerSettings(const int V, const Arguments<T> &args) {
  auto settings = TunerSettings();

  // Identification of the kernel
  settings.kernel_family = (V==1) ? "xgemm_direct_1" : "xgemm_direct_2";
  settings.kernel_name = "XgemmDirectTN";
  settings.sources =
#include "../src/kernels/level3/xgemm_direct_part1.opencl"
#include "../src/kernels/level3/xgemm_direct_part2.opencl"
#include "../src/kernels/level3/xgemm_direct_part3.opencl"
  ;

  // Buffer sizes
  settings.size_a = args.m * args.k;
  settings.size_b = args.n * args.k;
  settings.size_c = args.m * args.n;

  // Inputs and outputs IDs (X:0, Y:1, A:2, B:3, C:4, temp:5)
  settings.inputs = {2, 3, 4};
  settings.outputs = {4};

  // Sets the base thread configuration
  settings.global_size = {args.m, args.n};
  settings.global_size_ref = settings.global_size;
  settings.local_size = {1, 1};
  settings.local_size_ref = {8, 8};

  // Transforms the thread configuration based on the parameters
  settings.mul_local = {{"MDIMCD", "NDIMCD"}};
  settings.mul_global = {{"MDIMCD", "NDIMCD"}};
  settings.div_global = {{"WGD", "WGD"}};

  // Sets the tuning parameters and their possible values
  if (V==1) { // limited subset of tuning parameters - but explorable exhaustively
    settings.parameters = {
      {"WGD", {8, 16, 32}},
      {"MDIMCD", {8, 16, 32}},
      {"NDIMCD", {8, 16, 32}},
      {"MDIMAD", {8, 16, 32}},
      {"NDIMBD", {8, 16, 32}},
      {"KWID", {2}},
      {"VWMD", {1, 2, 4, 8}},
      {"VWND", {1, 2, 4, 8}},
      {"PADA", {1}},
      {"PADB", {1}},
    };
  }
  else { // a lot more tuning parameters - has to be sampled randomly, too much to test all
    settings.parameters = {
      {"WGD", {8, 16, 32, 64}},
      {"MDIMCD", {8, 16, 32}},
      {"NDIMCD", {8, 16, 32}},
      {"MDIMAD", {8, 16, 32}},
      {"NDIMBD", {8, 16, 32}},
      {"KWID", {2, 8, 16}},
      {"VWMD", {1, 2, 4, 8}},
      {"VWND", {1, 2, 4, 8}},
      {"PADA", {0, 1}},
      {"PADB", {0, 1}},
    };
  }

  // Describes how to compute the performance metrics
  settings.metric_amount = 2 * args.m * args.n * args.k;
  settings.performance_unit = "GFLOPS";

  return settings;
}

// Tests for valid arguments
template <typename T>
void TestValidArguments(const int, const Arguments<T> &) { }
std::vector<Constraint> SetConstraints(const int V) {
  auto constraints = std::vector<Constraint>();
  auto MultipleOfX = [] (std::vector<size_t> v) { return IsMultiple(v[0], v[1]); };
  auto MultipleOfXMulY = [] (std::vector<size_t> v) { return IsMultiple(v[0], v[1]*v[2]); };
  auto MultipleOfXMulYDivZ = [] (std::vector<size_t> v) { return IsMultiple(v[0], (v[1]*v[2])/v[3]); };
  // Requirement for unrolling the WGD loop
  constraints.push_back({MultipleOfX, {"WGD", "KWID"}});
  // Required for integer MWID and NWID
  constraints.push_back({MultipleOfXMulY, {"WGD", "MDIMCD", "VWMD"}});
  constraints.push_back({MultipleOfXMulY, {"WGD", "NDIMCD", "VWND"}});
  // Required for integer MWIAD and NWIBD
  constraints.push_back({MultipleOfXMulY, {"WGD", "MDIMAD", "VWMD"}});
  constraints.push_back({MultipleOfXMulY, {"WGD", "NDIMBD", "VWND"}});
  // WGD has to be a multiple of KDIMAD = ((MDIMCD*NDIMCD)/(MDIMAD)) and KDIMBD = (...)
  constraints.push_back({MultipleOfXMulYDivZ, {"WGD", "MDIMCD", "NDIMCD", "MDIMAD"}});
  constraints.push_back({MultipleOfXMulYDivZ, {"WGD", "MDIMCD", "NDIMCD", "NDIMBD"}});

  // Extra constraints for variation 1 to limit the set of options significantly
  if (V==1) {
    auto IsEqual = [] (std::vector<size_t> v) { return v[0] == v[1]; };
    constraints.push_back({IsEqual, {"MDIMCD", "MDIMAD"}});
    constraints.push_back({IsEqual, {"NDIMCD", "NDIMBD"}});
  }
  return constraints;
}

// Sets the kernel's arguments
template <typename T>
void SetArguments(const int, Kernel &kernel, const Arguments<T> &args, std::vector<Buffer<T>>& buffers) {
  kernel.SetArgument(0, static_cast<int>(args.m));
  kernel.SetArgument(1, static_cast<int>(args.n));
  kernel.SetArgument(2, static_cast<int>(args.k));
  kernel.SetArgument(3, GetRealArg(args.alpha));
  kernel.SetArgument(4, GetRealArg(args.beta));
  kernel.SetArgument(5, buffers[2]()); // 2 == A matrix
  kernel.SetArgument(6, 0); // a_offset
  kernel.SetArgument(7, static_cast<int>(args.k)); // a_ld
  kernel.SetArgument(8, buffers[3]()); // 3 == B matrix
  kernel.SetArgument(9, 0); // b_offset
  kernel.SetArgument(10, static_cast<int>(args.n)); // b_ld
  kernel.SetArgument(11, buffers[4]()); // 4 == C matrix
  kernel.SetArgument(12, 0); // c_offset
  kernel.SetArgument(13, static_cast<int>(args.n)); // c_ld
  kernel.SetArgument(14, 1); // c_do_transpose
  kernel.SetArgument(15, 0); // a_conjugate
  kernel.SetArgument(16, 0); // b_conjugate
}

// =================================================================================================
} // namespace clblast

// Shortcuts to the clblast namespace
using half = clblast::half;
using float2 = clblast::float2;
using double2 = clblast::double2;

// Function to tune a specific variation V (not within the clblast namespace)
template <int V>
void StartVariation(int argc, char *argv[]) {
  const auto command_line_args = clblast::RetrieveCommandLineArguments(argc, argv);
  switch(clblast::GetPrecision(command_line_args)) {
    case clblast::Precision::kHalf: clblast::Tuner<half>(argc, argv, V, clblast::GetTunerDefaults, clblast::GetTunerSettings<half>, clblast::TestValidArguments<half>, clblast::SetConstraints, clblast::SetArguments<half>); break;
    case clblast::Precision::kSingle: clblast::Tuner<float>(argc, argv, V, clblast::GetTunerDefaults, clblast::GetTunerSettings<float>, clblast::TestValidArguments<float>, clblast::SetConstraints, clblast::SetArguments<float>); break;
    case clblast::Precision::kDouble: clblast::Tuner<double>(argc, argv, V, clblast::GetTunerDefaults, clblast::GetTunerSettings<double>, clblast::TestValidArguments<double>, clblast::SetConstraints, clblast::SetArguments<double>); break;
    case clblast::Precision::kComplexSingle: clblast::Tuner<float2>(argc, argv, V, clblast::GetTunerDefaults, clblast::GetTunerSettings<float2>, clblast::TestValidArguments<float2>, clblast::SetConstraints, clblast::SetArguments<float2>); break;
    case clblast::Precision::kComplexDouble: clblast::Tuner<double2>(argc, argv, V, clblast::GetTunerDefaults, clblast::GetTunerSettings<double2>, clblast::TestValidArguments<double2>, clblast::SetConstraints, clblast::SetArguments<double2>); break;
  }
}

// Main function (not within the clblast namespace)
int main(int argc, char *argv[]) {
  StartVariation<1>(argc, argv);
  StartVariation<2>(argc, argv);
  return 0;
}

// =================================================================================================
