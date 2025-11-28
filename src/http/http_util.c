#include "http_util.h"

#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <openssl/md5.h>
#include <openssl/evp.h>

/* ---------- работа с s3_mem_buf_t ---------- */

int
s3_mem_buf_reserve(s3_client_t *c, s3_mem_buf_t *b, size_t need)
{
    if (b->capacity >= need)
        return 0;

    size_t new_cap = b->capacity ? b->capacity * 2 : 8192;
    while (new_cap < need)
        new_cap *= 2;

    char *p = (char *)s3_alloc(&c->alloc, new_cap);
    if (!p)
        return -1;

    if (b->data) {
        memcpy(p, b->data, b->size);
        s3_free(&c->alloc, b->data);
    }

    b->data = p;
    b->capacity = new_cap;
    return 0;
}

s3_error_code_t
s3_mem_buf_append(s3_client_t *c, s3_mem_buf_t *b,
                  const char *data, size_t len,
                  s3_error_t *err)
{
    size_t need = b->size + len + 1; /* +1 под '\0' */
    if (s3_mem_buf_reserve(c, b, need) != 0) {
        s3_error_set(err, S3_E_NOMEM,
                     "Out of memory in s3_mem_buf_append", ENOMEM, 0, 0);
        return err->code;
    }

    memcpy(b->data + b->size, data, len);
    b->size += len;
    b->data[b->size] = '\0';
    return S3_E_OK;
}

/* Упрощённый макрос для добавления строкового литерала. */
#define APPEND_STR(c, b, s, err)                                           \
    do {                                                                   \
        const char *_s = (s);                                              \
        size_t _len = strlen(_s);                                          \
        s3_error_code_t _rc =                                              \
            s3_mem_buf_append((c), (b), _s, _len, (err));                  \
        if (_rc != S3_E_OK)                                                \
            return _rc;                                                    \
    } while (0)

/* ---------- XML escape ---------- */

s3_error_code_t
s3_xml_append_escaped(s3_client_t *c, s3_mem_buf_t *b,
                      const char *s, s3_error_t *err)
{
    const char *p = s;
    while (*p) {
        const char *chunk_start = p;

        /* Ищем следующий спец-символ. */
        while (*p &&
               *p != '&' &&
               *p != '<' &&
               *p != '>' &&
               *p != '"') {
            p++;
        }

        /* Добавляем “обычный” кусок как есть. */
        if (p > chunk_start) {
            s3_error_code_t rc =
                s3_mem_buf_append(c, b, chunk_start,
                                  (size_t)(p - chunk_start), err);
            if (rc != S3_E_OK)
                return rc;
        }

        /* Эскейпим спец-символ (если есть). */
        if (*p == '\0')
            break;

        const char *ent = NULL;
        switch (*p) {
        case '&': ent = "&amp;";  break;
        case '<': ent = "&lt;";   break;
        case '>': ent = "&gt;";   break;
        case '"': ent = "&quot;"; break;
        default:
            ent = "?";
            break;
        }

        s3_error_code_t rc =
            s3_mem_buf_append(c, b, ent, strlen(ent), err);
        if (rc != S3_E_OK)
            return rc;

        p++;
    }

    return S3_E_OK;
}

/* ---------- URL-encoding для query-параметров ---------- */

int
s3_url_encode_query(s3_client_t *client, const char *src,
                    char **out, s3_error_t *error)
{
    s3_error_t local_err = S3_ERROR_INIT;
    s3_error_t *err = error ? error : &local_err;

    if (src == NULL) {
        *out = NULL;
        return 0;
    }

    size_t len = strlen(src);
    /* В худшем случае каждый символ станет %XX → *3 + 1 под '\0'. */
    size_t max_len = len * 3 + 1;

    char *dst = (char *)s3_alloc(&client->alloc, max_len);
    if (!dst) {
        s3_error_set(err, S3_E_NOMEM,
                     "Out of memory in s3_url_encode_query", ENOMEM, 0, 0);
        return -1;
    }

    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)src[i];

        /* RFC 3986 unreserved: ALPHA / DIGIT / "-" / "." / "_" / "~" */
        if ((c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '.' || c == '_' || c == '~')
        {
            dst[j++] = (char)c;
        } else {
            static const char hex[] = "0123456789ABCDEF";
            dst[j++] = '%';
            dst[j++] = hex[(c >> 4) & 0xF];
            dst[j++] = hex[c & 0xF];
        }
    }

    dst[j] = '\0';
    *out = dst;
    return 0;
}

