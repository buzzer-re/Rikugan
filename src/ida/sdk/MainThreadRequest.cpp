// The execute_sync request behind IdaHost::runOnMainThread (an SDK subclass: built without RTTI).
#include <memory>

#include "common/host/Host.h"
#include "ida/host/IdaSdk.h"
#include "ida/sdk/MainThreadRequest.h"

namespace rk::ida {

namespace {

// Heap-allocated for MFF_NOWAIT; IDA deletes it once it ran or was cancelled.
struct MainThreadRequest : public exec_request_t {
    explicit MainThreadRequest(std::shared_ptr<MainThreadCall> c) : call(std::move(c)) {}
    ssize_t idaapi execute() override {
        // an exception must never unwind into IDA's request loop
        try {
            call->runOnMain();
        } catch (...) {
        }
        return 0;
    }
    std::shared_ptr<MainThreadCall> call;
};

}  // namespace

int postMainThreadCall(const std::shared_ptr<MainThreadCall>& call, bool write) {
    auto* req = new MainThreadRequest(call);
    return int(execute_sync(*req, MFF_NOWAIT | (write ? MFF_WRITE : MFF_READ)));
}

void cancelMainThreadCall(int requestId) { cancel_exec_request(requestId); }

}  // namespace rk::ida
