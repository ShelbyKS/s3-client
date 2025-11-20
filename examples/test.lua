-- Добавляем build/ в пути поиска C-модулей
package.cpath = '../build/?.dylib;../build/?.so;' .. package.cpath

local s3 = require('s3')

local client = s3.new_client{
    endpoint   = 'http://127.0.0.1:9000',  -- твой MinIO
    region     = 'us-east-1',
    access_key = 'user',
    secret_key = '12345678',
    default_bucket = 'firstbucket',               -- если хочешь
    use_tls    = false,
    -- use_sigv4  = true,
}

local fio = require('fio')
local f = fio.open('payload.txt', {'O_RDONLY'})

local fd = f.fh  -- вот так получаем int fd
local ok, err = client:put_from_fd('firstbucket', 'obj.txt', fd, 0, nil)
if not ok then
    print('PUT failed: ', err.message, '(code= ',  err.code, 'http= ', err.http_status, ')')
end
