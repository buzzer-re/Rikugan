#pragma once
// A tool-less, in-memory omc session for account and model calls (get_available_models, login, logout,
// get_logout_accounts ...). Opened on first use; one per process.
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "common/omc/OmcSession.h"

namespace rk {

class ServiceSession {
public:
    using FrameHandler = std::function<void(std::string_view type, const std::string& json)>;

    // The session (opened on first call). Jobs posted to it run on its worker.
    static std::shared_ptr<OmcSession> get();
    // Where dialogs of a login (extension_ui_request) and notices go; empty to drop them. Any thread.
    static void setFrameHandler(FrameHandler h);
    // Makes omc re-read auth.json (get_logout_accounts reloads the credential store). Non-blocking.
    static void reloadCredentials();
    // Process shutdown.
    static void shutdown(int waitMs);
};

}  // namespace rk
