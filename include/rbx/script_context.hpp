// Illustrative reconstruction: ScriptContext service + facets.
// Names mirror observed facet/method shapes (ProtectedString source,
// Security::Identity, per-VM module state). Original code, MIT.
#pragma once
#include <memory>
#include <string>
#include "instance.hpp"
#include "reflection.hpp"

struct lua_State; // Luau; see luau_bridge.hpp

namespace rbx {

struct SecurityIdentity { uint32_t level = 0; };
struct ProtectedString { std::string bytes; }; // source is never raw char*

enum class RunContext { Legacy, Server, Client, Plugin };

enum class ScriptCapability : uint64_t {
    RunClientScript = 1ull << 8,
    RunServerScript = 1ull << 9,
};

struct ExecutionContext {
    RunContext runContext = RunContext::Legacy;
    SecurityIdentity identity;
    uint64_t capabilities = 0;

    bool has(ScriptCapability capability) const {
        return (capabilities & static_cast<uint64_t>(capability)) != 0;
    }

    bool admitsScript() const {
        switch (runContext) {
        case RunContext::Client: return has(ScriptCapability::RunClientScript);
        case RunContext::Server: return has(ScriptCapability::RunServerScript);
        case RunContext::Plugin:
        case RunContext::Legacy: return true; // additional native gates apply
        }
        return false;
    }
};

class ScriptContext : public Instance {
public:
    static const ClassDescriptor kDescriptor;
    const ClassDescriptor* classDescriptor() const override { return &kDescriptor; }

    struct LoaderFacet {
        virtual ~LoaderFacet() = default;
        virtual void addScript(std::shared_ptr<Instance> script) = 0;
        virtual void removeScript(std::shared_ptr<Instance> script) = 0;
        // Module loading from instance / asset id / library path.
        virtual Tuple requireModule(lua_State* L, std::shared_ptr<Instance> module) = 0;
        virtual void handleModuleResult(lua_State* L, Tuple result) = 0;
        virtual void callAsyncCallback(SecurityIdentity id, ProtectedString source,
                                      const char* chunkName, Tuple args) = 0;
    };
    struct SchedulerFacet {
        virtual ~SchedulerFacet() = default;
        virtual void resumeWaitingScripts() = 0; // WaitingHybridScriptsJob path
    };
    struct ExecutorFacet {
        virtual ~ExecutorFacet() = default;
        virtual void callInNewThread(std::weak_ptr<Instance> fn, Tuple args) = 0;
        virtual void callAsyncCallback(std::weak_ptr<Instance> implementingScript,
                                       ProtectedString source,
                                       const char* chunkName,
                                       Tuple args) = 0;
    };
    struct VmCollectionFacet {
        virtual ~VmCollectionFacet() = default;
        virtual lua_State* acquireVm() = 0; // lane from the pool
        virtual void releaseVm(lua_State* L) = 0;
        virtual size_t stateThreadCount() const = 0;
    };

    LoaderFacet& loader();
    SchedulerFacet& scheduler();
    ExecutorFacet& executor();
    VmCollectionFacet& vms();

    // Observed invariant: startScript rejects nesting beyond three levels.
    unsigned startDepth() const { return startDepth_; }
    bool tryEnterScript() {
        if (startDepth_ >= 3) return false;
        ++startDepth_;
        return true;
    }
    void leaveScript() { if (startDepth_ != 0) --startDepth_; }

private:
    unsigned startDepth_ = 0;
};

// Script instance kinds observed in the binary.
enum class ScriptKind { Script, LocalScript, ModuleScript, CoreScript };

enum class ModuleState {
    Unloaded,
    Loading,
    Loaded,
    Failed,
    Destroyed,
};

struct ModuleResult {
    ModuleState state = ModuleState::Unloaded;
    std::shared_ptr<const Tuple> value; // successful require returns one value
    std::string error;
};

enum class CoroutineState { Running, Suspended, Awaiting, Completed, Failed };

struct AsyncCallbackResult {
    bool accepted = false;
    CoroutineState state = CoroutineState::Failed;
    Tuple values;
    std::string error;
};

// The native callback path first resolves the weak script reference and then
// admits only a resumable coroutine. The VM-specific resume is intentionally
// left behind this interface.
class AsyncCallbackGate {
public:
    static AsyncCallbackResult admit(bool scriptStillAlive,
                                     CoroutineState state) {
        if (!scriptStillAlive)
            return {false, CoroutineState::Failed, {},
                    "script implementing callback was destroyed"};
        if (state != CoroutineState::Suspended &&
            state != CoroutineState::Awaiting)
            return {false, state, {},
                    "coroutine is not suspended/awaiting"};
        return {true, state, {}, {}};
    }
};

// Per-VM state is intentionally separate from the ModuleScript instance.
// The native path caches results, detects cycles, and invalidates destruction.
class ModulePerVmState {
public:
    ModuleResult result;
    bool loadingOnCurrentStack = false;

    bool beginLoad() {
        if (result.state == ModuleState::Destroyed ||
            result.state == ModuleState::Loaded)
            return false;
        if (result.state == ModuleState::Loading) {
            loadingOnCurrentStack = true;
            return false;
        }
        result.state = ModuleState::Loading;
        return true;
    }

    void complete(Tuple value) {
        result.value = std::make_shared<const Tuple>(std::move(value));
        result.state = ModuleState::Loaded;
        loadingOnCurrentStack = false;
    }

    void fail(std::string message) {
        result.error = std::move(message);
        result.state = ModuleState::Failed;
        loadingOnCurrentStack = false;
    }

    void destroy() {
        result.value.reset();
        result.state = ModuleState::Destroyed;
    }
};

// Destruction order follows the native lifetime boundary: stop admission,
// drain waiting work, release VM lanes, then invalidate the context.
class ScriptContextLifetime {
public:
    bool closing() const { return closing_; }
    void beginClose() { closing_ = true; }

    void noteWaitingThreads(size_t count) { waitingThreads_ = count; }
    void noteYieldWaiters(size_t count) { yieldWaiters_ = count; }
    size_t waitingThreads() const { return waitingThreads_; }
    size_t yieldWaiters() const { return yieldWaiters_; }

    bool canSchedule() const { return !closing_; }
    void drainComplete() {
        waitingThreads_ = 0;
        yieldWaiters_ = 0;
        vmsReleased_ = true;
    }
    bool vmsReleased() const { return vmsReleased_; }

private:
    bool closing_ = false;
    bool vmsReleased_ = false;
    size_t waitingThreads_ = 0;
    size_t yieldWaiters_ = 0;
};

struct ScriptOptions {
    ScriptKind kind = ScriptKind::Script;
    std::string runContext; // "Server" | "Client" | "Plugin" | "Legacy"
    bool canRunClient = false; // Run{Client,Server}Script capabilities
    bool canRunServer = false;
};

} // namespace rbx
