#include "error.h"

void
s3_error_clear(s3_error_t *err)
{
    if (err == NULL)
        return;
    memset(err, 0, sizeof(*err));
    err->code = S3_E_OK;
}

void
s3_error_set(s3_error_t *err, s3_error_code_t code,
             const char *msg, int os_error,
             int http_status, long curl_code)
{
    if (err == NULL)
        return;

    memset(err, 0, sizeof(*err));
    err->code = code;
    err->os_error = os_error;
    err->http_status = http_status;
    err->curl_code = curl_code;

    if (msg != NULL && msg[0] != '\0') {
        /* Обрежем строку, чтобы точно влезла с '\0'. */
        size_t n = strlen(msg);
        if (n >= sizeof(err->message))
            n = sizeof(err->message) - 1;
        memcpy(err->message, msg, n);
        err->message[n] = '\0';
    }
}

const char *
s3_error_code_str(s3_error_code_t code)
{
    switch (code) {
    case S3_E_OK:           return "S3_E_OK";
    case S3_E_INVALID_ARG:  return "S3_E_INVALID_ARG";
    case S3_E_NOMEM:        return "S3_E_NOMEM";
    case S3_E_INIT:         return "S3_E_INIT";
    case S3_E_CURL:         return "S3_E_CURL";
    case S3_E_HTTP:         return "S3_E_HTTP";
    case S3_E_SIGV4:        return "S3_E_SIGV4";
    case S3_E_IO:           return "S3_E_IO";
    case S3_E_TIMEOUT:      return "S3_E_TIMEOUT";
    case S3_E_NOT_FOUND:    return "S3_E_NOT_FOUND";
    case S3_E_AUTH:         return "S3_E_AUTH";
    case S3_E_ACCESS_DENIED:return "S3_E_ACCESS_DENIED";
    case S3_E_CANCELLED:    return "S3_E_CANCELLED";
    case S3_E_INTERNAL:     return "S3_E_INTERNAL";
    default:                return "S3_E_UNKNOWN";
    }
}

const char *
s3_error_message(const s3_error_t *err)
{
    if (err == NULL)
        return "S3 internal error (NULL error pointer)";

    if (err->message[0] != '\0')
        return err->message;

    switch (err->code) {
    case S3_E_OK:
        return "Success";
    case S3_E_INVALID_ARG:
        return "Invalid argument";
    case S3_E_NOMEM:
        return "Out of memory";
    case S3_E_INIT:
        return "Initialization error";
    case S3_E_CURL:
        return "libcurl error";
    case S3_E_HTTP:
        return "HTTP error";
    case S3_E_SIGV4:
        return "AWS SigV4 signing error";
    case S3_E_IO:
        return "I/O error";
    case S3_E_TIMEOUT:
        return "Operation timed out";
    case S3_E_NOT_FOUND:
        return "Object or bucket not found";
    case S3_E_AUTH:
        return "Authentication error";
    case S3_E_ACCESS_DENIED:
        return "Access denied";
    case S3_E_CANCELLED:
        return "Operation cancelled";
    case S3_E_INTERNAL:
    default:
        return "Internal error";
    }
}

s3_error_code_t
s3_http_map_curl_error(CURLcode cc)
{
    if (cc == CURLE_OK)
        return S3_E_OK;

    switch (cc) {
    case CURLE_OPERATION_TIMEDOUT:
        return S3_E_TIMEOUT;
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_CONNECT:
        return S3_E_INIT;
    case CURLE_READ_ERROR:
    case CURLE_WRITE_ERROR:
        return S3_E_IO;
    default:
        return S3_E_CURL;
    }
}

s3_error_code_t
s3_http_map_http_status(long status)
{
    if (status >= 200 && status < 300)
        return S3_E_OK;
    if (status == 404)
        return S3_E_NOT_FOUND;
    if (status == 403)
        return S3_E_ACCESS_DENIED;
    if (status == 401)
        return S3_E_AUTH;
    if (status == 408)
        return S3_E_TIMEOUT;
    return S3_E_HTTP;
}
