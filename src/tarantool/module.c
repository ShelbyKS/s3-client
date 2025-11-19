/* src/tarantool/module.c
 *
 * Lua-модуль Tarantool: require('s3')
 *
 * Экспортирует:
 *
 *   local s3 = require('s3')
 *   local client = s3.new_client{
 *       endpoint   = 'http://127.0.0.1:9000',
 *       region     = 'us-east-1',
 *       access_key = 'user',
 *       secret_key = '12345678',
 *       use_tls    = false,     -- опционально
 *       use_sigv4  = true       -- опционально, по умолчанию true
 *   }
 *
 *   -- sync PUT из fd:
 *   local ok, err = client:put_from_fd('bucket', 'key', fd, offset, limit)
 *
 *   -- sync GET в fd:
 *   local ok, err = client:get_to_fd('bucket', 'key', fd, offset, limit)
 *
 * Здесь:
 *   fd     — обычный файловый дескриптор (например fio.open(...):fileno());
 *   offset — смещение в файле (целое); nil или отсутствует => 0;
 *   limit  — сколько байт читать/писать; nil или отрицательное => до EOF.
 *
 * Важно:
 *   - event loop крутит сам Tarantool;
 *   - s3_sync_* внутри блокируют только текущий fiber, используя fiber_cond.
 */

#include <stdlib.h>
#include <string.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <ev.h>
#include <tarantool/module.h>

#include <s3/client.h>
#include <s3/config.h>
#include <s3-adapters/reactor_tarantool.h>
#include <s3-tarantool.h>

#define S3_LUA_CLIENT_MT "s3.client"

/* Обёртка userdata вокруг s3_client_t */
typedef struct lua_s3_client {
    s3_client_t *client;
} lua_s3_client_t;

/* ---------- Вспомогательные функции ---------- */

static lua_s3_client_t *
lua_s3_check_client(lua_State *L, int idx)
{
    void *ud = luaL_checkudata(L, idx, S3_LUA_CLIENT_MT);
    luaL_argcheck(L, ud != NULL, idx, "s3.client expected");
    return (lua_s3_client_t *)ud;
}

/* Прочитать строку из таблицы (обязательно) */
static const char *
lua_s3_get_required_string(lua_State *L, int idx, const char *field)
{
    const char *res;

    lua_getfield(L, idx, field);
    res = lua_tostring(L, -1);
    lua_pop(L, 1);

    if (res == NULL) {
        luaL_error(L, "field '%s' must be a string", field);
        /* не вернёмся, но для компилятора: */
        return NULL;
    }
    return res;
}

/* Прочитать строку из таблицы (опционально, либо из env) */
static const char *
lua_s3_get_opt_string_with_env(lua_State *L, int idx,
                               const char *field,
                               const char *env_name,
                               const char *default_val)
{
    const char *res = NULL;

    /* сначала env, если есть */
    if (env_name != NULL)
        res = getenv(env_name);

    /* затем Lua-таблица перекрывает env */
    lua_getfield(L, idx, field);
    if (!lua_isnil(L, -1))
        res = lua_tostring(L, -1);
    lua_pop(L, 1);

    if (res == NULL)
        res = default_val;

    return res;
}

/* ---------- Методы клиента ---------- */

/*
 * client:put_from_fd(bucket, key, fd [, offset [, limit]])
 */
