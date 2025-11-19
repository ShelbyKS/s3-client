#ifndef S3_TARANTOOL_H
#define S3_TARANTOOL_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 *  s3-tarantool.h
 *
 *  Синхронные helper-функции для использования s3client
 *  из C-кода под управлением Tarantool.
 *
 *  Ключевые идеи:
 *    - НИКАКИХ глобальных клиентов внутри этого API;
 *    - вызывающий код сам создаёт и хранит s3_client_t*;
 *    - все sync-функции принимают s3_client_t* явно.
 *
 *  Типичный сценарий:
 *
 *      struct ev_loop *loop = ev_default_loop(0);
 *
 *      s3_client_config_t cfg;
 *      s3_client_config_init_default(&cfg);
 *      s3_client_config_init_tarantool(&cfg, loop);
 *
 *      cfg.endpoint              = "...";
 *      cfg.region                = "...";
 *      cfg.credentials           = creds;
 *      cfg.endpoint_is_https     = 0;
 *      cfg.use_aws_sigv4         = 1;
 *
 *      s3_client_t *client = s3_client_new(&cfg);
 *
 *      int fd = open("file.bin", O_RDONLY);
 *      s3_tarantool_rc_t rc = s3_sync_put_from_fd(
 *          client, "bucket", "key", fd, 0, (uint64_t)-1);
 *
 *      close(fd);
 *      s3_client_destroy(client);
 *
 *  Реализация sync-хелперов:
 *    - внутри строит async-запрос (s3_object_*),
 *    - дожидается выполнения запроса,
 *    - возвращает код результата как обычная синхронная функция.
 */

#include <stdint.h>
#include <s3/client.h>   /* s3_client_t, s3_error_t, коды S3_... */

/*
 * Коды возврата для синхронных операций.
 *
 *  0   — успех (обычно соответствует S3_OK);
 *  !=0 — ошибка:
 *          * маппинг на s3_error_t::code будет описан в реализации;
 *          * на первом этапе можно возвращать просто s3_error.code
 *            или -errno для системных ошибок.
 */
typedef int s3_tarantool_rc_t;

/*
 * Синхронное скачивание объекта в файловый дескриптор.
 *
 *  - client  — уже инициализированный s3_client_t*, привязанный
 *              к Tarantool/libev event loop через s3_client_config_init_tarantool();
 *  - bucket, key — координаты объекта в S3;
 *  - fd          — открытый файловый дескриптор (на запись);
 *  - offset      — смещение в файле, с которого писать:
 *                    * offset >= 0  => использовать pwrite(fd, ..., offset + ...);
 *                    * offset < 0   => писать с текущей позиции (write()).
 *
 * Возвращает:
 *  - 0   при успехе;
 *  - !=0 при ошибке.
 *
 * Детали:
 *  - внутри запускается асинхронный s3_object_get_to_fd(...),
 *    event loop крутится до завершения (через ev_run()/analogue),
 *    результат ошибки конвертируется в s3_tarantool_rc_t.
 */
s3_tarantool_rc_t
s3_sync_get_to_fd(s3_client_t    *client,
                  const char     *bucket,
                  const char     *key,
                  int             fd,
                  int64_t         offset);

/*
 * Синхронная загрузка объекта из файлового дескриптора (PUT).
 *
 *  - client  — уже инициализированный s3_client_t*;
 *  - bucket, key — координаты объекта;
 *  - fd          — файловый дескриптор (на чтение);
 *  - offset      — смещение в файле, с которого читать:
 *                    * offset >= 0  => использовать pread();
 *                    * offset < 0   => читать с текущей позиции (read());
 *  - limit       — сколько байт отправить:
 *                    * (uint64_t)-1 => читать до EOF.
 *
 * Возвращает:
 *  - 0   при успехе;
 *  - !=0 при ошибке.
 */
s3_tarantool_rc_t
s3_sync_put_from_fd(s3_client_t  *client,
                    const char   *bucket,
                    const char   *key,
                    int           fd,
                    int64_t       offset,
                    uint64_t      limit);

/*
 * В будущем здесь можно добавить:
 *
 *    s3_tarantool_rc_t
 *    s3_sync_get_to_buffer(s3_client_t   *client,
 *                          const char    *bucket,
 *                          const char    *key,
 *                          s3_buffer_t   *buf);
 *
 *    s3_tarantool_rc_t
 *    s3_sync_put_from_buffer(s3_client_t *client,
 *                            const char  *bucket,
 *                            const char  *key,
 *                            s3_buffer_t *buf);
 *
 * и т.п. — но всё равно с явным s3_client_t*.
 */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* S3_TARANTOOL_H */
