#include "s3/client.h"
#include "error.h"

#include <lua.h>
#include <lauxlib.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <stdbool.h>

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

    s3_error_t tmp;
    if (err == NULL) {
        tmp = (s3_error_t)S3_ERROR_INIT;
        tmp.code = S3_E_INTERNAL;
        snprintf(tmp.message, sizeof(tmp.message),
                 "Unknown error");
        err = &tmp;
    }

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

/*
 * client:create_bucket(bucket) -> bool, err
 */
static int
l_s3_client_create_bucket(lua_State *L)
{
    struct l_s3_client *lc = l_s3_check_client(L, 1);
    s3_client_t *client = lc->client;

    const char *bucket = NULL;
    if (!lua_isnoneornil(L, 2))
        bucket = luaL_checkstring(L, 2);

    // TODO: acl, flags, etc.

    s3_create_bucket_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.bucket = bucket;

    s3_error_t err = S3_ERROR_INIT;
    s3_error_code_t rc =
        s3_client_create_bucket(client, &opts, &err);

    if (rc == S3_E_OK) {
        lua_pushboolean(L, 1);
        return 1;
    }

    lua_pushboolean(L, 0);
    l_s3_push_error(L, &err);
    return 2;
}

/*
 * client:list_objects(bucket, prefix, max_keys, continuation_token) -> result | nil, err
 *
 * bucket можно передать nil, тогда будет использоваться default_bucket.
 * prefix, max_keys, continuation_token — опциональные:
 *   prefix               — фильтр по префиксу;
 *   max_keys (integer)   — максимум объектов на странице (0 или nil → дефолт сервера);
 *   continuation_token   — токен для продолжения пагинации (nil → первая страница).
 *
 * При успехе возвращает одну таблицу:
 * {
 *   objects = {
 *     { key = "...", size = <int>, etag = "...",
 *       last_modified = "...", storage_class = "STANDARD" },
 *     ...
 *   },
 *   is_truncated = <bool>,
 *   next_continuation_token = <string|nil>,
 * }
 *
 * При ошибке: nil, err_table.
 */
static int
l_s3_client_list_objects(lua_State *L)
{
    struct l_s3_client *lc = l_s3_check_client(L, 1);
    s3_client_t *client = lc->client;

    const char *bucket = NULL;
    if (!lua_isnoneornil(L, 2))
        bucket = luaL_checkstring(L, 2);

    const char *prefix = NULL;
    if (!lua_isnoneornil(L, 3))
        prefix = luaL_checkstring(L, 3);

    uint32_t max_keys = 0;
    if (!lua_isnoneornil(L, 4)) {
        lua_Integer mk = luaL_checkinteger(L, 4);
        if (mk < 0)
            mk = 0;
        max_keys = (uint32_t)mk;
    }

    const char *continuation_token = NULL;
    if (!lua_isnoneornil(L, 5))
        continuation_token = luaL_checkstring(L, 5);

    s3_list_objects_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.bucket = bucket;                  /* может быть NULL → default_bucket */
    opts.prefix = prefix;                  /* может быть NULL */
    opts.max_keys = max_keys;              /* 0 → дефолт сервера */
    opts.continuation_token = continuation_token; /* NULL → первая страница */
    opts.flags = 0;

    s3_list_objects_result_t res;
    memset(&res, 0, sizeof(res));

    s3_error_t err = S3_ERROR_INIT;
    s3_error_code_t rc = s3_client_list_objects(client, &opts, &res, &err);

    if (rc != S3_E_OK) {
        /* На всякий случай подчистим результат, если там что-то успело заполниться. */
        s3_list_objects_result_destroy(client, &res);

        lua_pushnil(L);
        l_s3_push_error(L, &err);
        return 2;
    }

    /*
     * Строим Lua-таблицу результата:
     *
     * {
     *   objects = {
     *     { key = "...", size = 123, etag = "...",
     *       last_modified = "...", storage_class = "STANDARD" },
     *     ...
     *   },
     *   is_truncated = true/false,
     *   next_continuation_token = "..." or nil
     * }
     */

    lua_newtable(L); /* result */

    /* result.objects */
    lua_createtable(L, (int)res.count, 0); /* objects array */

    for (size_t i = 0; i < res.count; i++) {
        s3_object_info_t *o = &res.objects[i];

        lua_createtable(L, 0, 5); /* один объект */

        if (o->key != NULL) {
            lua_pushstring(L, o->key);
            lua_setfield(L, -2, "key");
        }

        lua_pushinteger(L, (lua_Integer)o->size);
        lua_setfield(L, -2, "size");

        if (o->etag != NULL) {
            lua_pushstring(L, o->etag);
            lua_setfield(L, -2, "etag");
        }

        if (o->last_modified != NULL) {
            lua_pushstring(L, o->last_modified);
            lua_setfield(L, -2, "last_modified");
        }

        if (o->storage_class != NULL) {
            lua_pushstring(L, o->storage_class);
            lua_setfield(L, -2, "storage_class");
        }

        /* objects[i+1] = объект */
        lua_rawseti(L, -2, (int)i + 1);
    }

    lua_setfield(L, -2, "objects"); /* result.objects = objects */

    /* result.is_truncated */
    lua_pushboolean(L, res.is_truncated);
    lua_setfield(L, -2, "is_truncated");

    /* result.next_continuation_token */
    if (res.next_continuation_token != NULL) {
        lua_pushstring(L, res.next_continuation_token);
    } else {
        lua_pushnil(L);
    }
    lua_setfield(L, -2, "next_continuation_token");

    /* Освобождаем все строки/массив, выделенные клиентским аллокатором. */
    s3_list_objects_result_destroy(client, &res);

    /* Возвращаем только result, без err. */
    return 1;
}


