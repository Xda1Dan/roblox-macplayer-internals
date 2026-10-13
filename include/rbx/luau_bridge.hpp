// Illustrative reconstruction: how the engine hosts Luau.
// API shapes follow Luau OSS (VM/include/lua.h, Require.h); the player plugs
// a DataModel-backed require navigator and verifies bytecode at luau_load.
// Original glue code, MIT. Needs Luau OSS headers to compile.
#pragma once
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <functional>

// Provided by Luau OSS (github.com/luau-lang/luau), NOT vendored here.
#include "lua.h"
#include "luacode.h"   // luau_compile
#include "lualib.h"    // luaL_openlibs
#include "Luau/Require.h" // luarequire_Configuration

namespace rbx {

struct ScriptExecutionPolicy {
    uint32_t identityLevel = 0;
    bool restrictedContainer = false;
    bool trustedBytecode = false;
};

// Native loader stages visible in the player: policy gate, cache/compile,
// then VM load/resume. This is an illustrative interface, not engine code.
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

// One pooled VM lane (cf. VmCollectionFacet: acquire/release).
class LuaLane {
public:
    explicit LuaLane(lua_State* global) : thread_(lua_newthread(global)) {}
    lua_State* get() const { return thread_; }
private:
    lua_State* thread_; // coroutine stack sharing global_State
};

// Compile-then-load boundary with a verification hook where the player's
// "Bytecode Auth Check" lives (before luau_load ever sees the bytes).
inline int verifiedLoad(lua_State* L, const char* chunkName,
                        const char* source, size_t size,
                        bool (*authFn)(const char*, size_t)) {
    size_t bytecodeSize = 0;
    char* bytecode = luau_compile(source, size, nullptr, &bytecodeSize);
    if (!bytecode) return LUA_ERRSYNTAX;
    int ok = LUA_OK;
    if (authFn && !authFn(bytecode, bytecodeSize)) {
        ok = LUA_ERRERR; // "Bytecode Auth Check missing or wrong version"
    } else {
        ok = luau_load(L, chunkName, bytecode, bytecodeSize, 0);
    }
    ::free(bytecode);
    return ok;
}

} // namespace rbx
