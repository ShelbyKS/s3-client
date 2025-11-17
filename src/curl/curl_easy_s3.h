/* src/curl/curl_easy_s3.h */

#ifndef S3_CURL_EASY_S3_H
#define S3_CURL_EASY_S3_H

#include <curl/curl.h>
#include <s3/client.h>
#include <s3/types.h>

/* struct s3_request объявлен в s3/types.h (как opaque),
 * а полное определение — в client_internal.h.
 */

CURL *s3_curl_easy_create_get(const s3_client_config_t      *cfg,
                              const s3_object_get_params_t  *params,
                              s3_request_t                  *req);

CURL *s3_curl_easy_create_put(const s3_client_config_t      *cfg,
                              const s3_object_put_params_t  *params,
                              s3_request_t                  *req);

#endif /* S3_CURL_EASY_S3_H */
