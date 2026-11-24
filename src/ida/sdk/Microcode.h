#pragma once
// The microcode optimizers of one IdaHost, in
// installation order. NOP rules are native optinsn_t handlers; install_microcode_optimizer's run in IDAPython.
#include <memory>
#include <vector>

namespace rk::ida {

class IdaHost;

class MicrocodeOptimizers {
public:
    struct Entry;  // src/ida/sdk/MicrocodeTools.cpp

    explicit MicrocodeOptimizers(IdaHost& host);
    // Removes every optimizer still installed (main thread).
    ~MicrocodeOptimizers();

    IdaHost& host;
    std::vector<std::unique_ptr<Entry>> entries;
    bool pythonHelpersReady = false;
};

}  // namespace rk::ida
