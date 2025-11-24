#ifndef TARANTOOL_S3_CURL_COMPAT_H_INCLUDED
#define TARANTOOL_S3_CURL_COMPAT_H_INCLUDED 1

#if defined(S3_USE_TARANTOOL_CURL)
#  include <tarantool/curl/curl.h>
#else
#  include <curl/curl.h>
#endif

#endif /* TARANTOOL_S3_CURL_COMPAT_H_INCLUDED */