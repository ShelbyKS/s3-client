/*
 * s3_env.c
 *
 * Реализация глобальной среды S3-клиента (s3_env_t).
 */

#include "s3_client.h"
#include "s3_config.h"
#include "s3_error.h"
#include "s3_internal.h"

#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>

/* ---------- Дефолтный аллокатор (malloc/free) ---------- */

static void *
s3_default_malloc(void *ud, size_t size)
{
    (void)ud;
    return malloc(size);
}

static void
s3_default_free(void *ud, void *ptr)
{
    (void)ud;
    free(ptr);
}

static const struct s3_allocator s3_default_allocator = {
    .malloc_fn = s3_default_malloc,
    .free_fn   = s3_default_free,
    .ud        = NULL,
};

/* ---------- Внутренний helper для ошибок ---------- */

static int
s3_env_set_error(struct s3_error_info *err, int code, const char *msg)
{
    if (err != NULL) {
        s3_error_reset(err);
        err->code = code;
        err->http_status = 0;
        if (msg != NULL) {
            size_t n = strlen(msg);
            if (n >= sizeof(err->msg))
                n = sizeof(err->msg) - 1;
            memcpy(err->msg, msg, n);
            err->msg[n] = '\0';
        }
    }
    return code;
}

/* ---------- Публичные функции ---------- */

int
s3_env_init(s3_env_t **penv,
            const struct s3_env_config *config,
            struct s3_error_info *err)
{
    if (penv == NULL) {
        return s3_env_set_error(err, S3_EINVAL,
                                "penv must not be NULL");
    }

    *penv = NULL;

    /* 1. Выбираем аллокатор один раз */
    const struct s3_allocator *user_alloc =
        (config != NULL && config->allocator != NULL)
        ? config->allocator
        : &s3_default_allocator;

    /* 2. Аллоцируем структуру среды через выбранный аллокатор */
    s3_env_t *env = (s3_env_t *)user_alloc->malloc_fn(user_alloc->ud,
                                                      sizeof(*env));
    if (env == NULL) {
        return s3_env_set_error(err, S3_ENOMEM,
                                "failed to allocate s3_env");
    }

    /* 3. Копируем конфиг (без allocator — он уже "разрешён" в env->alloc) */
    if (config != NULL) {
        env->config = *config;
        env->config.allocator = NULL; /* чтобы не было соблазна использовать его */
    } else {
        memset(&env->config, 0, sizeof(env->config));
        env->config.curl_share_connections = 0;
    }

    /* 4. Фиксируем выбранный аллокатор внутри env */
    env->alloc = *user_alloc;
    env->curl_initialized = 0;

    /* 5. Инициализация libcurl на уровне процесса */
    CURLcode c = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (c != CURLE_OK) {
        env->alloc.free_fn(env->alloc.ud, env);
        return s3_env_set_error(err, S3_ECURL,
                                "curl_global_init failed");
    }
    env->curl_initialized = 1;

    *penv = env;

    if (err != NULL)
        s3_error_reset(err);

    return S3_OK;
}

void
s3_env_destroy(s3_env_t *env)
{
    if (env == NULL)
        return;

    if (env->curl_initialized) {
        curl_global_cleanup();
        env->curl_initialized = 0;
    }

    /* ВАЖНО: освобождаем через тот же аллокатор, что и аллоцировали */
    env->alloc.free_fn(env->alloc.ud, env);
}
