#pragma once

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

struct s3c_ctx {
    char *endpoint;
    char *access_key;
    char *secret_key;
    char *region;
    long connect_timeout_ms;
    long request_timeout_ms;
    int verbose;
};

struct s3c_ctx *s3c_ctx_new(
    const char *endpoint,
    const char *access_key,
    const char *secret_key,
    const char *region,
    long connect_timeout_ms,
    long request_timeout_ms,
    int verbose
);

void s3c_ctx_free(struct s3c_ctx *ctx);

int s3c_put_object(
    struct s3c_ctx *ctx,
    const char *bucket,
    const char *key,
    int fd,
    size_t size,
    const char *content_type
);

int s3c_get_object(
    struct s3c_ctx *ctx,
    const char *bucket,
    const char *key,
    int fd,
    size_t *size
);

int s3c_list_objects(
    struct s3c_ctx *ctx,
    const char *bucket,
    const char *prefix,
    json_t **response
);

int s3c_delete_objects(
    struct s3c_ctx *ctx,
    const char *bucket,
    const char **keys,
    size_t key_count,
    json_t **response
);

int s3c_create_bucket(
    struct s3c_ctx *ctx,
    const char *bucket
);

