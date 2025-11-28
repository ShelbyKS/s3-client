
#include <curl/curl.h>
#include <pthread.h>
#include <string.h>

#include "s3_internal.h"
#include "error.h"

/*
 * Однократная инициализация libcurl.
 *
 * Используем pthread_once, чтобы быть thread-safe.
 */

static pthread_once_t s3_curl_init_once = PTHREAD_ONCE_INIT;
static s3_error_code_t s3_curl_init_result = S3_E_OK;
static char s3_curl_init_msg[128];

static void
s3_curl_do_global_init(void)
{
    CURLcode cc = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (cc != CURLE_OK) {
        s3_curl_init_result = S3_E_INIT;
        const char *msg = curl_easy_strerror(cc);
        size_t n = strlen(msg);
        if (n >= sizeof(s3_curl_init_msg))
            n = sizeof(s3_curl_init_msg) - 1;
        memcpy(s3_curl_init_msg, msg, n);
        s3_curl_init_msg[n] = '\0';
    } else {
        s3_curl_init_result = S3_E_OK;
        s3_curl_init_msg[0] = '\0';
    }
}

s3_error_code_t
s3_curl_global_init(s3_error_t *error)
{
    s3_error_t local_err = S3_ERROR_INIT;
    s3_error_t *err = error ? error : &local_err;

    pthread_once(&s3_curl_init_once, s3_curl_do_global_init);

    if (s3_curl_init_result != S3_E_OK) {
        s3_error_set(err, s3_curl_init_result,
                     (s3_curl_init_msg[0] ? s3_curl_init_msg :
                      "curl_global_init failed"),
                     0, 0, 0);
    } else {
        s3_error_clear(err);
        err->code = S3_E_OK;
    }

    return s3_curl_init_result;
}
