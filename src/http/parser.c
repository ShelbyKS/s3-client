#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include "s3_internal.h"
#include "s3/client.h"
#include "s3/parser.h"
#include "error.h"

static char *
s3_xml_get_text_between(s3_client_t *c,
                        const char *start,
                        const char *open_tag,
                        const char *close_tag,
                        s3_error_t *err)
{
    const char *p1 = strstr(start, open_tag);
    if (!p1)
        return NULL;
    p1 += strlen(open_tag);
    const char *p2 = strstr(p1, close_tag);
    if (!p2 || p2 < p1)
        return NULL;

    size_t len = (size_t)(p2 - p1);
    char *s = (char *)s3_alloc(&c->alloc, len + 1);
    if (!s) {
        s3_error_set(err, S3_E_NOMEM,
                     "Out of memory in xml_get_text_between", ENOMEM, 0, 0);
        return NULL;
    }
    memcpy(s, p1, len);
    s[len] = '\0';
    return s;
}

/*
 * Парсер ответа ListObjectsV2.
 * Предполагаем стандартный XML от MinIO/AWS:
 *
 *   <ListBucketResult>
 *     <IsTruncated>true|false</IsTruncated>
 *     <NextContinuationToken>...</NextContinuationToken>
 *     <Contents> ... </Contents>
 *     <Contents> ... </Contents>
 *   </ListBucketResult>
 */
s3_error_code_t
s3_parse_list_response(s3_client_t *client,
                       const char *xml,
                       s3_list_objects_result_t *out,
                       s3_error_t *error)
{
    s3_error_t local_err = S3_ERROR_INIT;
    s3_error_t *err = error ? error : &local_err;
    s3_error_clear(err);

    memset(out, 0, sizeof(*out));

    if (xml == NULL || xml[0] == '\0')
        return S3_E_OK; /* пустой ответ → пустой список */

    const char *p = xml;
    size_t capacity = 0;
    s3_object_info_t *objects = NULL;
    size_t count = 0;

    /* Сначала парсим IsTruncated и NextContinuationToken. */
    {
        char *truncated =
            s3_xml_get_text_between(client, xml,
                                    "<IsTruncated>", "</IsTruncated>", err);
        if (truncated) {
            out->is_truncated =
                (strcmp(truncated, "true") == 0 || strcmp(truncated, "True") == 0);
            s3_free(&client->alloc, truncated);
        }

        char *token =
            s3_xml_get_text_between(client, xml,
                                    "<NextContinuationToken>",
                                    "</NextContinuationToken>", err);
        out->next_continuation_token = token; /* может быть NULL */
    }

    /* Теперь собираем все <Contents>...</Contents>. */
    while ((p = strstr(p, "<Contents>")) != NULL) {
        const char *block_start = p;
        const char *block_end = strstr(block_start, "</Contents>");
        if (!block_end)
            break;
        block_end += strlen("</Contents>");

        if (count == capacity) {
            size_t new_cap = capacity ? capacity * 2 : 16;
            s3_object_info_t *tmp =
                (s3_object_info_t *)s3_alloc(&client->alloc,
                                             new_cap * sizeof(*tmp));
            if (!tmp) {
                s3_error_set(err, S3_E_NOMEM,
                             "Out of memory in s3_parse_list_response",
                             ENOMEM, 0, 0);
                /* чистим уже выделенное */
                s3_list_objects_result_t tmpres = { .objects = objects, .count = count,
                                            .is_truncated = out->is_truncated,
                                            .next_continuation_token = out->next_continuation_token };
                s3_list_objects_result_destroy(client, &tmpres);
                return err->code;
            }
            if (objects) {
                memcpy(tmp, objects, count * sizeof(*objects));
                s3_free(&client->alloc, objects);
            }
            objects = tmp;
            capacity = new_cap;
        }

        s3_object_info_t *obj = &objects[count];
        memset(obj, 0, sizeof(*obj));

        obj->key = s3_xml_get_text_between(client,
                                           block_start,
                                           "<Key>", "</Key>", err);

        char *size_str = s3_xml_get_text_between(client,
                                                 block_start,
                                                 "<Size>", "</Size>", err);
        if (size_str) {
            obj->size = (uint64_t)strtoull(size_str, NULL, 10);
            s3_free(&client->alloc, size_str);
        }

        obj->etag = s3_xml_get_text_between(client,
                                            block_start,
                                            "<ETag>", "</ETag>", err);
        if (obj->etag) {
            /* часто ETag приходит в кавычках — снимем их. */
            size_t len = strlen(obj->etag);
            if (len >= 2 && obj->etag[0] == '"' && obj->etag[len - 1] == '"') {
                obj->etag[len - 1] = '\0';
                memmove(obj->etag, obj->etag + 1, len - 1);
            }
        }

        obj->last_modified =
            s3_xml_get_text_between(client, block_start,
                                    "<LastModified>", "</LastModified>", err);

        obj->storage_class =
            s3_xml_get_text_between(client, block_start,
                                    "<StorageClass>", "</StorageClass>", err);

        count++;
        p = block_end;
    }

    out->objects = objects;
    out->count = count;
    return S3_E_OK;
}