/* ----------------- построение URL ----------------- */

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
             s3_error_t *error)
{
    s3_error_t local_err = S3_ERROR_INIT;
    s3_error_t *err = error ? error : &local_err;

    if (client->endpoint == NULL) {
        s3_error_set(err, S3_E_INVALID_ARG,
                     "endpoint must be set", 0, 0, 0);
        return err->code;
    }

    if (bucket == NULL)
        bucket = client->default_bucket;

    if (bucket == NULL) {
        s3_error_set(err, S3_E_INVALID_ARG,
                     "bucket must be set", 0, 0, 0);
        return err->code;
    }

    const char *endpoint = client->endpoint;
    size_t endpoint_len = strlen(endpoint);
    size_t bucket_len   = strlen(bucket);
    size_t key_len      = key ? strlen(key) : 0;

    /*
     * Базовый размер: endpoint + "/" + bucket + '\0'
     */
    size_t need = endpoint_len + 1 + bucket_len + 1;

    /*
     * Если есть key: добавляем "/" + key
     */
    if (key != NULL)
        need += 1 + key_len;

    char *url = (char *)s3_alloc(&client->alloc, need);
    if (url == NULL) {
        s3_error_set(err, S3_E_NOMEM,
                     "Out of memory in s3_build_url", ENOMEM, 0, 0);
        return err->code;
    }

    size_t pos = 0;

    /* Копируем endpoint */
    memcpy(url, endpoint, endpoint_len);
    pos = endpoint_len;

    /* Убираем возможный trailing slash у endpoint */
    if (pos > 0 && url[pos - 1] == '/')
        pos--;

    /* "/" + bucket */
    url[pos++] = '/';
    memcpy(url + pos, bucket, bucket_len);
    pos += bucket_len;

    /* Если есть key: "/" + key */
    if (key != NULL) {
        url[pos++] = '/';
        memcpy(url + pos, key, key_len);
        pos += key_len;
    }

    url[pos] = '\0';

    *out_url = url;
    return S3_E_OK;
}

/* ---------- ListObjectsV2 URL ---------- */

s3_error_code_t
s3_build_list_url(s3_client_t *client,
                  const s3_list_objects_opts_t *opts,
                  char **out_url,
                  s3_error_t *error)
{
    s3_error_t local_err = S3_ERROR_INIT;
    s3_error_t *err = error ? error : &local_err;

    const char *endpoint = client->endpoint;
    const char *bucket = opts->bucket ? opts->bucket : client->default_bucket;

    if (endpoint == NULL || bucket == NULL) {
        s3_error_set(err, S3_E_INVALID_ARG,
                     "endpoint and bucket must be set for LIST", 0, 0, 0);
        return err->code;
    }

    /* Кодируем prefix и continuation_token. */
    char *enc_prefix = NULL;
    char *enc_token  = NULL;

    if (opts->prefix && opts->prefix[0] != '\0') {
        if (s3_url_encode_query(client, opts->prefix, &enc_prefix, err) != 0)
            goto fail;
    }

    if (opts->continuation_token && opts->continuation_token[0] != '\0') {
        if (s3_url_encode_query(client, opts->continuation_token,
                                &enc_token, err) != 0)
            goto fail;
    }

    size_t endpoint_len = strlen(endpoint);
    size_t bucket_len   = strlen(bucket);

    size_t extra = 64; /* базовый запас под list-type и max-keys */
    if (enc_prefix)
        extra += strlen(enc_prefix) + 16;
    if (enc_token)
        extra += strlen(enc_token) + 32;

    char *url = (char *)s3_alloc(&client->alloc,
                                 endpoint_len + 1 + bucket_len + 1 + extra);
    if (!url) {
        s3_error_set(err, S3_E_NOMEM,
                     "Out of memory in s3_build_list_url", ENOMEM, 0, 0);
        goto fail;
    }

    size_t pos = 0;
    memcpy(url, endpoint, endpoint_len);
    pos = endpoint_len;
    if (pos > 0 && url[pos - 1] == '/')
        pos--;

    url[pos++] = '/';
    memcpy(url + pos, bucket, bucket_len);
    pos += bucket_len;

    url[pos++] = '?';
    pos += (size_t)sprintf(url + pos, "list-type=2");

    if (enc_prefix) {
        pos += (size_t)sprintf(url + pos, "&prefix=%s", enc_prefix);
    }

    if (opts->max_keys > 0) {
        pos += (size_t)sprintf(url + pos, "&max-keys=%u", opts->max_keys);
    }

    if (enc_token) {
        pos += (size_t)sprintf(url + pos, "&continuation-token=%s", enc_token);
    }

    url[pos] = '\0';
    *out_url = url;

    if (enc_prefix)
        s3_free(&client->alloc, enc_prefix);
    if (enc_token)
        s3_free(&client->alloc, enc_token);

    return S3_E_OK;

fail:
    if (enc_prefix)
        s3_free(&client->alloc, enc_prefix);
    if (enc_token)
        s3_free(&client->alloc, enc_token);
    *out_url = NULL;
    return err->code;
}

