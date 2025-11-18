/* examples/simple_get_put.c
 *
 * Простейший end-to-end тест:
 *   - PUT из буфера в S3;
 *   - GET того же объекта в другой буфер;
 *   - сравнение данных.
 *
 * Требует:
 *   - рабочий reactor на libev (мы используем s3_reactor_init_tarantool
 *     как "общий" libev-адаптер);
 *   - настроенный s3_client_config_t (endpoint, креды и т.п. — зависят от
 *     твоего s3/config.h и реализации signer_v4).
 *
 * Для простоты конфиг читается из env-переменных (ты подстроишь под свой config.h):
 *   S3_TEST_ENDPOINT
 *   S3_TEST_REGION
 *   S3_TEST_ACCESS_KEY
 *   S3_TEST_SECRET_KEY
 *   S3_TEST_BUCKET
 *   S3_TEST_KEY
 *
 * Даже если авторизация/endpoint пока не до конца реализованы — важно, что:
 *   - запросы уходят через curl;
 *   - колбэки вызываются;
 *   - done_cb срабатывает;
 *   - реактор и multi-контур работают.
 */

#include <s3/client.h>
#include <s3/config.h>
#include <s3/types.h>
#include <s3-adapters/reactor_tarantool.h>  /* для s3_client_config_init_tarantool */

#include <ev.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ====================== helpers ====================== */

static const char *
get_env_or(const char *name, const char *defval)
{
    const char *v = getenv(name);
    return v ? v : defval;
}

/* ====================== PUT ====================== */

typedef struct put_ctx {
    struct ev_loop *loop;
    int             done;
    s3_error_t      err;
} put_ctx_t;

static void
put_done_cb(struct s3_request *req,
            const s3_error_t  *error,
            void              *user_data)
{
    (void)req;
    put_ctx_t *ctx = (put_ctx_t *)user_data;

    ctx->done = 1;
    if (error) {
        ctx->err = *error;
    } else {
        s3_error_clear(&ctx->err);
    }

    /* Останавливаем ev_run() */
    if (ctx->loop)
        ev_break(ctx->loop, EVBREAK_ALL);
}

/* ====================== GET ====================== */

typedef struct get_ctx {
    struct ev_loop *loop;
    int             done;
    s3_error_t      err;
} get_ctx_t;

static void
get_done_cb(struct s3_request *req,
            const s3_error_t  *error,
            void              *user_data)
{
    (void)req;
    get_ctx_t *ctx = (get_ctx_t *)user_data;

    ctx->done = 1;
    if (error) {
        ctx->err = *error;
    } else {
        s3_error_clear(&ctx->err);
    }

    if (ctx->loop)
        ev_break(ctx->loop, EVBREAK_ALL);
}

/* Можно добавить headers_cb, но для мини-теста он не обязателен. */

/* ====================== main ====================== */

