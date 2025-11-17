#include <s3/client.h>
#include <string.h>

/* Строковое имя кода ошибки. */
const char *
s3_error_code_str(s3_error_code_t code)
{
    switch (code) {
    case S3_OK:                 return "S3_OK";
    case S3_E_INVALID_ARGUMENT: return "S3_E_INVALID_ARGUMENT";
    case S3_E_OUT_OF_MEMORY:    return "S3_E_OUT_OF_MEMORY";
    case S3_E_CURL_GLOBAL:      return "S3_E_CURL_GLOBAL";
    case S3_E_CURL_INIT:        return "S3_E_CURL_INIT";
    case S3_E_CURL_PERFORM:     return "S3_E_CURL_PERFORM";
    case S3_E_TIMEOUT:          return "S3_E_TIMEOUT";
    case S3_E_CANCELLED:        return "S3_E_CANCELLED";
    case S3_E_HTTP:             return "S3_E_HTTP";
    case S3_E_S3_ERROR:         return "S3_E_S3_ERROR";
    case S3_E_INTERNAL:         return "S3_E_INTERNAL";
    default:                    return "S3_E_UNKNOWN";
    }
}

/* Заполнить err значением "OK". */
void
s3_error_clear(s3_error_t *err)
{
    if (err == NULL)
        return;

    err->code        = S3_OK;
    err->http_status = 0;
    err->curl_code   = 0;
    err->message[0]  = '\0';
}
