package.cpath = '../build/?.dylib;../build/?.so;' .. package.cpath

local json = require('json')
local s3 = require('s3')

local function random_string(len)
    local chars = 'abcdefghijklmnopqrstuvwxyz0123456789'
    local s = {}
    for i = 1, len do
        local r = math.random(#chars)
        s[i] = chars:sub(r, r)
    end
    return table.concat(s)
end

math.randomseed(os.time())

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
	local bucket = "test-" .. random_string(10)
	ok, err = client:create_bucket(bucket)
	print('CREATE_BUCKET :', bucket, ok, err and json.encode(err) or nil)
end