int
main(void)
{
    /* ---------- 1. Читаем настройки из окружения ---------- */

    const char *endpoint   = get_env_or("S3_TEST_ENDPOINT",   "127.0.0.1:9000");
    const char *region     = get_env_or("S3_TEST_REGION",     "us-east-1");
    const char *access_key = get_env_or("S3_TEST_ACCESS_KEY", "user");
    const char *secret_key = get_env_or("S3_TEST_SECRET_KEY", "12345678");
    const char *bucket     = get_env_or("S3_TEST_BUCKET",     "firstbucket");
    const char *key        = get_env_or("S3_TEST_KEY",        "test-object");

    printf("Using endpoint=%s bucket=%s key=%s\n", endpoint, bucket, key);

    /* ---------- 2. Создаём ev_loop и конфиг клиента ---------- */

    struct ev_loop *loop = ev_default_loop(0);
    if (!loop) {
        fprintf(stderr, "ev_default_loop() failed\n");
        return 1;
    }

    s3_client_config_t cfg;
    s3_client_config_init_default(&cfg);

    /* Настраиваем reactor через libev-адаптер. */
    if (s3_client_config_init_tarantool(&cfg, loop) != 0) {
        fprintf(stderr, "s3_client_config_init_tarantool() failed\n");
        return 1;
    }

    /* Дальше – адаптируй под свой s3/config.h: */
    cfg.endpoint   = endpoint;
    cfg.region     = region;

    s3_credentials_t creds;
    creds.access_key_id = access_key;
    creds.secret_access_key = secret_key;

    cfg.credentials = creds;
    /* Возможны и другие поля: use_tls, ca_path, verify_peer и т.п. */

    cfg.endpoint_is_https = 0;
    cfg.use_aws_sigv4 = 1;

    s3_client_t *client = s3_client_new(&cfg);
    if (!client) {
        fprintf(stderr, "s3_client_new() failed\n");
        return 1;
    }

    /* ---------- 3. Готовим данные для PUT ---------- */

    const char payload[] = "hello from s3-client tarantool";
    s3_buffer_t put_buf;
    put_buf.data = (uint8_t *)payload;
    put_buf.size = sizeof(payload) - 1; /* без завершающего '\0' */
    put_buf.used = sizeof(payload) - 1;

    s3_object_put_params_t put_params;
    memset(&put_params, 0, sizeof(put_params));
    put_params.bucket         = bucket;
    put_params.key            = key;
    put_params.content_length = put_buf.used;
    put_params.content_type   = "text/plain";

    put_ctx_t pctx;
    memset(&pctx, 0, sizeof(pctx));
    s3_error_clear(&pctx.err);
    pctx.loop = loop;

    printf("== PUT...\n");

    s3_request_t *put_req = s3_object_put_from_buffer(
        client,
        &put_params,
        &put_buf,
        put_done_cb,
        &pctx);

    if (put_req == NULL) {
        fprintf(stderr, "PUT: failed to start request, code=%d msg=%s\n",
                pctx.err.code,
                pctx.err.message);
        s3_client_destroy(client);
        return 1;
    }

    /* Крутим event-loop до окончания PUT. */
    ev_run(loop, 0);

    if (pctx.err.code != S3_OK) {
        fprintf(stderr, "PUT: finished with error code=%d msg=%s\n",
                pctx.err.code,
                pctx.err.message);
        s3_client_destroy(client);
        return 1;
    }

    printf("== PUT OK\n");

    // /* ---------- 4. Готовим GET в буфер ---------- */

    uint8_t get_storage[4096];
    s3_buffer_t get_buf;
    get_buf.data = get_storage;
    get_buf.size = sizeof(get_storage);
    get_buf.used = 0;

    s3_object_get_params_t get_params;
    memset(&get_params, 0, sizeof(get_params));
    get_params.bucket = bucket;
    get_params.key    = key;
    get_params.use_range = 0;

    get_ctx_t gctx;
    memset(&gctx, 0, sizeof(gctx));
    s3_error_clear(&gctx.err);
    gctx.loop = loop;

    printf("== GET...\n");

    s3_request_t *get_req = s3_object_get_to_buffer(
        client,
        &get_params,
        &get_buf,
        /* headers_cb = */ NULL,
        get_done_cb,
        &gctx);

    if (get_req == NULL) {
        fprintf(stderr, "GET: failed to start request, code=%d msg=%s\n",
                gctx.err.code,
                gctx.err.message);
        s3_client_destroy(client);
        return 1;
    }

    ev_run(loop, 0);

    if (gctx.err.code != S3_OK) {
        fprintf(stderr, "GET: finished with error code=%d msg=%s\n",
                gctx.err.code,
                gctx.err.message);
        s3_client_destroy(client);
        return 1;
    }

    printf("== GET OK, got %zu bytes\n", get_buf.used);
    printf("Payload: '%.*s'\n", (int)get_buf.used, (const char *)get_buf.data);

    // /* ---------- 5. Сравниваем, что PUT == GET ---------- */

    if (get_buf.used != put_buf.used ||
        memcmp(get_buf.data, put_buf.data, put_buf.used) != 0) {
        fprintf(stderr, "MISMATCH between PUT payload and GET payload\n");
        s3_client_destroy(client);
        return 1;
    }

    printf("== E2E OK: data matches\n");

    s3_client_destroy(client);
    return 0;
}
