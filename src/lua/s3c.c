#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include "../s3c.h"  // общий API
#include "s3c.h"     // lua-обертка
#include <stdlib.h>
#include <string.h>

static struct s3c_ctx **check_ctx(lua_State *L, int idx) {
    struct s3c_ctx **ud = luaL_checkudata(L, idx, "s3c.ctx");
    luaL_argcheck(L, ud != NULL && *ud != NULL, idx, "s3c ctx expected");
    return ud;
}

static int luaT_s3c_new(lua_State *L) {
    const char *endpoint = luaL_checkstring(L, 1);
    const char *access_key = luaL_optstring(L, 2, NULL);
    const char *secret_key = luaL_optstring(L, 3, NULL);
    const char *region = luaL_optstring(L, 4, "us-east-1");
    long connect_timeout_ms = luaL_optlong(L, 5, 5000);
    long request_timeout_ms = luaL_optlong(L, 6, 0);
    int verbose = luaL_optinteger(L, 7, 0L);

    struct s3c_ctx *ctx = s3c_ctx_new(endpoint, access_key, secret_key, region,
                                    connect_timeout_ms, request_timeout_ms, verbose);
    if (!ctx) return luaL_error(L, "failed to create s3c context");

    struct s3c_ctx **lua_ctx = lua_newuserdata(L, sizeof(ctx));
    *lua_ctx = ctx;

    luaL_getmetatable(L, "s3c.ctx");
    lua_setmetatable(L, -2);
    return 1;
}

static int luaT_s3c_free(lua_State *L) {
    struct s3c_ctx **lua_ctx = check_ctx(L, 1);
    if (lua_ctx && *lua_ctx) {
        s3c_ctx_free(*lua_ctx);
        *lua_ctx = NULL;
    }
    return 0;
}

static int luaT_put(lua_State *L) {
    struct s3c_ctx **lua_ctx = check_ctx(L, 1);

    const char *bucket = luaL_checkstring(L, 2);
    const char *key = luaL_checkstring(L, 3);
    size_t len;
    const char *data = luaL_checklstring(L, 4, &len);
    const char *content_type = luaL_optstring(L, 5, "application/octet-stream");

    int res = s3c_put_object(*lua_ctx, bucket, key, data, len, content_type);
    if (res != 0) {
        lua_pushnil(L);
        lua_pushfstring(L, "put failed: %d", res);
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int luaT_get(lua_State *L) {
    struct s3c_ctx **lua_ctx = check_ctx(L, 1);

    const char *bucket = luaL_checkstring(L, 2);
    const char *key = luaL_checkstring(L, 3);

    char *data = NULL;
    size_t len = 0;

    int res = s3c_get_object(*lua_ctx, bucket, key, &data, &len);
    if (res != 0) {
        lua_pushnil(L);
        lua_pushfstring(L, "get failed: %d", res);
        return 2;
    }
    lua_pushlstring(L, data, len);
    free(data);
    return 1;
}

static const luaL_Reg methods[] = {
    {"put", luaT_put},
    {"get", luaT_get},
    {NULL, NULL}
};

static const luaL_Reg libf[] = {
    {"new", luaT_s3c_new},
    {NULL, NULL}
};

LUA_API int luaopen_s3c(struct lua_State *L) {
    /* Метатаблица для контекста */
    luaL_newmetatable(L, "s3c.ctx");

    /* __gc — деструктор */
    lua_pushstring(L, "__gc");
    lua_pushcfunction(L, luaT_s3c_free);
    lua_settable(L, -3);

    /* __index = methods */
    lua_pushstring(L, "__index");
    lua_newtable(L);
    luaL_setfuncs(L, methods, 0);
    lua_settable(L, -3);

    /* Теперь создаём таблицу модуля */
    luaL_newlib(L, libf);
    return 1;
}