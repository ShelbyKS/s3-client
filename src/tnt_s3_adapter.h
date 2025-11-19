#pragma once

#include "tarantool/module.h"

#include "s3_client.h"
#include "s3_types.h"
#include "s3_error.h"

/*
 * Синхронный с точки зрения вызывающего файбера метод put_fd,
 * реализованный через coio_call.
 *
 * Вызывать можно из любого C-кода на tx-треде:
 *
 *   struct s3_error_info err;
 *   int rc = tnt_s3_put_fd_coio(client, bucket, key, fd, &params, &err);
 *
 * Он:
 *   - создаст задачу в coio-пуле;
 *   - сделает yield текущего файбера;
 *   - вернётся, когда HTTP-запрос будет выполнен;
 *   - вернёт тот же rc, что и s3_client_put_fd.
 */

int
tnt_s3_put_fd_coio(s3_client_t *client,
                   const char *bucket,
                   const char *key,
                   int src_fd,
                   const struct s3_put_params *params,
                   struct s3_error_info *err);
