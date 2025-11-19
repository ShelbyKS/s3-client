/*
 * src/tarantool/module.c
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
 *       use_sigv4  = true       -- опционально, сейчас всегда true
 *   }
 *
 *   -- sync PUT из fd (через coio_call внутри tnt_s3_adapter):
 *   local ok, err = client:put_from_fd('bucket', 'key', fd, offset, limit)
 *
 *   fd     — файловый дескриптор (fio.open(...):fd());
 *   offset — смещение в файле (пока должно быть 0);
 *   limit  — сколько байт писать (пока не поддерживается, должно быть nil).
 *
 * Важно:
 *   - event loop крутит сам Tarantool;
 *   - внутри библиотеки s3_client_* синхронные и блокирующие;
 *   - неблокирующее поведение для tx-треда обеспечивается в tnt_s3_adapter.c
 *     через coio_call(...) + worker-пул.
 */

#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <tarantool/module.h>

#include <s3_client.h>
#include <s3_config.h>
#include <s3_error.h>
#include <s3_types.h>
#include "tnt_s3_adapter.h"

#define S3_LUA_CLIENT_MT "s3.client"

/* Глобальная env для всех клиентов */
static s3_env_t *g_s3_env = NULL;

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

/* Прочитать строку из таблицы (опционально, с env и значением по умолчанию) */
static const char *
lua_s3_get_opt_string_with_env(lua_State *L, int idx,
                               const char *field,
                               const char *env_name,
                               const char *default_val)
{
    const char *res = NULL;

    if (env_name != NULL)
        res = getenv(env_name);

    lua_getfield(L, idx, field);
    if (!lua_isnil(L, -1))
        res = lua_tostring(L, -1);
    lua_pop(L, 1);

    if (res == NULL)
        res = default_val;

    return res;
}

/* Аккуратно заполнить Lua-таблицу ошибки из s3_error_info */
static void
lua_s3_push_error(lua_State *L, const struct s3_error_info *err)
{
    lua_newtable(L);

    lua_pushinteger(L, err ? err->code : -1);
    lua_setfield(L, -2, "code");

    lua_pushinteger(L, err ? err->http_status : 0);
    lua_setfield(L, -2, "http_status");

    lua_pushstring(L, err && err->msg[0] != '\0' ? err->msg : "unknown error");
    lua_setfield(L, -2, "message");
}

/* ---------- Методы клиента ---------- */

/*
 * client:put_from_fd(bucket, key, fd [, offset [, limit]])
 *
 * offset и limit пока не поддержаны — требуем offset == 0 и limit == nil.
 */