/*
 * client:delete_objects(bucket, keys[, quiet]) -> bool | nil, err
 *
 * bucket:
 *   - строка с именем бакета
 *   - или nil → будет использован default_bucket клиента
 *
 * keys:
 *   - массив строковых ключей: { "k1", "k2", "k3", ... }
 *
 * quiet (опционально):
 *   - true  → <Quiet>true</Quiet> в XML (сервер не шлёт список успешно удалённых)
 *   - false / nil → обычный режим
 */
static int
l_s3_client_delete_objects(lua_State *L)
{
    struct l_s3_client *lc = l_s3_check_client(L, 1);
    s3_client_t *client = lc->client;

    /* 2-й аргумент: bucket (может быть nil) */
    const char *bucket = NULL;
    if (!lua_isnoneornil(L, 2))
        bucket = luaL_checkstring(L, 2);

    /* 3-й аргумент: keys (обязательная таблица-массив) */
    luaL_checktype(L, 3, LUA_TTABLE);

    /* 4-й аргумент: quiet (опционально) */
    bool quiet = false;
    if (!lua_isnoneornil(L, 4))
        quiet = lua_toboolean(L, 4);

    /* Кол-во элементов в таблице keys.
     * В Tarantool/LuaJIT нет lua_rawlen, используем lua_objlen.
     */
    size_t count = lua_objlen(L, 3);
    if (count == 0) {
        /* Пустой список — считаем no-op и сразу успех. */
        lua_pushboolean(L, 1);
        return 1;
    }

    /* Массив описаний объектов храним в обычном malloc/free:
     * он нужен только на время этого вызова и не используется библиотекой
     * после возврата s3_client_delete_objects.
     */
    s3_delete_object_t *objs =
        (s3_delete_object_t *)malloc(sizeof(*objs) * count);
    if (objs == NULL) {
        s3_error_t err = S3_ERROR_INIT;
        err.code = S3_E_NOMEM;
        err.os_error = ENOMEM;
        snprintf(err.message, sizeof(err.message),
                 "Out of memory in delete_objects");

        lua_pushnil(L);
        l_s3_push_error(L, &err);
        return 2;
    }

    memset(objs, 0, sizeof(*objs) * count);

    /* Заполняем массив из Lua-таблицы:
     * ключи (строки Lua) живут до конца вызова C-функции, поэтому
     * можно сохранять указатель (без копирования).
     */
    for (size_t i = 0; i < count; i++) {
        lua_rawgeti(L, 3, (int)(i + 1));  /* keys[i+1] на вершине стека */

        const char *key = luaL_checkstring(L, -1);
        objs[i].key = key;

        lua_pop(L, 1); /* убираем key со стека */
    }

    s3_delete_objects_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.bucket  = bucket;   /* может быть NULL → default_bucket */
    opts.objects = objs;
    opts.count   = count;
    opts.quiet   = quiet;
    opts.flags   = 0;

    s3_error_t err = S3_ERROR_INIT;
    s3_error_code_t rc =
        s3_client_delete_objects(client, &opts, &err);

    free(objs);

    if (rc == S3_E_OK) {
        lua_pushboolean(L, 1);
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
    { "put_fd",         l_s3_client_put_fd },
    { "get_fd",         l_s3_client_get_fd },
    { "create_bucket",  l_s3_client_create_bucket },
    { "list_objects",   l_s3_client_list_objects },
    { "delete_objects", l_s3_client_delete_objects },
    { "close",          l_s3_client_close },
    { "__gc",           l_s3_client_gc },
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