/* ---------- Multi-Object Delete: XML тело + URL ---------- */

s3_error_code_t
s3_build_delete_body(s3_client_t *client,
                     const s3_delete_objects_opts_t *opts,
                     s3_mem_buf_t *buf,
                     s3_error_t *error)
{
    s3_error_t local_err = S3_ERROR_INIT;
    s3_error_t *err = error ? error : &local_err;

    if (client == NULL || opts == NULL || buf == NULL ||
        opts->objects == NULL || opts->count == 0) {
        s3_error_set(err, S3_E_INVALID_ARG,
                     "invalid args in s3_build_delete_body", 0, 0, 0);
        return err->code;
    }

    /* Начинаем с пустого буфера, но переиспользуем capacity при наличии. */
    buf->size = 0;
    if (buf->data)
        buf->data[0] = '\0';

    /* Корневой элемент с namespace — обязательно по спецификации. */
    APPEND_STR(client, buf,
               "<Delete xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">\n",
               err);

    /* Quiet (если нужно). */
    if (opts->quiet) {
        APPEND_STR(client, buf, "  <Quiet>true</Quiet>\n", err);
    }

    /* Перебираем объекты. */
    for (size_t i = 0; i < opts->count; i++) {
        const s3_delete_object_t *obj = &opts->objects[i];

        if (obj->key == NULL || obj->key[0] == '\0') {
            s3_error_set(err, S3_E_INVALID_ARG,
                         "delete_objects: object key is empty", 0, 0, 0);
            return err->code;
        }

        APPEND_STR(client, buf, "  <Object>\n    <Key>", err);

        /* Эскейпим key. */
        s3_error_code_t rc =
            s3_xml_append_escaped(client, buf, obj->key, err);
        if (rc != S3_E_OK)
            return rc;

        APPEND_STR(client, buf, "</Key>\n", err);

        if (obj->version_id && obj->version_id[0] != '\0') {
            APPEND_STR(client, buf, "    <VersionId>", err);
            rc = s3_xml_append_escaped(client, buf, obj->version_id, err);
            if (rc != S3_E_OK)
                return rc;
            APPEND_STR(client, buf, "</VersionId>\n", err);
        }

        APPEND_STR(client, buf, "  </Object>\n", err);
    }

    APPEND_STR(client, buf, "</Delete>", err);

    return S3_E_OK;
}

s3_error_code_t
s3_build_delete_url(s3_client_t *client,
                    const s3_delete_objects_opts_t *opts,
                    char **out_url,
                    s3_error_t *error)
{
    s3_error_t local_err = S3_ERROR_INIT;
    s3_error_t *err = error ? error : &local_err;

    const char *endpoint = client->endpoint;
    const char *bucket = opts->bucket ? opts->bucket : client->default_bucket;
    if (!endpoint || !bucket) {
        s3_error_set(err, S3_E_INVALID_ARG,
                     "endpoint and bucket must be set for DELETE", 0, 0, 0);
        return err->code;
    }

    size_t endpoint_len = strlen(endpoint);
    size_t bucket_len   = strlen(bucket);

    /* endpoint + '/' + bucket + "?delete" + '\0' */
    const char *qs = "?delete";
    size_t qs_len = strlen(qs);
    size_t need = endpoint_len + 1 + bucket_len + qs_len + 1;

    char *url = (char *)s3_alloc(&client->alloc, need);
    if (!url) {
        s3_error_set(err, S3_E_NOMEM,
                     "Out of memory in s3_build_delete_url", ENOMEM, 0, 0);
        return err->code;
    }

    size_t pos = 0;
    memcpy(url, endpoint, endpoint_len);
    pos = endpoint_len;
    if (pos > 0 && url[pos - 1] == '/')
        pos--;

    url[pos++] = '/';
    memcpy(url + pos, bucket, bucket_len);
    pos += bucket_len;

    memcpy(url + pos, qs, qs_len);
    pos += qs_len;
    url[pos] = '\0';

    *out_url = url;
    return S3_E_OK;
}

