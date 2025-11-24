#include "s3/client.h"

#include <lua.h>
#include <lauxlib.h>
#include <string.h>
#include <errno.h>

/* Имя метатабы для клиента. */
#define S3_LUA_CLIENT_MT "s3_client_mt"

struct l_s3_client {
    s3_client_t *client;
};

/* ---------- утилиты для ошибок ---------- */

static void
l_s3_push_error(lua_State *L, const s3_error_t *err)
{
    lua_newtable(L);

    lua_pushstring(L, s3_error_code_str(err->code));
    lua_setfield(L, -2, "code");

    lua_pushstring(L, s3_error_message(err));
    lua_setfield(L, -2, "message");

    lua_pushinteger(L, err->http_status);
    lua_setfield(L, -2, "http_status");

    lua_pushinteger(L, err->curl_code);
    lua_setfield(L, -2, "curl_code");

    lua_pushinteger(L, err->os_error);
    lua_setfield(L, -2, "os_error");
}

static int
l_s3_parse_backend(lua_State *L, int idx, s3_http_backend_t *out)
{
    if (lua_isnoneornil(L, idx)) {
        *out = S3_HTTP_BACKEND_CURL_EASY;
        return 0;
    }

    const char *s = luaL_checkstring(L, idx);
    if (strcmp(s, "easy") == 0) {
        *out = S3_HTTP_BACKEND_CURL_EASY;
        return 0;
    } else if (strcmp(s, "multi") == 0) {
        *out = S3_HTTP_BACKEND_CURL_MULTI;
        return 0;
    }

    luaL_error(L, "invalid backend '%s', expected 'easy' or 'multi'", s);
    return -1;
}

/* ---------- методы клиента ---------- */

static struct l_s3_client *
l_s3_check_client(lua_State *L, int idx)
{
    struct l_s3_client *c =
        (struct l_s3_client *)luaL_checkudata(L, idx, S3_LUA_CLIENT_MT);
    if (c == NULL || c->client == NULL)
        luaL_error(L, "attempt to use closed s3 client");
    return c;
}

/* client:close() */
static int
l_s3_client_close(lua_State *L)
{
    struct l_s3_client *c =
        (struct l_s3_client *)luaL_checkudata(L, 1, S3_LUA_CLIENT_MT);

    if (c->client != NULL) {
        s3_client_delete(c->client);
        c->client = NULL;
    }
    return 0;
}

/* __gc */
static int
l_s3_client_gc(lua_State *L)
{
    return l_s3_client_close(L);
}

/*
 * client:put_fd(fd, bucket, key, offset, size)
 *
 * bucket можно передать nil, тогда будет использоваться default_bucket.
 */
static int
l_s3_client_put_fd(lua_State *L)
{
    struct l_s3_client *lc = l_s3_check_client(L, 1);
    s3_client_t *client = lc->client;

    int fd = luaL_checkinteger(L, 2);

    const char *bucket = NULL;
    if (!lua_isnoneornil(L, 3))
        bucket = luaL_checkstring(L, 3);

    const char *key = luaL_checkstring(L, 4);

    off_t offset = 0;
    if (!lua_isnoneornil(L, 5))
        offset = (off_t)luaL_checkinteger(L, 5);

    size_t size = (size_t)luaL_checkinteger(L, 6);

    s3_put_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.bucket = bucket; /* может быть NULL */
    opts.key = key;
    opts.content_type = NULL;
    opts.content_length = size;
    opts.flags = 0;

    s3_error_t err = S3_ERROR_INIT;
    s3_error_code_t rc =
        s3_client_put_fd(client, &opts, fd, offset, size, &err);

    if (rc == S3_E_OK) {
        lua_pushboolean(L, 1);
        return 1;
    }

    lua_pushnil(L);
    l_s3_push_error(L, &err);
    return 2;
}

/*
 * client:get_fd(fd, bucket, key, offset, max_size) -> bytes_written | nil, err
 *
 * max_size может быть nil (или 0) → без ограничения.
 */
static int
l_s3_client_get_fd(lua_State *L)
{
    struct l_s3_client *lc = l_s3_check_client(L, 1);
    s3_client_t *client = lc->client;

    int fd = luaL_checkinteger(L, 2);

    const char *bucket = NULL;
    if (!lua_isnoneornil(L, 3))
        bucket = luaL_checkstring(L, 3);

    const char *key = luaL_checkstring(L, 4);

    off_t offset = 0;
    if (!lua_isnoneornil(L, 5))
        offset = (off_t)luaL_checkinteger(L, 5);

    size_t max_size = 0;
    if (!lua_isnoneornil(L, 6))
        max_size = (size_t)luaL_checkinteger(L, 6);

    s3_get_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.bucket = bucket;
    opts.key = key;
    opts.range = NULL;
    opts.flags = 0;

    size_t bytes_written = 0;
    s3_error_t err = S3_ERROR_INIT;
    s3_error_code_t rc =
        s3_client_get_fd(client, &opts, fd, offset, max_size,
                         &bytes_written, &err);

    if (rc == S3_E_OK) {
        lua_pushinteger(L, (lua_Integer)bytes_written);
        return 1;
    }

    lua_pushnil(L);
    l_s3_push_error(L, &err);
    return 2;
}

