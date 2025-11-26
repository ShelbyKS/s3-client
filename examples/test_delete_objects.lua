package.cpath = '../build/?.dylib;../build/?.so;' .. package.cpath

print("--------------------- test_delete_objects [START] --------------------------")

local s3 = require('s3')
local fio = require('fio')

local client_easy, client_multi, err = nil, nil, nil

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
	local files = {}

	for i = 1, 3 do
		local file_name = '/tmp/del-' .. i .. '.txt'
		table.insert(files, file_name)

		local f = io.open(file_name, 'wb')
		assert(f, 'failed to open ', file_name)
		f:write('to delete ' .. i)
		f:close()

		f = fio.open(file_name, {'O_RDONLY'})
		local fd = f.fh
		local st = fio.stat(file_name)
		assert(st, 'fio.stat failed')
		local size = st.size

		local ok, err = client:put_fd(fd, 'firstbucket', file_name, nil, size)
		f:close()
		if not ok then
			print('PUT error:', require('json').encode(err))
		end
	end

	-- удалим пачкой
	local ok, err = client:delete_objects('firstbucket', files, true)  -- quiet=true

	print('DELETE:', ok, err and require('json').encode(err) or nil)
end

print("--------------------- test_delete_objects [FINISHED] --------------------------")