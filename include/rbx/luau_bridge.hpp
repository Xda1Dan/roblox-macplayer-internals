// Luau hosting: how the player owns a lua_State and runs code on it.
//
// What this shows:
// - one VM collection owns states; scripts borrow threads;
// - creation: allocator + context, state init, hooks, libraries, _G,
//   sandbox (see 0x10299fd5b, 0x1015b92cd);
// - loading: policy check, cache or compile, bytecode check, load;
// - running: resume with deadline and stack guards; task.wait yields
//   instead of sleeping (see 0x1015566d6, 0x14cf74a).
//
// Needs public Luau headers to compile the optional demo.
// Original illustration, MIT.
#pragma once
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Provided by public Luau, NOT vendored here.
#include "lua.h"
#include "luacode.h"   // luau_compile
#include "lualib.h"    // luaL_openlibs
#include "Luau/Require.h"

namespace rbx {

struct ScriptExecutionPolicy {
    uint32_t identityLevel = 0;
    bool restrictedContainer = false;
    bool trustedBytecode = false;
};

// Policy check, then cache or compile. Mirrors the native order:
// gate first, bytes second, VM third.
class ScriptExecutionPipeline {
public:
    using PolicyCheck = std::function<bool(const ScriptExecutionPolicy&)>;
    using CacheLookup = std::function<std::vector<char>(const char*, size_t)>;

    ScriptExecutionPipeline(PolicyCheck policy, CacheLookup cache)
        : policy_(std::move(policy)), cache_(std::move(cache)) {}

    bool allowed(const ScriptExecutionPolicy& policy) const {
        return !policy.restrictedContainer && policy_(policy);
    }
    std::vector<char> cachedOrCompiled(const char* source, size_t size) const {
        return cache_(source, size);
    }

private:
    PolicyCheck policy_;
    CacheLookup cache_;
};

// One pooled lane: a coroutine sharing its owner's global state.
class LuaLane {
public:
    explicit LuaLane(lua_State* global) : thread_(lua_newthread(global)) {}
    lua_State* get() const { return thread_; }
private:
    lua_State* thread_;
};

struct LuaStateConfig {
    void* allocatorContext = nullptr;
    bool sandboxed = true;
    bool openBase = true;
    bool openMath = true;
    bool openOs = false;
    bool robloxBindingsInstalled = false;
};

// The collection owns states. Callers supply the allocator, hooks,
// libraries, globals, and bindings.
class VmCollection {
public:
    using Allocator = void* (*)(void*, void*, size_t, size_t);
    using Hook = void (*)(lua_State*);

    explicit VmCollection(LuaStateConfig config) : config_(config) {}

    bool create(Allocator allocator, void* context, Hook panicHook,
                Hook interruptHook) {
        if (!allocator) return false;
        allocator_ = allocator;
        config_.allocatorContext = context;
        panicHook_ = panicHook;
        interruptHook_ = interruptHook;
        created_ = true;
        return true;
    }

    bool created() const { return created_; }
    const LuaStateConfig& config() const { return config_; }

private:
    LuaStateConfig config_;
    Allocator allocator_ = nullptr;
    Hook panicHook_ = nullptr;
    Hook interruptHook_ = nullptr;
    bool created_ = false;
};

enum class CoroutineStatus { Running, Suspended, Waiting, Completed, Failed };

// task.wait: record a wake deadline and yield. Never an OS sleep, and
// only valid on a yieldable thread.
struct WaitRequest {
    double seconds = 0.0;
    double wakeAt = 0.0;
    CoroutineStatus status = CoroutineStatus::Running;
};

inline bool scheduleTaskWait(WaitRequest& request, double now) {
    if (request.status != CoroutineStatus::Running)
        return false;
    request.wakeAt = now + request.seconds;
    request.status = CoroutineStatus::Waiting;
    return true;
}

// Compile, check, then load. The check runs before luau_load ever sees
// the bytes ("Bytecode Auth Check missing or wrong version").
inline int verifiedLoad(lua_State* L, const char* chunkName,
                        const char* source, size_t size,
                        bool (*authFn)(const char*, size_t)) {
    size_t bytecodeSize = 0;
    char* bytecode = luau_compile(source, size, nullptr, &bytecodeSize);
    if (!bytecode) return LUA_ERRSYNTAX;
    int ok = LUA_OK;
    if (authFn && !authFn(bytecode, bytecodeSize)) {
        ok = LUA_ERRERR;
    } else {
        ok = luau_load(L, chunkName, bytecode, bytecodeSize, 0);
    }
    ::free(bytecode);
    return ok;
}

} // namespace rbx