static int
lua_s3_client_put_from_fd(lua_State *L)
{
    lua_s3_client_t *ud = lua_s3_check_client(L, 1);
    const char *bucket  = luaL_checkstring(L, 2);
    const char *key     = luaL_checkstring(L, 3);
    int fd              = luaL_checkint(L, 4);

    int64_t offset = 0;
    uint64_t limit = (uint64_t)-1;

    if (!lua_isnoneornil(L, 5))
        offset = (int64_t)luaL_checknumber(L, 5);

    if (!lua_isnoneornil(L, 6)) {
        lua_Number lim = luaL_checknumber(L, 6);
        if (lim >= 0)
            limit = (uint64_t)lim;
    }

    if (ud->client == NULL) {
        lua_pushnil(L);
        lua_newtable(L);
        lua_pushinteger(L, -1);
        lua_setfield(L, -2, "rc");
        lua_pushstring(L, "s3 client is NULL");
        lua_setfield(L, -2, "message");
        return 2;
    }

    int rc = s3_sync_put_from_fd(
        ud->client,
        bucket,
        key,
        fd,
        offset,
        limit
    );

    if (rc != 0) {
        lua_pushnil(L);
        lua_newtable(L);

        lua_pushinteger(L, rc);
        lua_setfield(L, -2, "rc");

        lua_pushstring(L, "s3_sync_put_from_fd failed");
        lua_setfield(L, -2, "message");

        return 2;
    }

    lua_pushboolean(L, 1);
    return 1;
}


/*
 * client:get_to_fd(bucket, key, fd [, offset [, limit]])
 */
static int
lua_s3_client_get_to_fd(lua_State *L)
{
    lua_s3_client_t *ud = lua_s3_check_client(L, 1);
    const char *bucket  = luaL_checkstring(L, 2);
    const char *key     = luaL_checkstring(L, 3);
    int fd              = luaL_checkint(L, 4);

    int64_t offset = 0;

    if (!lua_isnoneornil(L, 5))
        offset = (int64_t)luaL_checknumber(L, 5);

    if (ud->client == NULL) {
        lua_pushnil(L);
        lua_newtable(L);
        lua_pushinteger(L, -1);
        lua_setfield(L, -2, "rc");
        lua_pushstring(L, "s3 client is NULL");
        lua_setfield(L, -2, "message");
        return 2;
    }

    int rc = s3_sync_get_to_fd(
        ud->client,
        bucket,
        key,
        fd,
        offset
    );

    if (rc != 0) {
        lua_pushnil(L);
        lua_newtable(L);

        lua_pushinteger(L, rc);
        lua_setfield(L, -2, "rc");

        lua_pushstring(L, "s3_sync_get_to_fd failed");
        lua_setfield(L, -2, "message");

        return 2;
    }

    lua_pushboolean(L, 1);
    return 1;
}

/* GC-метод: уничтожить клиент, когда userdata собирается */
static int
lua_s3_client_gc(lua_State *L)
{
    lua_s3_client_t *ud = (lua_s3_client_t *)luaL_checkudata(L, 1, S3_LUA_CLIENT_MT);
    if (ud != NULL && ud->client != NULL) {
        s3_client_destroy(ud->client);
        ud->client = NULL;
    }
    return 0;
}

/* ---------- s3.new_client(...) ---------- */

/*
 * new_client([opts])
 *
 * opts — таблица (опционально):
 *   {
 *     endpoint   = "http://127.0.0.1:9000" или "127.0.0.1:9000",
 *     region     = "us-east-1",
 *     access_key = "user",
 *     secret_key = "12345678",
 *     use_tls    = boolean (default: false),
 *     use_sigv4  = boolean (default: true)
 *   }
 *
 * Любое из полей может прийти из env:
 *   S3_ENDPOINT, S3_REGION, S3_ACCESS_KEY, S3_SECRET_KEY
 *
 * Возвращает userdata-клиент.
 */
