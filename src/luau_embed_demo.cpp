// Demo: the player's script-load boundary against real Luau.
// Needs Luau OSS: make LUAU_INCLUDE=/path/to/luau/VM/include luau_demo
// Links: -L/path/to/luau/build -lLuau.VM -lLuau.Compiler -lLuau.Require
#include <cstdio>
#include <cstdlib>
#include "rbx/luau_bridge.hpp"

static bool allowAll(const char*, size_t) { return true; } // stand-in auth

int main() {
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    const char* src = "local x = 40 + 2 return x";
    if (rbx::verifiedLoad(L, "demo", src, 24, allowAll) != LUA_OK) {
        std::puts(lua_tostring(L, -1));
        return 1;
    }
    if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
        std::puts(lua_tostring(L, -1));
        return 1;
    }
    std::printf("result = %d\n", (int)lua_tonumber(L, -1));
    lua_close(L);
    return 0;
}
