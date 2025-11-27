#ifndef S3_HTTP_UTIL_H
#define S3_HTTP_UTIL_H

#include "s3_internal.h"
#include "s3/curl_easy_factory.h"

/*
 * Утилиты для работы с s3_mem_buf_t, XML и URL.
 *
 * Это внутренний API для HTTP-слоя (curl_easy_factory, http_easy, http_multi).
 */

/*
 * Гарантирует, что в буфере b есть хотя бы `need` байт capacity.
 * Если capacity уже достаточен — ничего не делает.
 * При необходимости аллоцирует новый буфер через client->alloc,
 * копирует старые данные и освобождает старый.
 *
 * Возвращает 0 при успехе, -1 при OOM.
 */
int
s3_mem_buf_reserve(s3_client_t *c, s3_mem_buf_t *b, size_t need);

/*
 * Добавить произвольный кусок данных в s3_mem_buf_t, с завершающим '\0'.
 * При нехватке места расширяет буфер через s3_mem_buf_reserve().
 *
 * Возвращает S3_E_OK или ошибку (обычно S3_E_NOMEM).
 */
s3_error_code_t
s3_mem_buf_append(s3_client_t *c, s3_mem_buf_t *b,
                  const char *data, size_t len,
                  s3_error_t *err);

/*
 * Добавить XML-эскейпнутую строку (для Key, VersionId и т.п.).
 * Эскейпятся: &, <, >, ".
 */
s3_error_code_t
s3_xml_append_escaped(s3_client_t *c, s3_mem_buf_t *b,
                      const char *s, s3_error_t *err);

/*
 * Простейший URL-энкодер для query-параметров.
 *
 * src → out (через client->alloc), out всегда 0-терминирован.
 * Разрешены только RFC 3986 unreserved: ALPHA / DIGIT / "-" / "." / "_" / "~".
 * Всё остальное кодируется как %HH.
 *
 * Возвращает 0 при успехе, -1 при OOM.
 */
int
s3_url_encode_query(s3_client_t *client, const char *src,
                    char **out, s3_error_t *error);


/* TODO: добавить URL-encoding key (кроме '/'). */
/*
 * Построение URL.
 *
 * Варианты:
 *   1) bucket != NULL, key != NULL  →  /bucket/key
 *   2) bucket != NULL, key == NULL  →  /bucket
 *
 * endpoint не должен заканчиватьcя слэшем.
 */
s3_error_code_t
s3_build_url(s3_client_t *client,
             const char *bucket,
             const char *key,         /* может быть NULL */
             char **out_url,
             s3_error_t *error);

/*
 * Построение URL для ListObjectsV2:
 *   endpoint/bucket?list-type=2[&prefix=...][&max-keys=...][&continuation-token=...]
 *
 * out_url аллоцируется через client->alloc.
 */
s3_error_code_t
s3_build_list_url(s3_client_t *client,
                  const s3_list_objects_opts_t *opts,
                  char **out_url,
                  s3_error_t *error);

/*
 * Построение XML-тела для Multi-Object Delete в buf.
 *
 * Формат:
 * <Delete xmlns="http://s3.amazonaws.com/doc/2006-03-01/">
 *   <Quiet>true</Quiet>   <!-- если opts->quiet -->
 *   <Object>
 *     <Key>...</Key>
 *     <VersionId>...</VersionId>  <!-- опционально -->
 *   </Object>
 *   ...
 * </Delete>
 */
s3_error_code_t
s3_build_delete_body(s3_client_t *client,
                     const s3_delete_objects_opts_t *opts,
                     s3_mem_buf_t *buf,
                     s3_error_t *error);

/*
 * Построение URL для Multi-Object Delete:
 *   endpoint/bucket?delete
 *
 * out_url аллоцируется через client->alloc.
 */
s3_error_code_t
s3_build_delete_url(s3_client_t *client,
                    const s3_delete_objects_opts_t *opts,
                    char **out_url,
                    s3_error_t *error);

/*
 * Стандартный Base64 (RFC 4648, без переносов строк).
 *
 * Возвращает длину результата или -1, если out_cap недостаточен.
 */
int
s3_base64_encode(const unsigned char *in, size_t in_len,
                 char *out, size_t out_cap);

#endif /* S3_HTTP_UTIL_H */
