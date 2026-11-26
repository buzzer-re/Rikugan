#pragma once
// Rikugan's own omc configuration (~/.rikugan/omc), prepared once per process before any omc call.
#include <QString>

namespace rk {

class AgentHome {
public:
    // Host main thread, at plugin init, before the first omc_session_new: environment, managed config.yml,
    // models.yml, the trimmed project prompt, built-in skills.
    static void prepare();
    // Rewrites omc/agent/models.yml: the service session's placeholder provider and the config's custom endpoints.
    // libomc reads the file once per process, so a change made after the first session opened takes effect when
    // the host restarts.
    static void writeModelsYaml();
    // The model the service session opens with. Settings must work before any login or key exists, and omc refuses
    // to open a session without a model; this one is never prompted and never offered as a choice.
    static QString serviceProvider();
    static QString serviceModel();
    // Re-syncs built-in skills with config.disabledSkills.
    static void syncSkills();
    // Standalone omc's agent dir as it was before prepare() redirected omc (for AuthSync).
    static QString standaloneAgentDir();
};

}  // namespace rk