static int
lua_s3_client_put_from_fd(lua_State *L)
{
    lua_s3_client_t *ud = lua_s3_check_client(L, 1);
    const char *bucket  = luaL_checkstring(L, 2);
    const char *key     = luaL_checkstring(L, 3);
    int fd              = (int)luaL_checkinteger(L, 4);

    long long offset = 0;
    long long limit  = -1;

    if (!lua_isnoneornil(L, 5))
        offset = (long long)luaL_checkinteger(L, 5);

    if (!lua_isnoneornil(L, 6))
        limit = (long long)luaL_checkinteger(L, 6);

    if (ud->client == NULL) {
        lua_pushnil(L);
        lua_s3_push_error(L, NULL);
        lua_pushstring(L, "s3 client is NULL");
        lua_setfield(L, -2, "message");
        return 2;
    }

    if (offset != 0) {
        lua_pushnil(L);
        lua_newtable(L);
        lua_pushinteger(L, S3_EINVAL);
        lua_setfield(L, -2, "code");
        lua_pushstring(L, "offset != 0 is not supported yet");
        lua_setfield(L, -2, "message");
        return 2;
    }

    if (limit >= 0) {
        /* Можно будет поддержать позже через content_length/INFILESIZE_LARGE. */
        lua_pushnil(L);
        lua_newtable(L);
        lua_pushinteger(L, S3_EINVAL);
        lua_setfield(L, -2, "code");
        lua_pushstring(L, "limit is not supported yet (must be nil)");
        lua_setfield(L, -2, "message");
        return 2;
    }

    struct s3_put_params params;
    memset(&params, 0, sizeof(params));
    params.offset         = 0;  /* пока не поддерживаем смещения внутри fd */
    params.content_length = 0;  /* 0 => не задавать Content-Length явно */
    params.content_type   = "application/octet-stream";

    struct s3_error_info err;
    s3_error_reset(&err);

    int rc = tnt_s3_put_fd_coio(ud->client, bucket, key, fd, &params, &err);
    if (rc != S3_OK) {
        lua_pushnil(L);
        lua_s3_push_error(L, &err);
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
        s3_client_delete(ud->client);
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
 *     endpoint      = "http://127.0.0.1:9000",
 *     region        = "us-east-1",
 *     access_key    = "user",
 *     secret_key    = "12345678",
 *     default_bucket = "bucket",     -- опционально
 *     use_tls       = boolean,
 *     use_sigv4     = boolean (зарезервировано, сейчас всегда true)
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

    /* 1. Инициализируем глобальную среду (один раз на процесс/модуль) */
    struct s3_error_info err;
    s3_error_reset(&err);

    if (g_s3_env == NULL) {
        struct s3_env_config env_cfg;
        memset(&env_cfg, 0, sizeof(env_cfg));
        env_cfg.curl_share_connections = 0;
        env_cfg.allocator = NULL;

        int rc_env = s3_env_init(&g_s3_env, &env_cfg, &err);
        if (rc_env != S3_OK) {
            return luaL_error(L, "s3_env_init failed: %s", err.msg);
        }
    }

    /* 2. Собираем конфиг клиента */
    struct s3_client_config cfg;
    memset(&cfg, 0, sizeof(cfg));

    const char *endpoint = NULL;
    const char *region   = NULL;
    const char *access   = NULL;
    const char *secret   = NULL;
    const char *def_bucket = NULL;

    if (has_opts) {
        endpoint = lua_s3_get_opt_string_with_env(
            L, 1, "endpoint", "S3_ENDPOINT", NULL);
        region = lua_s3_get_opt_string_with_env(
            L, 1, "region", "S3_REGION", "us-east-1");
        access = lua_s3_get_opt_string_with_env(
            L, 1, "access_key", "S3_ACCESS_KEY", NULL);
        secret = lua_s3_get_opt_string_with_env(
            L, 1, "secret_key", "S3_SECRET_KEY", NULL);

        lua_getfield(L, 1, "default_bucket");
        if (!lua_isnil(L, -1))
            def_bucket = lua_tostring(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, 1, "use_tls");
        int use_tls = lua_toboolean(L, -1);
        lua_pop(L, 1);

        /* use_sigv4 пока не используется, но читаем флаг для будущего */
        lua_getfield(L, 1, "use_sigv4");
        int has_sigv4 = !lua_isnil(L, -1);
        int use_sigv4 = has_sigv4 ? lua_toboolean(L, -1) : 1;
        (void)use_sigv4;
        lua_pop(L, 1);

        cfg.use_https = use_tls ? 1 : 0;
    } else {
        endpoint = getenv("S3_ENDPOINT");
        region   = getenv("S3_REGION");
        access   = getenv("S3_ACCESS_KEY");
        secret   = getenv("S3_SECRET_KEY");

        if (region == NULL)
            region = "us-east-1";

        cfg.use_https = 0;
    }

    if (endpoint == NULL)
        return luaL_error(L,
                          "S3 endpoint is not set (opts.endpoint or S3_ENDPOINT)");

    if (access == NULL || secret == NULL)
        return luaL_error(L,
                          "S3 access_key/secret_key are not set "
                          "(opts.access_key/opts.secret_key or "
                          "S3_ACCESS_KEY/S3_SECRET_KEY)");

    cfg.endpoint      = endpoint;
    cfg.region        = region;
    cfg.access_key    = access;
    cfg.secret_key    = secret;
    cfg.session_token = NULL;
    cfg.default_bucket = def_bucket;

    cfg.verify_peer = 0;
    cfg.verify_host = 0;
    cfg.connect_timeout_ms = 5000;
    cfg.request_timeout_ms = 30000;

    /* 3. Создаём клиент */
    s3_client_t *client = NULL;
    s3_error_reset(&err);

    int rc = s3_client_new(g_s3_env, &cfg, &client, &err);
    if (rc != S3_OK || client == NULL) {
        return luaL_error(L,
                          "s3_client_new failed: %s (code=%d http=%d)",
                          err.msg, err.code, err.http_status);
    }

    /* 4. Оборачиваем в userdata */
    lua_s3_client_t *ud = (lua_s3_client_t *)lua_newuserdata(L, sizeof(*ud));
    ud->client = client;

    luaL_getmetatable(L, S3_LUA_CLIENT_MT);
    lua_setmetatable(L, -2);

    return 1;
}

/* ---------- Регистрация модуля и метатаблицы ---------- */

static const struct luaL_Reg s3_client_methods[] = {
    {"put_from_fd", lua_s3_client_put_from_fd},
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
