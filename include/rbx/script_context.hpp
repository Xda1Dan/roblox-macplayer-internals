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
};

// Script instance kinds observed in the binary.
enum class ScriptKind { Script, LocalScript, ModuleScript, CoreScript };

struct ScriptOptions {
    ScriptKind kind = ScriptKind::Script;
    std::string runContext; // "Server" | "Client" | "Plugin" | "Legacy"
    bool canRunClient = false; // Run{Client,Server}Script capabilities
    bool canRunServer = false;
};

} // namespace rbx
