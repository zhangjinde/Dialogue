#ifndef DIALOGUE_SCRIPT
#define DIALOGUE_SCRIPT

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#define SCRIPT_LIB "Dialogue.Actor.Script"

typedef struct Script {
    int table_reference;
    int object_reference;
    int is_loaded;
    struct Script *next;
} Script;

/*
 * Check for a Script at index. Errors if it isn't a Script.
 */
Script *
lua_check_script (lua_State *L, int index);

/*
 * Push the object of a Script at index.
 */
void
script_push_object (lua_State *L, int index);

/*
 * Push a table of a Script at index.
 */
void
script_push_table (lua_State *L, int index);

/*
 * Expects a Script table at index. Pushes the module onto the stack.
 */
void
script_push_module (lua_State *L, int index);

/*
 * Expects a Script table at index. Pushes all data onto the stack. Returns
 * the number of args pushed.
 */
int
script_push_data (lua_State *L, int index);

int 
luaopen_Dialogue_Actor_Script (lua_State *L);

#endif
