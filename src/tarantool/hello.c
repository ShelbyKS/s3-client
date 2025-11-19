#include <lua.h>
#include <lauxlib.h>

static int
l_hello(lua_State *L)
{
    lua_pushstring(L, "Hello from C module!");
    return 1;
}

LUA_API int
luaopen_hello(lua_State *L)
{
    lua_newtable(L);

    lua_pushcfunction(L, l_hello);
    lua_setfield(L, -2, "hello");

    return 1;
}
