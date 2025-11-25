package.cpath = '../build/?.dylib;../build/?.so;' .. package.cpath

local fio = require('fio')
local json = require('json')
local s3 = require('s3')

local BUCKET = 'test-create-bucket'

local client_easy, client_multi, err, ok = nil, nil, nil, nil

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

for _, client in ipairs({client_easy, client_multi}) do
	ok, err = client:create_bucket(BUCKET)
	print('CREATE_BUCKET:', ok, err and json.encode(err) or nil)
end