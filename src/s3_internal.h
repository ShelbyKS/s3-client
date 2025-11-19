#ifndef S3_INTERNAL_H_INCLUDED
#define S3_INTERNAL_H_INCLUDED

/*
 * s3_internal.h
 *
 * Внутренние определения структур, общие для реализации:
 *   - struct s3_env
 *   - struct s3_client
 *
 * Этот заголовок не является частью публичного API и
 * не должен устанавливаться или инклюдиться пользователями библиотеки.
 */

#include "s3_config.h"  /* s3_env_config, s3_client_config, s3_allocator */
#include "s3_client.h"  /* typedef struct s3_env s3_env_t; typedef struct s3_client s3_client_t */

/*
 * Полное определение структуры s3_env.
 *
 * Снаружи (в публичных заголовках) видно только opaque-тип s3_env_t.
 * Внутренняя реализация (s3_env.c, s3_client.c и т.п.) инклюдит этот заголовок,
 * чтобы иметь доступ к полям.
 */
struct s3_env {
    struct s3_env_config config;  /* копия пользовательского конфига (allocator внутри обнуляем) */
    struct s3_allocator  alloc;   /* выбранный аллокатор для этой среды */
    int                  curl_initialized;
};

/*
 * Полное определение структуры s3_client.
 *
 * Снаружи (в s3_client.h) — только typedef struct s3_client s3_client_t;
 * Здесь — фактическое содержимое, доступное реализации.
 */
struct s3_client {
    s3_env_t                 *env;    /* владеющая среда */
    struct s3_client_config   config; /* копия пользовательского конфига */
    struct s3_http_backend   *http;
};

#endif /* S3_INTERNAL_H_INCLUDED */
