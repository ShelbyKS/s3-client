#ifndef S3_TARANTOOL_H
#define S3_TARANTOOL_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 *  s3-tarantool.h
 *
 *  Публичный C-API Tarantool-модуля s3.
 *
 *  Идея:
 *    - сам S3-клиент живёт в общей библиотеке (s3client, см. s3/client.h);
 *    - Tarantool-модуль (s3.so) поверх него предоставляет:
 *        * синхронные операции GET/PUT (через fiber + coio),
 *        * узкий C-API, чтобы другие C-модули Tarantool могли пользоваться
 *          уже настроенным клиентом (endpoint, credentials, reactor).
 *
 *  Этот хедер описывает именно этот C-API.
 *
 *  Реализацию (заполнение таблицы функций) мы можем сделать позже,
 *  не мешая сборке проекта.
 */

#include <stdint.h>

/*
 * Коды возврата для синхронных операций Tarantool-модуля.
 *
 *  0  — успех;
 *  !=0 — ошибка (детализация будет зависеть от реализации;
 *         на первом этапе можно использовать отрицательные errno или
 *         собственные коды).
 */
typedef int s3_tarantool_rc_t;

/*
 * Небольшой API синхронных операций, доступный другим C-модулям.
 *
 * Все функции:
 *   - предполагается вызывать из Tarantool-fiber контекста;
 *   - внутри блокируют fiber, но НЕ блокируют event loop
 *     (используют coio/thread pool для файлового I/O).
 */
typedef struct s3_tarantool_sync_api {

    /*
     * Синхронный скачивание объекта в файловый дескриптор.
     *
     *  - bucket, key  — S3 координаты;
     *  - fd           — уже открытый файловый дескриптор (на запись);
     *  - offset       — смещение внутри файла, с которого начинать запись;
     *
     * Возвращает:
     *  - 0   при успехе;
     *  - !=0 при ошибке.
     */
    s3_tarantool_rc_t (*get_to_fd)(
        const char *bucket,
        const char *key,
        int         fd,
        int64_t     offset);

    /*
     * Синхронная загрузка объекта из файлового дескриптора.
     *
     *  - bucket, key  — S3 координаты;
     *  - fd           — файловый дескриптор (на чтение);
     *  - offset       — смещение в файле, с которого читать;
     *  - limit        — сколько байт отправить; (uint64_t)-1 => до EOF.
     *
     * Возвращает:
     *  - 0   при успехе;
     *  - !=0 при ошибке.
     */
    s3_tarantool_rc_t (*put_from_fd)(
        const char *bucket,
        const char *key,
        int         fd,
        int64_t     offset,
        uint64_t    limit);

    /*
     * В будущем здесь можно добавить:
     *   - get_to_buffer / put_from_buffer;
     *   - delete_object;
     *   - list_objects и т.п.;
     *   - функции для получения/смены текущей конфигурации.
     */

} s3_tarantool_sync_api_t;

/*
 * Глобальный указатель на API.
 *
 *  - Инициализируется внутри Tarantool-модуля s3 (в module.c),
 *    когда модуль загружается Tarantool'ом.
 *  - Другие C-модули Tarantool могут проверять его на NULL и вызывать
 *    функции API, если он установлен.
 *
 * Пример использования:
 *
 *    #include <s3-tarantool.h>
 *
 *    if (s3_tarantool_sync_api && s3_tarantool_sync_api->get_to_fd) {
 *        int rc = s3_tarantool_sync_api->get_to_fd("bucket", "key", fd, 0);
 *        if (rc != 0) {
 *            // обработать ошибку
 *        }
 *    }
 */
extern s3_tarantool_sync_api_t *s3_tarantool_sync_api;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* S3_TARANTOOL_H */
