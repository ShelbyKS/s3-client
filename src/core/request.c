/* src/core/request.c */

#include <s3/client.h>

/*
 * request.c
 *
 * В будущем здесь будет:
 *  - внутренняя логика state-machine для s3_request;
 *  - обработка событий от libcurl (через reactor);
 *  - обработка curl_multi_info_read и вызов done_cb;
 *  - возможно, вспомогательные функции для requeue/reties и т.п.
 *
 * Пока файл существует как заглушка, чтобы структура проекта была полной.
 */

/* Можно оставить файл вообще без функций — это валидный TU.
 * Если хочется "зацепку" под будущее, можно добавить static-пустышку:
 */

static void
s3_request_core_stub(void)
{
    /* nothing yet */
    (void)0;
}
