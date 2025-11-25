-- stress_multi.lua
--
-- Небольшой стресс-тест для S3-клиента:
--  - создаёт 100 fibers
--  - каждый делает PUT + GET своего объекта
--  - измеряет общее время
--
-- Важно: предполагаем, что:
--   * модуль s3.so лежит в ../build/
--   * MinIO/S3 доступен по endpoint'у ниже
--   * bucket "firstbucket" уже существует

package.cpath = '../build/?.dylib;../build/?.so;' .. package.cpath

local fiber = require('fiber')
local fio   = require('fio')
local clock = require('clock')
local json  = require('json')
local s3    = require('s3')

local NUM_FIBERS = 100
local BUCKET     = 'firstbucket'
local OBJECT_PREFIX = 'stress-object-'

-- Настройки клиента: меняй backend на 'easy' или 'multi' для сравнения
local CLIENT_BACKEND = 'multi'
-- local CLIENT_BACKEND = 'easy'

local function new_client()
    local client, err = s3.new{
        endpoint        = 'http://minio:9000',
        region          = 'us-east-1',
        access_key      = 'user',
        secret_key      = '12345678',
        default_bucket  = BUCKET,
        backend         = CLIENT_BACKEND,

        -- Чуть более жёсткие таймауты, чтобы быстрее видеть проблемы
        connect_timeout_ms       = 2000,
        request_timeout_ms       = 10000,
        max_total_connections    = 64,
        max_connections_per_host = 16,
		multi_idle_timeout_ms = 10,
        require_sigv4   = true,

        -- Для локального http MinIO TLS можно не трогать
        ca_file = nil,
        ca_path = nil,
        proxy   = nil,

        -- при необходимости можно добавить флаги:
        -- flags = { skip_peer_verification = true, force_path_style = true },
    }

    if not client then
        error(('s3.new failed: %s'):format(err and err.message or 'unknown'))
    end
    return client
end

local function prepare_payload_file()
    local path = '/tmp/s3_stress_payload.txt'
    local fh = io.open(path, 'wb')
    assert(fh, 'failed to open payload file')
    -- небольшой, но не совсем tiny payload
    fh:write(('Hello S3 stress test! '):rep(1000)) -- ~22 KB
    fh:close()

    local st = fio.stat(path)
    assert(st and st.size > 0, 'failed to stat payload file')
    return path, st.size
end

local function run_worker(client, worker_id, payload_path, payload_size, results)
    local ok, err_msg = pcall(function()
        local key = OBJECT_PREFIX .. worker_id .. '.txt'

        -- PUT
        local in_f = fio.open(payload_path, {'O_RDONLY'})
        assert(in_f, 'fio.open(payload) failed')
        local in_fd = in_f.fh

        local ok_put, perr = client:put_fd(
            in_fd,
            nil,       -- bucket (nil -> default_bucket)
            key,
            nil,       -- offset
            payload_size
        )
        in_f:close()

        if not ok_put then
            error(('PUT failed: %s'):format(perr and json.encode(perr) or 'unknown'))
        end

        -- GET
        local out_path = string.format('/tmp/s3_stress_out_%d.txt', worker_id)
        local out_f = fio.open(out_path, {'O_CREAT', 'O_WRONLY', 'O_TRUNC'}, 420)
        assert(out_f, 'fio.open(out) failed')
        local out_fd = out_f.fh

        local bytes, gerr = client:get_fd(
            out_fd,
            nil,   -- bucket (nil -> default_bucket)
            key,
            nil,   -- offset
            0      -- max_size = 0 → без лимита
        )
        out_f:close()

        if not bytes then
            error(('GET failed: %s'):format(gerr and json.encode(gerr) or 'unknown'))
        end

        -- Можно по желанию проверить равенство содержимого (но это 100 раз × 22KB)
        -- Для стресса достаточно проверить, что bytes > 0
        if bytes ~= payload_size then
            -- не считаем фатальной, просто логируем
            print(string.format(
                '[worker %d] WARNING: bytes mismatch: got %d, expected %d',
                worker_id, bytes, payload_size
            ))
        end
    end)

    results[worker_id] = { ok = ok, err = err_msg }
end

local function main()
    print("--------------------- test_multi [START] --------------------------")

    print(string.format('Starting S3 stress test: backend=%s, fibers=%d',
        CLIENT_BACKEND, NUM_FIBERS))

    local client = new_client()
    local payload_path, payload_size = prepare_payload_file()

    print(string.format('Payload: %s (%d bytes)', payload_path, payload_size))

    local fibers = {}
    local results = {}

    local start_time = clock.monotonic()

    for i = 1, NUM_FIBERS do
        fibers[i] = fiber.create(run_worker, client, i, payload_path, payload_size, results)
        fibers[i]:set_joinable(true)
    end

    for i = 1, NUM_FIBERS do
        local ok, err = fibers[i]:join()
        if not ok then
            print(string.format('[fiber %d] join error: %s', i, tostring(err)))
        end
    end

    local total_time = clock.monotonic() - start_time

    -- Собираем статистику
    local success = 0
    local failed  = 0

    for i = 1, NUM_FIBERS do
        local r = results[i]
        if r and r.ok then
            success = success + 1
        else
            failed = failed + 1
            print(string.format('[worker %d] FAILED: %s', i, r and r.err or 'no result'))
        end
    end

    print('--- S3 stress test finished ---')
    print('backend: ', CLIENT_BACKEND)
    print('fibers:  ', NUM_FIBERS)
    print('success: ', success)
    print('failed:  ', failed)
    print(string.format('total time: %.3f sec', total_time))
    print(string.format('avg per request (PUT+GET): %.3f ms',
        (total_time / NUM_FIBERS) * 1000))

    print("--------------------- test_multi [FINISHED] --------------------------")
end

main()
