#include <jansson.h>
#include <tarantool/curl/curl.h>

#define BATCH_DELETE_LIMIT 1000
#define LIBCURL_WITH_AWS_SIGV4 0x073B00 // 7.59.0 (где впервые появился AWS SigV4)
#define MAX_URL_LEN 1024
#define MAX_HEADER_LEN 8192

static int curl_initialized = 0;

static char *strdup_safe(const char *s) {
    if (!s) return NULL;
    char *r = malloc(strlen(s) + 1);
    if (r) strcpy(r, s);
    return r;
}

struct s3c_upload_ctx {
    const char *data;
    size_t size;
    size_t sent;
};

static size_t s3c_curl_read_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    struct s3c_upload_ctx *ctx = (struct s3c_upload_ctx *)userdata;
    size_t max_bytes = size * nmemb;
    size_t bytes_left = ctx->size - ctx->sent;
    size_t bytes_to_send = bytes_left < max_bytes ? bytes_left : max_bytes;

    if (bytes_to_send > 0) {
        memcpy(ptr, ctx->data + ctx->sent, bytes_to_send);
        ctx->sent += bytes_to_send;
    }
    return bytes_to_send;
}

struct s3c_curl_file {
    int fd;
    size_t size;
};

static size_t s3c_curl_region_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	assert(ptr != NULL);

	struct region *body = (struct region *)userdata;
	const size_t bytes = size * nmemb;

	char *p = region_alloc(body, bytes);
	if (!p) {
		log_errno(ENOMEM, "region_alloc");
		return 0;
	}

	memcpy(p, ptr, bytes);
	return bytes;
}

static size_t s3c_curl_file_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    struct s3c_curl_file *body = (struct s3c_curl_file *)userdata;
    const size_t bytes = size * nmemb;

    size_t l = bytes;
    while (l > 0) {
       ssize_t n = pwrite(body->fd, ptr + bytes - l, l, body->size + bytes - l);
       int err = errno;
       if (n == 0)
           break;
       else if (n < 0 && err == EAGAIN)
           continue;
       else if (n < 0) {
           log_errno(err, "pwrite");
           return bytes - l;
       }
       l -= n;
    }

    body->size += bytes;

    return bytes;
}

static int s3c_build_url(char *url, size_t url_size, const char *endpoint, const char *bucket, const char *key) {
    if (!endpoint || !bucket || !key) return -1;

    int expected_bytes_num = snprintf(url, url_size, "%s/%s/%s", endpoint, bucket, key);
    return (expected_bytes_num < 0 || (size_t)expected_bytes_num >= url_size) ? -1 : 0;
}

