#pragma once
// "Test connection" for a custom connection: asks the server for its model list with the connection's key and
// trusted certificate, using the system's curl (the updater's tool), and says in plain words what happened: the
// models it lists, an unreachable host, a certificate it does not trust, a refused key, a wrong base URL.
#include <QString>

#include "common/config/RikuganConfig.h"

namespace rk {

struct ConnectionTestResult {
    bool ok = false;
    QString message;  // user-facing
};

// Blocks up to ~15 s: call it off the UI thread.
ConnectionTestResult testConnection(const CustomEndpoint& endpoint);

// The address the test asks: the API's model list under the base URL.
QString connectionTestUrl(const CustomEndpoint& endpoint);

}  // namespace rk