static int
lua_s3_new_client(lua_State *L)
{
    int has_opts = 0;
    if (!lua_isnoneornil(L, 1)) {
        luaL_checktype(L, 1, LUA_TTABLE);
        has_opts = 1;
    }

    struct ev_loop *loop = ev_default_loop(0);
    if (loop == NULL)
        return luaL_error(L, "ev_default_loop() returned NULL");

    s3_client_config_t cfg;
    s3_client_config_init_default(&cfg);

    if (s3_client_config_init_tarantool(&cfg, loop) != 0)
        return luaL_error(L, "s3_client_config_init_tarantool() failed");

    const char *endpoint = NULL;
    const char *region   = NULL;
    const char *access   = NULL;
    const char *secret   = NULL;

    if (has_opts) {
        /* endpoint / region / creds: table + env + defaults */
        endpoint = lua_s3_get_opt_string_with_env(
            L, 1, "endpoint", "S3_ENDPOINT", NULL);
        region = lua_s3_get_opt_string_with_env(
            L, 1, "region", "S3_REGION", "us-east-1");
        access = lua_s3_get_opt_string_with_env(
            L, 1, "access_key", "S3_ACCESS_KEY", NULL);
        secret = lua_s3_get_opt_string_with_env(
            L, 1, "secret_key", "S3_SECRET_KEY", NULL);

        /* use_tls */
        lua_getfield(L, 1, "use_tls");
        int use_tls = lua_toboolean(L, -1);
        lua_pop(L, 1);

        /* use_sigv4 (default = true) */
        lua_getfield(L, 1, "use_sigv4");
        int has_sigv4 = !lua_isnil(L, -1);
        int use_sigv4 = has_sigv4 ? lua_toboolean(L, -1) : 1;
        lua_pop(L, 1);

        cfg.endpoint_is_https = use_tls ? 1 : 0;
        cfg.use_aws_sigv4     = use_sigv4 ? 1 : 0;
    } else {
        /* Без таблицы — только env + дефолты. */
        endpoint = getenv("S3_ENDPOINT");
        region   = getenv("S3_REGION");
        access   = getenv("S3_ACCESS_KEY");
        secret   = getenv("S3_SECRET_KEY");

        if (region == NULL)
            region = "us-east-1";

        /* Пусть по умолчанию SigV4 включён, TLS выключен. */
        cfg.endpoint_is_https = 0;
        cfg.use_aws_sigv4     = 1;
    }

    if (endpoint == NULL)
        return luaL_error(L, "S3 endpoint is not set (opts.endpoint or S3_ENDPOINT)");

    if (access == NULL || secret == NULL)
        return luaL_error(L, "S3 access_key/secret_key are not set "
                              "(opts.access_key/opts.secret_key or "
                              "S3_ACCESS_KEY/S3_SECRET_KEY)");

    cfg.endpoint = endpoint;
    cfg.region   = region;

    s3_credentials_t creds;
    creds.access_key_id     = access;
    creds.secret_access_key = secret;
    cfg.credentials         = creds;

    s3_client_t *client = s3_client_new(&cfg);
    if (client == NULL)
        return luaL_error(L, "s3_client_new() failed");

    lua_s3_client_t *ud = (lua_s3_client_t *)lua_newuserdata(L, sizeof(*ud));
    ud->client = client;

    luaL_getmetatable(L, S3_LUA_CLIENT_MT);
    lua_setmetatable(L, -2);

    return 1;
}

/* ---------- Регистрация модуля и метатаблицы ---------- */

static const struct luaL_Reg s3_client_methods[] = {
    {"put_from_fd", lua_s3_client_put_from_fd},
    {"get_to_fd",   lua_s3_client_get_to_fd},
    {NULL, NULL}
};

static const struct luaL_Reg s3_module_funcs[] = {
    {"new_client", lua_s3_new_client},
    {NULL, NULL}
};

LUA_API int
luaopen_s3(lua_State *L)
{
    /* Метатаблица для s3.client userdata */
    luaL_newmetatable(L, S3_LUA_CLIENT_MT);

    /* __gc */
    lua_pushcfunction(L, lua_s3_client_gc);
    lua_setfield(L, -2, "__gc");

    /* __index = сама метатаблица */
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");

    /* Методы клиента в метатаблицу */
    luaL_register(L, NULL, s3_client_methods);

    lua_pop(L, 1); /* pop metatable */

    /* Таблица модуля: s3.* */
    luaL_register(L, "s3", s3_module_funcs);

    /* Стек: [module_table]; вернуть её */
    return 1;
}
