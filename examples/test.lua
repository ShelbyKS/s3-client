package.cpath = '../build/?.dylib;../build/?.so;' .. package.cpath

local fio = require('fio')
local json = require('json')
local s3 = require('s3')

local client, err = s3.new{
    endpoint        = 'http://127.0.0.1:9000',
    region          = 'us-east-1',
    access_key      = 'user',
    secret_key      = '12345678',
    backend         = 'multi',       -- или 'easy'
    default_bucket  = 'firstbucket',
}

assert(client, ('s3.new failed: %s'):format(err and err.message or 'unknown'))

----------------------------------------------------------------------
-- 1. Пишем тестовый файл и грузим его через put_fd
----------------------------------------------------------------------

do
    local fh = io.open('/tmp/test.txt', 'wb')
    assert(fh, 'failed to open /tmp/test.txt for write')
    fh:write('Hello S3')
    fh:close()
end

local in_f = fio.open('/tmp/test.txt', {'O_RDONLY'})
assert(in_f, 'fio.open(/tmp/test.txt) failed')
local in_fd = in_f.fh              -- ВАЖНО: fh, не fd

local st = fio.stat('/tmp/test.txt')
assert(st, 'fio.stat failed')
local size = st.size

local ok, perr = client:put_fd(
    in_fd,
    nil,           -- bucket (nil -> default_bucket)
    'hello.txt',   -- key
    nil,           -- offset
    size           -- size
)
in_f:close()

print('PUT:', ok, perr and json.encode(perr) or nil)

----------------------------------------------------------------------
-- 2. Читаем назад через get_fd
----------------------------------------------------------------------

local out_f = fio.open('/tmp/test_out.txt',
    {'O_CREAT', 'O_WRONLY', 'O_TRUNC'}, 420) -- 0644
assert(out_f, 'fio.open(/tmp/test_out.txt) failed')
local out_fd = out_f.fh            -- снова fh

local bytes, gerr = client:get_fd(
    out_fd,
    nil,           -- bucket
    'hello.txt',   -- key
    nil,           -- offset
    0              -- max_size = 0 → без лимита
)
out_f:close()

print('GET bytes:', bytes, gerr and json.encode(gerr) or nil)

----------------------------------------------------------------------
-- 3. Сравниваем содержимое
----------------------------------------------------------------------

local f1 = fio.open('/tmp/test.txt', {'O_RDONLY'})
local f2 = fio.open('/tmp/test_out.txt', {'O_RDONLY'})
assert(f1 and f2)

local d1 = f1:read(1024)
local d2 = f2:read(1024)
f1:close()
f2:close()

print('equal:', d1 == d2)
