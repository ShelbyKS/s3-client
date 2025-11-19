/*
 * s3_error.c
 *
 * Реализация функций работы с ошибками для S3-клиента.
 */

#include "s3_error.h"

#include <stddef.h>

/**
 * Вернуть строковое представление high-level кода ошибки.
 *
 * Эти строки:
 *  - статические (живут в процессе всё время);
 *  - не зависят от конкретного HTTP-ответа или текста из libcurl;
 *  - предназначены для простых логов/отладки.
 *
 * Для более детальной диагностики используйте s3_error_info.msg.
 */
const char *
s3_strerror(int code)
{
    switch (code) {
    case S3_OK:         return "no error";
    case S3_EINVAL:     return "invalid argument";
    case S3_ENOMEM:     return "out of memory";
    case S3_EIO:        return "local I/O error";
    case S3_ENETWORK:   return "network error";
    case S3_EHTTP:      return "HTTP error";
    case S3_ECURL:      return "libcurl error";
    case S3_EAUTH:      return "authentication/authorization error";
    case S3_ESERVER:    return "server-side error";
    case S3_ECANCELLED: return "operation cancelled";
    default:            return "unknown s3 error code";
    }
}

/**
 * Сбросить структуру ошибки в состояние "нет ошибки".
 */
void
s3_error_reset(struct s3_error_info *err)
{
    if (err == NULL)
        return;

    err->code = S3_OK;
    err->http_status = 0;
    err->msg[0] = '\0';
}
