package.cpath = '../build/?.dylib;../build/?.so;' .. package.cpath

local s3 = require('s3')

local client_easy, client_multi, err, res = nil, nil, nil, nil

client_easy, err = s3.new{
    endpoint        = 'http://minio:9000',
    region          = 'us-east-1',
    access_key      = 'user',
    secret_key      = '12345678',
    backend         = 'easy',
    require_sigv4   = true,
}
assert(client_easy, ('s3.new failed easy: %s'):format(err and err.message or 'unknown'))

client_multi, err = s3.new{
    endpoint        = 'http://minio:9000',
    region          = 'us-east-1',
    access_key      = 'user',
    secret_key      = '12345678',
    backend         = 'multi',
    require_sigv4   = true,
}

assert(client_multi, ('s3.new failed multi: %s'):format(err and err.message or 'unknown'))

print("--------------------- test_list_objects [START] --------------------------")

for _, client in ipairs({client_easy, client_multi}) do
	res, err = client:list_objects('firstbucket', nil, 2, nil)
	if not res then
    	print('LIST error:', require('json').encode(err))
    	return
	end

	print('is_truncated:', res.is_truncated)
	print('next token:', res.next_continuation_token)

	for i, obj in ipairs(res.objects) do
		print(i, obj.key, obj.size, obj.storage_class, obj.last_modified, obj.etag)
	end
end

print("--------------------- test_list_objects [FINISHED] --------------------------")