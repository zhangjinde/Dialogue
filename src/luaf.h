#ifndef DIALOGUE_LUAF
#define DIALOGUE_LUAF

#include <string.h>
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

/*
 * Call Lua from C by passing code as a string. Pass the number of expected
 * items to be left on top of the stack.
 */
void
lua_interpret (lua_State *L, const char *input, int ret_args);

/*
 * Lua Format.
 *
 * Call Lua from C by passing a format string of code. Optionally leave items
 * on the stack by preprending 'return' to your code and passing the number of
 * items to be left on the stack.
 *
 * Looks for stack variables in the form of %[1-9], e.g. "%2.__index = %1".
 * Only supports 8 stack items, 1 through 9.
 *
 * luaf returns the number of arguments left on the stack.
 *
 * Examples:
 *
 * luaf(L, "return %1", 1);
 * luaf(L, "return %1:method_with_error()", 2);
 * luaf(L, "%1:each(function(e) %2(e) end)");
 */
int
luaf (lua_State *L, const char *format, ...);

#endif
