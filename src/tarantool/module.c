#include <lua.h>
#include <lauxlib.h>

#include <s3-tarantool.h>

/*
 * Пока что здесь — только минимальная заглушка Lua-модуля.
 *
 * В будущем:
 *   - инициализация s3_client_tarantool (создание клиента, reactor и т.д.);
 *   - заполнение s3_tarantool_sync_api;
 *   - привязка sync_get/sync_put к Lua-функциям.
 */

/*
 * Точка входа для require('s3') в Tarantool.
 *
 * Сейчас возвращает пустую таблицу:
 *
 *   local s3 = require('s3')
 */
// int
// luaopen_s3(lua_State *L)
// {
//     /* Создаём пустую таблицу как экспорт модуля. */
//     lua_newtable(L);
//     return 1;
// }