/* ---------- Base64 ---------- */

static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789+/";

int
s3_base64_encode(const unsigned char *in, size_t in_len,
                 char *out, size_t out_cap)
{
    size_t out_len = 4 * ((in_len + 2) / 3);

    if (out_cap < out_len + 1) /* +1 для '\0' */
        return -1;

    size_t i = 0;
    size_t o = 0;

    while (i + 2 < in_len) {
        unsigned v = (in[i] << 16) | (in[i+1] << 8) | in[i+2];
        out[o++] = b64_table[(v >> 18) & 0x3F];
        out[o++] = b64_table[(v >> 12) & 0x3F];
        out[o++] = b64_table[(v >> 6)  & 0x3F];
        out[o++] = b64_table[v & 0x3F];
        i += 3;
    }

    if (i < in_len) {
        unsigned v = in[i] << 16;
        if (i + 1 < in_len)
            v |= (in[i+1] << 8);

        out[o++] = b64_table[(v >> 18) & 0x3F];
        out[o++] = b64_table[(v >> 12) & 0x3F];

        if (i + 1 < in_len) {
            out[o++] = b64_table[(v >> 6) & 0x3F];
            out[o++] = '=';
        } else {
            out[o++] = '=';
            out[o++] = '=';
        }
    }

    out[o] = '\0';
    return (int)o;
}

#if OPENSSL_VERSION_NUMBER >= 0x30000000L

static int
s3_md5_digest(const void *data, size_t len, unsigned char *out)
{
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (ctx == NULL)
        return -1;

    int rc = 0;

    if (EVP_DigestInit_ex(ctx, EVP_md5(), NULL) != 1 ||
        EVP_DigestUpdate(ctx, data, len) != 1) {
        rc = -1;
        goto done;
    }

    unsigned int md_len = 0;
    if (EVP_DigestFinal_ex(ctx, out, &md_len) != 1 || md_len != MD5_DIGEST_LENGTH) {
        rc = -1;
        goto done;
    }

done:
    EVP_MD_CTX_free(ctx);
    return rc;
}

#else

static int
s3_md5_digest(const void *data, size_t len, unsigned char *out)
{
    /* Классический MD5() для OpenSSL < 3.0 */
    MD5((const unsigned char *)data, len, out);
    return 0;
}

#endif

s3_error_code_t
s3_build_content_md5_header(const void *data, size_t len,
                            char *out, size_t out_cap,
                            s3_error_t *error)
{
    s3_error_t local_err = S3_ERROR_INIT;
    s3_error_t *err = error ? error : &local_err;

    if (data == NULL || len == 0 || out == NULL || out_cap == 0) {
        s3_error_set(err, S3_E_INVALID_ARG,
                     "invalid args for s3_build_content_md5_header",
                     0, 0, 0);
        return err->code;
    }

    unsigned char md5_raw[MD5_DIGEST_LENGTH];
	if (s3_md5_digest(data, len, md5_raw) != 0) {
		s3_error_set(err, S3_E_INTERNAL,
					"failed to compute MD5 digest", 0, 0, 0);
		return err->code;
	}

    char md5_b64[MD5_DIGEST_LENGTH * 4 / 3 + 4];
    int enc_len = s3_base64_encode(md5_raw, MD5_DIGEST_LENGTH,
                                   md5_b64, sizeof(md5_b64));
    if (enc_len < 0) {
        s3_error_set(err, S3_E_INTERNAL,
                     "base64 buffer is too small for MD5", 0, 0, 0);
        return err->code;
    }

    int n = snprintf(out, out_cap, "Content-MD5: %s", md5_b64);
    if (n <= 0 || (size_t)n >= out_cap) {
        s3_error_set(err, S3_E_INTERNAL,
                     "Content-MD5 header buffer too small", 0, 0, 0);
        return err->code;
    }

    return S3_E_OK;
}
