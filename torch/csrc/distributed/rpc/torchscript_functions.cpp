#include <torch/csrc/distributed/rpc/torchscript_functions.h>

#include <torch/csrc/distributed/autograd/utils.h>
#include <torch/csrc/distributed/rpc/message.h>
#include <torch/csrc/distributed/rpc/rpc_agent.h>
#include <torch/csrc/distributed/rpc/rref_proto.h>
#include <torch/csrc/distributed/rpc/script_call.h>
#include <torch/csrc/distributed/rpc/utils.h>

namespace torch {
namespace distributed {
namespace rpc {

c10::intrusive_ptr<c10::ivalue::Future> rpcTorchscript(
    const std::string& dstWorkerName,
    const c10::QualifiedName& qualifiedName,
    const c10::FunctionSchema& functionSchema,
    std::vector<c10::IValue>& stack,
    const std::shared_ptr<torch::autograd::profiler::RecordFunction>& rf) {
  auto scriptCall =
      std::make_unique<ScriptCall>(qualifiedName, std::move(stack));
  auto rpcAgentPtr = RpcAgent::getCurrentRpcAgent();
  auto futMessage = autograd::sendMessageWithAutograd(
      *rpcAgentPtr,
      rpcAgentPtr->getWorkerInfo(dstWorkerName),
      std::move(*scriptCall).toMessage(),
      true /*forceGradRecording*/,
      rf);

  // Get function return type to construct c10::ivalue::Future.
  auto returns = functionSchema.returns();
  // Script call only allows single IValue returned.
  TORCH_INTERNAL_ASSERT(
      returns.size() == 1,
      "Return value of an annotated torchScript function should be a single "
      "IValue.",
      returns.size());
  auto returnType = returns.at(0).type();

  // Create a JIT future and pass it to futMessage's callback to set state
  // of the JIT future.
  auto futPtr = c10::make_intrusive<c10::ivalue::Future>(returnType);
  futMessage->addCallback([futPtr, futMessage]() {
    if (futMessage->hasError()) {
      c10::ivalue::Future::FutureError jitFutErr(futMessage->error()->what());
      futPtr->setError(std::move(jitFutErr));
    } else {
      futPtr->markCompleted(deserializeRespToIValue(futMessage->constValue()));
    }
  });
  return futPtr;
}

c10::intrusive_ptr<RRef> remoteTorchscript(
    const std::string& dstWorkerName,
    const c10::QualifiedName& qualifiedName,
    const c10::FunctionSchema& functionSchema,
    std::vector<c10::IValue>& stack,
    const std::shared_ptr<torch::autograd::profiler::RecordFunction>& rf) {
  auto rpcAgentPtr = RpcAgent::getCurrentRpcAgent();
  auto dstWorkerInfo = rpcAgentPtr->getWorkerInfo(dstWorkerName);
  auto& ctx = RRefContext::getInstance();

  // Get function return type to construct UserRRef.
  auto returns = functionSchema.returns();
  // Script call only allows single IValue returned.
  TORCH_INTERNAL_ASSERT(
      returns.size() == 1,
      "Return value of an annotated torchScript function should be a single "
      "IValue.",
      returns.size());
  auto returnType = returns.at(0).type();

  if (ctx.getWorkerId() != dstWorkerInfo.id_) {
    auto userRRefPtr = ctx.createUserRRef(dstWorkerInfo.id_, returnType);

    auto scriptRemoteCall = std::make_unique<ScriptRemoteCall>(
        qualifiedName,
        std::move(stack),
        userRRefPtr->rrefId(),
        userRRefPtr->forkId());

    auto fm = torch::distributed::autograd::sendMessageWithAutograd(
        *rpcAgentPtr,
        dstWorkerInfo,
        std::move(*scriptRemoteCall).toMessage(),
        true /*forceGradRecording*/,
        rf);

    ctx.addPendingUser(userRRefPtr->forkId(), userRRefPtr);
    fm->addCallback([forkId{userRRefPtr->forkId()}, fm]() {
      callback::confirmPendingUser(fm, forkId);
    });

    return userRRefPtr;
  } else {
    auto ownerRRefPtr = ctx.createOwnerRRef(returnType);
    // prevent this owner RRef from being deleted due to other forks
    ctx.addSelfAsFork(ownerRRefPtr);

    auto scriptRemoteCall = std::make_unique<ScriptRemoteCall>(
        qualifiedName,
        std::move(stack),
        ownerRRefPtr->rrefId(),
        ownerRRefPtr->rrefId());

    auto fm = torch::distributed::autograd::sendMessageWithAutograd(
        *rpcAgentPtr,
        dstWorkerInfo,
        std::move(*scriptRemoteCall).toMessage(),
        true /*forceGradRecording*/,
        rf);

    fm->addCallback([fm]() { callback::finishCreatingOwnerRRef(fm); });
    return ownerRRefPtr;
  }
}

} // namespace rpc
} // namespace distributed
} // namespace torch
