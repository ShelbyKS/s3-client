/* src/core/signer_v4.c */

#include <s3/config.h>
#include <s3/client.h>

/*
 * signer_v4.c
 *
 * В будущем здесь будет реализация AWS Signature Version 4:
 *   - построение canonical request;
 *   - string to sign;
 *   - вычисление HMAC-SHA256;
 *   - добавление заголовков Authorization, X-Amz-Date, X-Amz-Security-Token;
 *   - поддержка region/endpoint из s3_client_config_t.
 *
 * Сейчас это stub-файл, чтобы проект собирался и структура core/ была полной.
 */

static void
s3_signer_v4_stub(void)
{
    /* nothing yet */
    (void)0;
}
