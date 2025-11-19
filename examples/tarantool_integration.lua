-- Добавляем build/ в пути поиска C-модулей
package.cpath = '../build/?.dylib;../build/?.so;' .. package.cpath

local fio = require('fio')
local s3  = require('s3')

-- box.cfg{}

-- -- Можно через opts, можно через env.
-- local client = s3.new_client{
--     endpoint   = '127.0.0.1:9000',
--     region     = 'us-east-1',
--     access_key = 'user',
--     secret_key = '12345678',
--     use_sigv4  = true,
--     use_tls    = false,
-- }

-- -- Подготовим файл для PUT
-- local tmp = fio.tempdir()
-- local path = fio.pathjoin(tmp, 'payload.txt')
-- local fh = fio.open(path, {'O_CREAT', 'O_TRUNC', 'O_WRONLY'}, 0x1A4) -- 0644
-- fh:write('hello from tarantool s3 sync api\n')
-- fh:close()

-- -- Открываем снова на чтение, получаем fd
-- fh = fio.open(path, {'O_RDONLY'})
-- local fd = fh:fileno()

-- local ok, err = client:put_from_fd('firstbucket', 'test-object', fd, 0, -1)
-- fh:close()

-- if not ok then
--     error(('PUT failed: code=%s rc=%s msg=%s'):format(err.code, err.rc, err.message))
-- end
-- print('PUT OK')

-- -- Теперь GET в другой файл
-- local out_path = fio.pathjoin(tmp, 'download.txt')
-- local out = fio.open(out_path, {'O_CREAT', 'O_TRUNC', 'O_WRONLY'}, 0x1A4)
-- local out_fd = out:fileno()

-- ok, err = client:get_to_fd('firstbucket', 'test-object', out_fd, 0, -1)
-- out:close()

-- if not ok then
--     error(('GET failed: code=%s rc=%s msg=%s'):format(err.code, err.rc, err.message))
-- end
-- print('GET OK')