static int s3c_curl_req_init(CURL *req, struct curl_slist **l, const char *url, struct s3c_ctx *ctx)
{
    curl_easy_setopt(req, CURLOPT_URL, url);
    curl_easy_setopt(req, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(req, CURLOPT_VERBOSE, ctx->verbose ? 1L : 0L);
    curl_easy_setopt(req, CURLOPT_NOPROGRESS, 1L);
    curl_easy_setopt(req, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(req, CURLOPT_TCP_KEEPALIVE, 1L);

    // TODO: make optional
    // curl_easy_setopt(req, CURLOPT_USE_SSL, CURLUSESSL_ALL);
    // curl_easy_setopt(req, CURLOPT_SSL_VERIFYPEER, 1L);
    // curl_easy_setopt(req, CURLOPT_SSL_VERIFYHOST, 2L);

#if LIBCURL_VERSION_NUM >= LIBCURL_WITH_AWS_SIGV4
    {
        char sigopt[128];
        snprintf(sigopt, sizeof(sigopt), "aws:amz:%s:s3", ctx->region);
        curl_easy_setopt(req, CURLOPT_USERPWD, NULL);
        curl_easy_setopt(req, CURLOPT_AWS_SIGV4, sigopt);
    }
#endif

    if (ctx->access_key && ctx->secret_key) {
        curl_easy_setopt(req, CURLOPT_USERNAME, ctx->access_key);
        curl_easy_setopt(req, CURLOPT_PASSWORD, ctx->secret_key);
    }

    if (ctx->connect_timeout_ms > 0)
        curl_easy_setopt(req, CURLOPT_CONNECTTIMEOUT_MS, ctx->connect_timeout_ms);
    if (ctx->request_timeout_ms > 0)
        curl_easy_setopt(req, CURLOPT_TIMEOUT_MS, ctx->request_timeout_ms);

	return 0;
}

static int s3c_curl_perform(CURL *req)
{
    CURLcode res = curl_easy_perform(req);
    int err = -1;

    switch (res) {
    case CURLE_OK:
       err = 0;
       break;
    case CURLE_UNSUPPORTED_PROTOCOL:
    case CURLE_URL_MALFORMAT:
    case CURLE_NOT_BUILT_IN:
    case CURLE_FUNCTION_NOT_FOUND:
    case CURLE_BAD_FUNCTION_ARGUMENT:
    case CURLE_UNKNOWN_OPTION:
       err = EINVAL;
       break;
    case CURLE_OUT_OF_MEMORY:
       err = ENOMEM;
       break;
    case CURLE_OPERATION_TIMEDOUT:
       err = ETIMEDOUT;
       break;
    default:
       err = EIO;
       break;
    }

    if (err)
       log_error("curl_easy_perform: %s (%d)", curl_easy_strerror(res), res);

    return err;
}

/* Public API */

struct s3c_ctx *s3c_ctx_new(
    const char *endpoint,
    const char *access_key,
    const char *secret_key,
    const char *region,
    long connect_timeout_ms,
    long request_timeout_ms, 
    int verbose
){
    if (!endpoint) return NULL;
    
    if (!curl_initialized) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        curl_initialized = 1;
    }

    struct s3c_ctx *ctx = malloc(sizeof(*ctx));
    if (!ctx) return NULL;

    memset(ctx, 0, sizeof(*ctx));
    ctx->endpoint = strdup_safe(endpoint);
    ctx->access_key = strdup_safe(access_key);
    ctx->secret_key = strdup_safe(secret_key);
    ctx->region = strdup_safe(region);
    ctx->connect_timeout_ms = connect_timeout_ms;
    ctx->request_timeout_ms = request_timeout_ms;
    ctx->verbose = verbose;

    return ctx;
}

void s3c_ctx_free(struct s3c_ctx *ctx) {
    if (!ctx) return;
    free(ctx->endpoint);
    free(ctx->region);
    free(ctx->access_key);
    free(ctx->secret_key);
    free(ctx);

    curl_global_cleanup();
}

static ssize_t s3c_put_object_sync(va_list args) {
    struct s3c_ctx *ctx        = va_arg(args, struct s3c_ctx *);
    const char *bucket         = va_arg(args, const char *);
    const char *key            = va_arg(args, const char *);
    int fd                     = va_arg(args, int);
    size_t size                = va_arg(args, size_t);
    const char *content_type   = va_arg(args, const char *);

    // TODO: better check
    if (!ctx || !bucket || !key) {
        return -1;
    }

    int err = 0;
	CURL *req = NULL;
	struct curl_slist *headers = NULL;

    char *map = (char *)mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) {
       err = errno;
       log_errno(err, "mmap");
       goto cleanup;
    }

    struct s3c_upload_ctx upctx = {
        .data = map,
        .size = size,
        .sent = 0,
    };

    char *url = region_alloc(&GLOBAL(fsc)->gc, MAX_URL_LEN + 1);
    int expected_bytes_num = snprintf(url, MAX_URL_LEN + 1, "%s/%s/%s", ctx->endpoint, bucket, key);
    if (expected_bytes_num < 0 || (size_t)expected_bytes_num >= MAX_URL_LEN) {
        log_errno(err, "s3c_build_url");
        goto cleanup;
    }

    req = curl_easy_init();
    if (!req) {
       log_errno(ENOMEM, "curl_easy_init");
       err = ENOMEM;
       goto cleanup;
    }

    err = s3c_curl_req_init(req, &headers, url, ctx);
    if (err) {
       log_errno(err, "s3c_curl_req_init");
       goto cleanup;
    }

    curl_easy_setopt(req, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(req, CURLOPT_READFUNCTION, s3c_curl_read_cb);
    curl_easy_setopt(req, CURLOPT_READDATA, &upctx);
    curl_easy_setopt(req, CURLOPT_INFILESIZE_LARGE, (curl_off_t)size);

    /* headers */
    if (content_type) {
        char h[256];
        snprintf(h, sizeof(h), "Content-Type: %s", content_type);
        headers = curl_slist_append(headers, h);
    }

    // another headers. ommited in mvp
    curl_easy_setopt(req, CURLOPT_HTTPHEADER, headers);

    err = s3c_curl_perform(req);
    if (err) {
       printf("s3c_curl_perform failed");
    }
    long status = 0;
    curl_easy_getinfo(req, CURLINFO_RESPONSE_CODE, &status);

    if (status >= 300) {
       log_errno(EIO, "curl_easy_getinfo [status_code=%ld]", status);
       err = EIO;
       goto cleanup;
    }

cleanup:
    if (map)
       munmap(map, size);
    if (headers)
       curl_slist_free_all(headers);
    if (req)
       curl_easy_cleanup(req);
    // if (err)
    // 	*err_ptr = err;

    return err ? (ssize_t)-1 : (ssize_t)0;
}

int s3c_put_object(
    struct s3c_ctx *ctx,
    const char *bucket,
    const char *key,
    int fd,
    size_t size,
    const char *content_type
){
	if (size == 0) {
        // -1 ?
		return 0;
	}

    // TODO: add observeability
    return coio_call(s3c_put_object_sync, ctx, bucket, key, fd, size, content_type);
}

static ssize_t s3c_get_object_sync(va_list args) {
    struct s3c_ctx *ctx        = va_arg(args, struct s3c_ctx *);
    const char *bucket         = va_arg(args, const char *);
    const char *key            = va_arg(args, const char *);
    int fd                     = va_arg(args, int);

    if (!ctx || !bucket || !key) return -1;

    int err;
	CURL *req = NULL;
	struct curl_slist *headers = NULL;
    struct s3c_curl_file file = {.fd = fd, .size = 0};

    char *url = region_alloc(&GLOBAL(fsc)->gc, MAX_URL_LEN + 1);
    int expected_bytes_num = snprintf(url, MAX_URL_LEN + 1, "%s/%s/%s", ctx->endpoint, bucket, key);
    if (expected_bytes_num < 0 || (size_t)expected_bytes_num >= MAX_URL_LEN) {
        log_errno(err, "s3c_build_url");
        goto cleanup;
    }

    req = curl_easy_init();
	if (!req) {
		log_errno(ENOMEM, "curl_easy_init");
		err = ENOMEM;
		goto cleanup;
	}

    err = s3c_curl_req_init(req, &headers, url, ctx);
	if (err) {
		log_errno(err, "s3c_curl_req_init");
		goto cleanup;
	}

    curl_easy_setopt(req, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(req, CURLOPT_WRITEFUNCTION, s3c_curl_file_write_cb);
	curl_easy_setopt(req, CURLOPT_WRITEDATA, &file);
    curl_easy_setopt(req, CURLOPT_HTTPHEADER, headers);

    err = s3c_curl_perform(req);
    if (err) {
       log_errno(err, "s3c_curl_perform");
       goto cleanup;
    }

    long status = 0;
    curl_easy_getinfo(req, CURLINFO_RESPONSE_CODE, &status);

    if (status >= 300) {
       log_errno(EIO, "curl_easy_getinfo [status_code=%ld]", status);
       err = EIO;
       goto cleanup;
    }

cleanup:
    if (headers)
       curl_slist_free_all(headers);
    if (req)
       curl_easy_cleanup(req);
    if (err) {
       if (ftruncate(fd, 0))
           abort();
       // *err_ptr = err;
    }

    return err ? (ssize_t)-1 : (ssize_t)file.size;
}

int s3c_get_object(
    struct s3c_ctx *ctx,
    const char *bucket,
    const char *key,
    int fd,
    size_t *size
){
    *size = (size_t)coio_call(s3c_get_object_sync, ctx, bucket, key, fd);
    return 0;
}

static ssize_t s3c_list_objects_sync(va_list args)
{
    struct s3c_ctx *ctx     = va_arg(args, struct s3c_ctx *);
    const char *bucket      = va_arg(args, const char *);
    const char *prefix      = va_arg(args, const char *);
    char **response         = va_arg(args, json_t **);

    if (!ctx || !bucket || !response) return -1;

    int err = 0;
    struct curl_slist *headers = NULL;
    struct region resp_body;
	size_t body_len = 0;
	region_create(&resp_body, mqfs_hlp_slab_cache());

    char *url = region_alloc(&GLOBAL(fsc)->gc, MAX_URL_LEN + 1);
    int expected_bytes_num;
    if (prefix && strlen(prefix) > 0)
        expected_bytes_num = snprintf(url, MAX_URL_LEN + 1, "%s/%s?list-type=2&prefix=%s", ctx->endpoint, bucket, prefix);
    else
        expected_bytes_num = snprintf(url, MAX_URL_LEN + 1, "%s/%s?list-type=2", ctx->endpoint, bucket);
    
    if (expected_bytes_num < 0 || (size_t)expected_bytes_num >= MAX_URL_LEN) {
        log_errno(err, "s3c_build_url");
        goto cleanup;
    }

    CURL *req = curl_easy_init();
    if (!req) {
        log_errno(ENOMEM, "curl_easy_init");
        err = ENOMEM;
        goto cleanup;
    }

    err = s3c_curl_req_init(req, &headers, url, ctx);
    if (err) {
        log_errno(err, "s3c_curl_req_init");
        goto cleanup;
    }

    curl_easy_setopt(req, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(req, CURLOPT_WRITEFUNCTION, s3c_curl_region_write_cb);
    curl_easy_setopt(req, CURLOPT_WRITEDATA, &resp_body);
    curl_easy_setopt(req, CURLOPT_HTTPHEADER, headers);

    err = s3c_curl_perform(req);
    if (err){
        log_errno(err, "s3c_curl_perform");
        goto cleanup;
    }

    long status = 0;
    curl_easy_getinfo(req, CURLINFO_RESPONSE_CODE, &status);

    if (status >= 300) {
       log_errno(EIO, "curl_easy_getinfo [status_code=%ld]", status);
       err = EIO;
       goto cleanup;
    }

    json_error_t jerr;
    json_t *root = NULL;

    body_len = region_used(&resp_body);
    root = json_loadb(region_join(&resp_body, body_len), body_len, 0, &jerr);
    if (!root) {
        log_error("json_loadb failed at line %d: %s", jerr.line, jerr.text);
        err = EIO;
        goto cleanup;
    }

    *response = root;

cleanup:
    json_decref(root);
    if (headers)
        curl_slist_free_all(headers);
    if (req)
        curl_easy_cleanup(req);
    region_destroy(&resp_body);
    // if (err)
	// 	*err_ptr = err;

    return err ? (ssize_t)-1 : (ssize_t)0;
}


int s3c_list_objects(
    struct s3c_ctx *ctx,
    const char *bucket,
    const char *prefix,
    json_t **response
){
    return coio_call(s3c_list_objects_sync, ctx, bucket, prefix, response);
}

static ssize_t s3c_delete_objects_sync(va_list args) {
    struct s3c_ctx *ctx      = va_arg(args, struct s3c_ctx *);
    const char *bucket       = va_arg(args, const char *);
    const char **keys        = va_arg(args, const char **);
    size_t key_count         = va_arg(args, size_t);
    json_t **response        = va_arg(args, json_t **);

    if (!ctx || !bucket || !keys || !response) return -1;

    if (key_count > BATCH_DELETE_LIMIT) {
        log_error("batch delete limit reached");
        return -1;
    }

    int err = 0;
    struct curl_slist *headers = NULL;
    struct region resp_body;
	size_t body_len = 0;
	region_create(&resp_body, mqfs_hlp_slab_cache());

    // req_body = 

    // TODO: better build xml
    struct region *gc = &GLOBAL(fsc)->gc;
    size_t xml_len = 64 + key_count * 128;
    char *xml = region_alloc(gc, xml_len);
    size_t pos = snprintf(xml, xml_len, "<Delete>");
    for (size_t i = 0; i < key_count; i++) {
        pos += snprintf(xml + pos, xml_len - pos, "<Object><Key>%s</Key></Object>", keys[i]);
    }
    snprintf(xml + pos, xml_len - pos, "</Delete>");

    // TODO: check url path
    char *url = region_alloc(&GLOBAL(fsc)->gc, MAX_URL_LEN + 1);
    int expected_bytes_num = snprintf(url, MAX_URL_LEN + 1, "%s/%s?delete", ctx->endpoint, bucket);
    if (expected_bytes_num < 0 || (size_t)expected_bytes_num >= MAX_URL_LEN) {
        log_errno(err, "s3c_build_url");
        goto cleanup;
    }

    CURL *req = curl_easy_init();
    if (!req) {
        log_errno(ENOMEM, "curl_easy_init");
        err = ENOMEM;
        goto cleanup;
    }

    err = s3c_curl_req_init(req, &headers, url, ctx);
    if (err) {
        log_errno(err, "s3c_curl_req_init");
        goto cleanup;
    }

    headers = curl_slist_append(headers, "Content-Type: application/xml");
    curl_easy_setopt(req, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(req, CURLOPT_POST, 1L);
    curl_easy_setopt(req, CURLOPT_POSTFIELDS, xml);
    curl_easy_setopt(req, CURLOPT_POSTFIELDSIZE, (long)strlen(xml));
    curl_easy_setopt(req, CURLOPT_WRITEFUNCTION, s3c_curl_region_write_cb);
    curl_easy_setopt(req, CURLOPT_WRITEDATA, &resp_body);

    err = s3c_curl_perform(req);
    if (err){
        log_errno(err, "s3c_curl_perform");
        goto cleanup;
    }

    long status = 0;
    curl_easy_getinfo(req, CURLINFO_RESPONSE_CODE, &status);

    if (status >= 300) {
       log_errno(EIO, "curl_easy_getinfo [status_code=%ld]", status);
       err = EIO;
       goto cleanup;
    }

    json_error_t jerr;
    json_t *root = NULL;

    // TODO: need to parse body carefully because s3 may delete not all elements
    body_len = region_used(&resp_body);
    root = json_loadb(region_join(&resp_body, body_len), body_len, 0, &jerr);
    if (!root) {
        log_error("json_loadb failed at line %d: %s", jerr.line, jerr.text);
        err = EIO;
        goto cleanup;
    }

    *response = root;

cleanup:
    json_decref(root);
    if (headers)
        curl_slist_free_all(headers);
    if (req)
        curl_easy_cleanup(req);
    region_destroy(&resp_body);
    // if (err)
	// 	*err_ptr = err;

    return err ? (ssize_t)-1 : (ssize_t)0;
}

int s3c_delete_objects(
    struct s3c_ctx *ctx,
    const char *bucket,
    const char **keys,
    size_t key_count,
    json_t **response
) {
    return coio_call(s3c_delete_objects_sync, ctx, bucket, keys, key_count, response);
}

static ssize_t s3c_create_bucket_sync(va_list args) {
    struct s3c_ctx *ctx      = va_arg(args, struct s3c_ctx *);
    const char *bucket  = va_arg(args, const char *);

    if (!ctx || !bucket) return -1;

    int err = 0;
    struct curl_slist *headers = NULL;

    char *url = region_alloc(&GLOBAL(fsc)->gc, MAX_URL_LEN + 1);
    int expected_bytes_num = snprintf(url, MAX_URL_LEN + 1, "%s/%s", ctx->endpoint, bucket);
    if (expected_bytes_num < 0 || (size_t)expected_bytes_num >= MAX_URL_LEN) {
        log_errno(err, "s3c_build_url");
        goto cleanup;
    }

    CURL *req = curl_easy_init();
    if (!req) {
        log_errno(ENOMEM, "curl_easy_init");
        err = ENOMEM;
        goto cleanup;
    }

    err = s3c_curl_req_init(req, &headers, url, ctx);
    if (err) {
        log_errno(err, "s3c_curl_req_init");
        goto cleanup;
    }

    // TODO: user region. it is shit
    // region xml (only if not us-east-1)
    char xml[256];
    const char *region = ctx->region;
    if (region && strcmp(region, "us-east-1") != 0) {
        snprintf(xml, sizeof(xml),
                 "<CreateBucketConfiguration xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">"
                 "<LocationConstraint>%s</LocationConstraint>"
                 "</CreateBucketConfiguration>",
                 region);

        curl_easy_setopt(req, CURLOPT_POSTFIELDS, xml);
        curl_easy_setopt(req, CURLOPT_POSTFIELDSIZE, (long)strlen(xml));
        headers = curl_slist_append(headers, "Content-Type: application/xml");
    }

    // TODO: does it work?
    curl_easy_setopt(req, CURLOPT_UPLOAD, 0L);
    curl_easy_setopt(req, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(req, CURLOPT_HTTPHEADER, headers);


    err = s3c_curl_perform(req);
    if (err) {
        log_errno(err, "s3c_curl_perform");
        goto cleanup;
    }

    long status = 0;
    curl_easy_getinfo(req, CURLINFO_RESPONSE_CODE, &status);

    if (status >= 300) {
       log_errno(EIO, "curl_easy_getinfo [status_code=%ld]", status);
       err = EIO;
       goto cleanup;
    }

    cleanup:
    if (headers)
        curl_slist_free_all(headers);
    if (req)
        curl_easy_cleanup(req);
    // if (err)
	// 	*err_ptr = err;

    return err ? (ssize_t)-1 : (ssize_t)0;
}

int s3c_create_bucket(
    struct s3c_ctx *ctx,
    const char *bucket
){
    return coio_call(s3c_create_bucket_sync, ctx, bucket);
}