/* ---------- s3.new{...} ---------- */

static int
l_s3_new(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);

    s3_client_opts_t opts;
    memset(&opts, 0, sizeof(opts));

    /* endpoint (обязательно) */
    lua_getfield(L, 1, "endpoint");
    const char *endpoint = luaL_checkstring(L, -1);
    lua_pop(L, 1);

    /* region (обязательно) */
    lua_getfield(L, 1, "region");
    const char *region = luaL_checkstring(L, -1);
    lua_pop(L, 1);

    /* access_key (обязательно) */
    lua_getfield(L, 1, "access_key");
    const char *access_key = luaL_checkstring(L, -1);
    lua_pop(L, 1);

    /* secret_key (обязательно) */
    lua_getfield(L, 1, "secret_key");
    const char *secret_key = luaL_checkstring(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 1, "require_sigv4");
    bool require_sigv4 = false; /* по умолчанию basic auth */
    if (!lua_isnil(L, -1)) {
        require_sigv4 = lua_toboolean(L, -1) ? true : false;
    }
    lua_pop(L, 1);

    /* session_token (опционально) */
    lua_getfield(L, 1, "session_token");
    const char *session_token = NULL;
    if (!lua_isnil(L, -1))
        session_token = luaL_checkstring(L, -1);
    lua_pop(L, 1);

    /* default_bucket (опционально) */
    lua_getfield(L, 1, "default_bucket");
    const char *default_bucket = NULL;
    if (!lua_isnil(L, -1))
        default_bucket = luaL_checkstring(L, -1);
    lua_pop(L, 1);

    /* backend: "easy" или "multi", по умолчанию "easy" */
    lua_getfield(L, 1, "backend");
    s3_http_backend_t backend;
    l_s3_parse_backend(L, -1, &backend);
    lua_pop(L, 1);

    /* timeouts (мс), опционально */
    lua_getfield(L, 1, "connect_timeout_ms");
    uint32_t connect_timeout_ms = 0;
    if (!lua_isnil(L, -1))
        connect_timeout_ms = (uint32_t)luaL_checkinteger(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 1, "request_timeout_ms");
    uint32_t request_timeout_ms = 0;
    if (!lua_isnil(L, -1))
        request_timeout_ms = (uint32_t)luaL_checkinteger(L, -1);
    lua_pop(L, 1);



    /* allocator: пока из Lua не прокидываем, используем NULL -> malloc. */
    opts.endpoint = endpoint;
    opts.region = region;
    opts.access_key = access_key;
    opts.secret_key = secret_key;
    opts.session_token = session_token;
    opts.default_bucket = default_bucket;
    opts.require_sigv4 = require_sigv4;
    opts.backend = backend;
    opts.allocator = NULL;
    opts.connect_timeout_ms = connect_timeout_ms;
    opts.request_timeout_ms = request_timeout_ms;
    opts.flags = 0;

    s3_client_t *client = NULL;
    s3_error_t err = S3_ERROR_INIT;
    s3_error_code_t rc = s3_client_new(&opts, &client, &err);
    if (rc != S3_E_OK) {
        lua_pushnil(L);
        l_s3_push_error(L, &err);
        return 2;
    }

    struct l_s3_client *ud =
        (struct l_s3_client *)lua_newuserdata(L, sizeof(*ud));
    ud->client = client;

    luaL_getmetatable(L, S3_LUA_CLIENT_MT);
    lua_setmetatable(L, -2);

    return 1;
}

/* ---------- регистрация модуля ---------- */

static const luaL_Reg s3_client_methods[] = {
    { "put_fd",  l_s3_client_put_fd },
    { "get_fd",  l_s3_client_get_fd },
    { "close",   l_s3_client_close },
    { "__gc",    l_s3_client_gc },
    { NULL, NULL }
};

static void
l_s3_create_client_mt(lua_State *L)
{
    luaL_newmetatable(L, S3_LUA_CLIENT_MT);

    /* metatable.__index = metatable */
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");

    /* Методы и __gc. */
    luaL_setfuncs(L, s3_client_methods, 0);

    lua_pop(L, 1); /* метатаблица остаётся зарегистрированной по имени */
}

static const luaL_Reg s3_module_funcs[] = {
    { "new", l_s3_new },
    { NULL, NULL }
};

int
luaopen_s3(lua_State *L)
{
    l_s3_create_client_mt(L);

    lua_newtable(L);
    luaL_setfuncs(L, s3_module_funcs, 0);

    return 1;
}
