#pragma once
// IdaHost::runOnMainThread's execute_sync plumbing (src/ida/sdk/MainThreadRequest.cpp).
#include <memory>

namespace rk {
class MainThreadCall;
}

namespace rk::ida {

// Queues call->runOnMain() on IDA's main thread (execute_sync MFF_NOWAIT with MFF_WRITE or MFF_READ) and returns
// the request id.
int postMainThreadCall(const std::shared_ptr<MainThreadCall>& call, bool write);
// cancel_exec_request
void cancelMainThreadCall(int requestId);

}  // namespace rk::ida
