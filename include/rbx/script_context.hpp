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

struct ScriptOptions {
    ScriptKind kind = ScriptKind::Script;
    std::string runContext; // "Server" | "Client" | "Plugin" | "Legacy"
    bool canRunClient = false; // Run{Client,Server}Script capabilities
    bool canRunServer = false;
};

} // namespace rbx
