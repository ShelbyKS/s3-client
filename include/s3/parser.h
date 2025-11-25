#ifndef S3_LIST_PARSE_H_INCLUDED
#define S3_LIST_PARSE_H_INCLUDED

#include "s3/client.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Разбор XML ответа ListObjectsV2.
 * xml      — строка (не обязательно 0-терминированная).
 * result   — куда положить разобранный результат.
 * Память под строки/массив выделяется через client->alloc.
 */
s3_error_code_t
s3_parse_list_response(s3_client_t *client,
                       const char *xml,
                       s3_list_objects_result_t *result,
                       s3_error_t *error);

#ifdef __cplusplus
}
#endif

#endif /* S3_LIST_PARSE_H_INCLUDED */
