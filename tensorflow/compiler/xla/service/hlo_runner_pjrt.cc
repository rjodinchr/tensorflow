/* Copyright 2022 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/compiler/xla/service/hlo_runner_pjrt.h"

#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "tensorflow/compiler/xla/client/xla_computation.h"
#include "tensorflow/compiler/xla/pjrt/pjrt_client.h"
#include "tensorflow/compiler/xla/pjrt/pjrt_executable.h"
#include "tensorflow/compiler/xla/pjrt/pjrt_future.h"
#include "tensorflow/compiler/xla/service/executable.h"
#include "tensorflow/compiler/xla/service/hlo_module.h"
#include "tensorflow/compiler/xla/statusor.h"
#include "tensorflow/tsl/platform/errors.h"
#include "tensorflow/tsl/platform/statusor.h"

namespace xla {

// TODO(b/245550554): Remove the use of PjRtWrappedExecutable.
class PjRtWrappedExecutable : public Executable {
 public:
  explicit PjRtWrappedExecutable(std::shared_ptr<HloModule> hlo_module,
                                 PjRtLoadedExecutable* pjrt_loaded_executable)
      : Executable(hlo_module),
        pjrt_loaded_executable_(pjrt_loaded_executable) {}

  StatusOr<ExecutionOutput> ExecuteAsyncOnStream(
      const ServiceExecutableRunOptions* run_options,
      std::vector<ExecutionInput> arguments,
      HloExecutionProfile* hlo_execution_profile) override;

  PjRtLoadedExecutable* GetPjRtLoadedExecutable() const {
    return pjrt_loaded_executable_;
  }

 private:
  PjRtLoadedExecutable* pjrt_loaded_executable_;
};

StatusOr<ExecutionOutput> PjRtWrappedExecutable::ExecuteAsyncOnStream(
    const ServiceExecutableRunOptions* run_options,
    std::vector<ExecutionInput> arguments,
    HloExecutionProfile* hlo_execution_profile) {
  return Unimplemented(
      "PjRtWrappedExecutable: Unimplemented ExecuteAsyncOnStream");
}

static const int kDeviceIdx = 0;

HloRunnerPjRt::HloRunnerPjRt(std::unique_ptr<PjRtClient> pjrt_client)
    : pjrt_client_(std::move(pjrt_client)) {}

HloRunnerPjRt::~HloRunnerPjRt() = default;

StatusOr<CompileOptions> HloRunnerPjRt::GenerateDefaultCompileOptions(
    HloModule* module, bool run_hlo_passes) {
  TF_ASSIGN_OR_RETURN(
      auto device_assignment,
      pjrt_client_->GetDefaultDeviceAssignment(
          module->config().replica_count(), module->config().num_partitions()));

  CompileOptions compile_options;

  compile_options.executable_build_options.set_device_assignment(
      device_assignment);
  compile_options.executable_build_options.set_num_partitions(
      module->config().num_partitions());
  compile_options.executable_build_options.set_num_replicas(
      module->config().replica_count());
  compile_options.executable_build_options.set_run_backend_only(
      !run_hlo_passes);

  return compile_options;
}

StatusOr<Literal> HloRunnerPjRt::TransferLiteralFromDevice(PjRtBuffer& buffer) {
  TF_RETURN_IF_ERROR(buffer.GetReadyFuture().Await());

  TF_ASSIGN_OR_RETURN(auto literal, buffer.ToLiteralSync());
  return std::move(*literal);
}

StatusOr<std::unique_ptr<PjRtBuffer>> HloRunnerPjRt::TransferLiteralToDevice(
    const Literal& literal) {
  auto devices = pjrt_client_->addressable_devices();

  TF_ASSIGN_OR_RETURN(auto assignment, pjrt_client_->BufferFromHostLiteral(
                                           literal, devices[kDeviceIdx]));

  return std::move(assignment);
}

StatusOr<std::vector<std::unique_ptr<PjRtBuffer>>>
HloRunnerPjRt::TransferLiteralsToDevice(
    absl::Span<const Literal* const> literals) {
  std::vector<std::unique_ptr<PjRtBuffer>> buffers;
  buffers.reserve(literals.size());
  for (const Literal* literal : literals) {
    TF_RET_CHECK(literal != nullptr);
    TF_ASSIGN_OR_RETURN(std::unique_ptr<PjRtBuffer> buffer,
                        TransferLiteralToDevice(*literal));
    TF_RETURN_IF_ERROR(buffer->GetReadyFuture().Await());
    buffers.push_back(std::move(buffer));
  }
  return std::move(buffers);
}

StatusOr<Literal> HloRunnerPjRt::Execute(
    std::unique_ptr<HloModule> module,
    absl::Span<const Literal* const> arguments, bool run_hlo_passes,
    ExecutionProfile* profile) {
  TF_ASSIGN_OR_RETURN(auto compile_options, GenerateDefaultCompileOptions(
                                                module.get(), run_hlo_passes));

  TF_ASSIGN_OR_RETURN(auto executable,
                      CreateExecutable(std::move(module), run_hlo_passes));

  return ExecuteWithExecutable(executable.get(), arguments, {});
}

std::vector<PjRtBuffer*> HloRunnerPjRt::BufferVecToPointerVec(
    const std::vector<std::unique_ptr<PjRtBuffer>>& buffer) {
  std::vector<PjRtBuffer*> argument_ptrs;
  argument_ptrs.resize(buffer.size());
  for (int i = 0; i < buffer.size(); ++i) {
    argument_ptrs[i] = buffer[i].get();
  }

  return argument_ptrs;
}

StatusOr<std::unique_ptr<PjRtLoadedExecutable>> HloRunnerPjRt::CreateExecutable(
    HloModule* module, CompileOptions compile_options) {
  XlaComputation computation(module->ToProto());

  return pjrt_client_->Compile(computation, compile_options);
}

StatusOr<std::vector<std::unique_ptr<PjRtBuffer>>>
HloRunnerPjRt::ExecuteWithDeviceBuffers(
    PjRtLoadedExecutable* executable,
    const std::vector<std::unique_ptr<PjRtBuffer>>& arguments) {
  ExecuteOptions execute_options;

  std::vector<PjRtBuffer*> argument_ptrs = BufferVecToPointerVec(arguments);

  auto devices = pjrt_client_->addressable_devices();

  std::optional<PjRtFuture<Status>> returned_future = {};

  VLOG(1) << "HloRunnerPjRt::ExecuteWithDeviceBuffers"
          << executable->device_assignment().ToString();

  TF_ASSIGN_OR_RETURN(
      auto output_buffers,
      executable->ExecuteSharded(argument_ptrs, devices[kDeviceIdx],
                                 execute_options, returned_future, false));

  return output_buffers;
}

StatusOr<Literal> HloRunnerPjRt::ExecuteWithExecutable(
    Executable* executable, absl::Span<const Literal* const> arguments,
    ExecutionProfile* profile) {
  PjRtWrappedExecutable* wrapped_executable =
      static_cast<PjRtWrappedExecutable*>(executable);

  TF_ASSIGN_OR_RETURN(auto argument_handles,
                      TransferLiteralsToDevice(arguments));

  TF_ASSIGN_OR_RETURN(
      auto output_buffer,
      ExecuteWithDeviceBuffers(wrapped_executable->GetPjRtLoadedExecutable(),
                               std::move(argument_handles)));
  // TODO (b/245550554): Support more than 1 output.
  CHECK_EQ(output_buffer.size(), 1);

  return TransferLiteralFromDevice(*output_buffer[0]);
}

StatusOr<std::unique_ptr<Executable>> HloRunnerPjRt::CreateExecutable(
    std::unique_ptr<HloModule> module, bool run_hlo_passes) {
  TF_ASSIGN_OR_RETURN(auto compile_options, GenerateDefaultCompileOptions(
                                                module.get(), run_hlo_passes));
  TF_ASSIGN_OR_RETURN(auto pjrt_executable,
                      CreateExecutable(module.get(), compile_options));

  auto executable = std::make_unique<PjRtWrappedExecutable>(
      std::shared_ptr<HloModule>(std::move(module)), pjrt_executable.release());

  std::unique_ptr<Executable> exec =
      static_cast<std::unique_ptr<Executable>>(executable.release());
  return exec;
}

StatusOr<std::vector<Literal>> HloRunnerPjRt::ExecuteReplicated(
    std::unique_ptr<HloModule> module,
    const HloRunnerInterface::ReplicatedExecuteOptions& options) {
  return Unimplemented("Unimplemented ExecuteReplicated");
}

StatusOr<std::vector<Literal>> HloRunnerPjRt::ExecuteReplicated(
    std::unique_ptr<HloModule> module,
    const HloRunnerInterface::ReplicatedExecuteOptions& options,
    DeviceAssignment* device_assignment) {
  return Unimplemented("Unimplemented ExecuteReplicated");
}

StatusOr<std::vector<Literal>> HloRunnerPjRt::ExecuteReplicated(
    std::function<Executable*(int64_t)> executable_provider,
    std::function<int64_t(int64_t)> argument_count_provider,
    std::function<const Literal*(int64_t, int64_t)> argument_provider,
    const HloRunnerInterface::ReplicatedExecuteOptions& options,
    DeviceAssignment* device_assignment) {
  return Unimplemented("Unimplemented ExecuteReplicated");
}

absl::string_view HloRunnerPjRt::Name() const { return "HloRunnerPjRt"; }

}  // namespace xla
