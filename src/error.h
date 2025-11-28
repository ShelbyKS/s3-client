#include <string.h>

#include "s3_internal.h"
#include <s3/curl_compat.h>


/* Обнулить ошибку и выставить code = S3_E_OK. */
void
s3_error_clear(s3_error_t *err);

/* Заполнить структуру ошибки. msg можно NULL. */
void
s3_error_set(s3_error_t *err, s3_error_code_t code,
             const char *msg, int os_error,
             int http_status, long curl_code);

/* ----------------- маппинг ошибок CURL/HTTP -> s3_error ----------------- */
s3_error_code_t
s3_http_map_curl_error(CURLcode cc);

s3_error_code_t
s3_http_map_http_status(long status);