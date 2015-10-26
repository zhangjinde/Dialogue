#include <stdio.h>
#include "dialogue.h"
#include "envelope.h"
#include "mailbox.h"
#include "post.h"
#include "actor.h"
#include "script.h"
#include "utils.h"

/*
 * Add a script to the given actor, always at the front.
 */
void
actor_add_script (Actor *actor, Script *script)
{
    if (actor->script == NULL)
        goto set_actor_script;
    script->next = actor->script;
set_actor_script:
    script->actor = actor;
    actor->script = script;
}

/*
 * Add a child to the given actor, always at the front.
 */
void
actor_add_child (Actor *actor, Actor *child)
{
    if (actor->child == NULL)
        goto set_actor_child;
    child->next = actor->child;
set_actor_child:
    actor->child = child;
    child->parent = actor;
    child->dialogue = actor->dialogue;
    child->mailbox = actor->mailbox;
}

/*
 * Find and remove the Script from the Actor's linked-list of Scripts.
 */
void
actor_remove_script (Actor *actor, Script *removing)
{
    Script *now, *previous = NULL;

    if (actor->script == removing)
        actor->script = NULL;
    
    for (now = actor->script; now != NULL; previous = now, now = now->next) {
        if (now != removing)
            continue;

        previous->next = now->next;
        break;
    }
}

/*
 * Get the actor from the lua_State and craft an envelope.
 */
Envelope *
actor_envelope_create (lua_State *L, Tone tone, Actor *recipient)
{
    Actor* actor = lua_check_actor(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    return envelope_create(L, actor, tone, NULL);
}

/*
 * From an envelope, send a message to each Script an actor owns.
 */
void
actor_send_envelope (Actor *actor, Envelope *envelope)
{
    Script *script;
    pthread_mutex_lock(&actor->mutex);

    for (script = actor->script; script != NULL; script = script->next) {
        lua_method_push(actor->L, script, SCRIPT_LIB, "send");
        envelope_push_table(actor->L, envelope);
        if (lua_pcall(actor->L, 2, 0, 0))
            luaL_error(actor->L, "Error sending: %s", lua_tostring(actor->L, -1));
        lua_pop(actor->L, lua_gettop(actor->L));
    }

    pthread_mutex_unlock(&actor->mutex);
}

/*
 * Check for an Actor at index. Errors if it isn't an Actor.
 */
Actor *
lua_check_actor (lua_State *L, int index)
{
    return (Actor *) luaL_checkudata(L, index, ACTOR_LIB);
}

/*
 * Create an Actor, which is a glorified lua_State that holds specific scripts.
 * Actor{ {"draw", 400, 200}, {"weapon", "longsword"} };
 */
static int
lua_actor_new (lua_State *L)
{
    lua_State *A;
    Actor *actor;
    luaL_checktype(L, 1, LUA_TTABLE);  /* 1 */

    actor = lua_newuserdata(L, sizeof(Actor)); /* 2 */
    luaL_getmetatable(L, ACTOR_LIB);
    lua_setmetatable(L, -2);

    actor->parent = NULL;
    actor->next = NULL;
    actor->child = NULL;
    actor->script = NULL;
    actor->mutex = (pthread_mutex_t) PTHREAD_MUTEX_INITIALIZER;
    actor->L = luaL_newstate();
    A = actor->L;

    luaL_openlibs(A);

    /* load this module (the one you're reading) into the Actor's state */
    luaL_requiref(A, "Dialogue", luaopen_Dialogue, 1);
    lua_pop(A, 1);

    /* push Actor so Scripts can reference the Actor it belongs to. */
    utils_push_object(A, actor, ACTOR_LIB);
    lua_setglobal(A, "actor");

    return 1;
}

/*
 * Create a Script from a given table and 'give' it to the actor.
 * player:give{ "weapon", "gun" }
 */
static int
lua_actor_give (lua_State *L)
{
    Script *script = NULL;
    Actor* actor = lua_check_actor(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);

    lua_getglobal(actor->L, "Dialogue");
    lua_getfield(actor->L, -1, "Actor");
    lua_getfield(actor->L, -1, "Script");
    lua_getfield(actor->L, -1, "new");
    utils_copy_table(actor->L, L, 2);
    if (lua_pcall(actor->L, 1, 1, 0))
        luaL_error(L, "Giving script failed: %s", lua_tostring(actor->L, -1));

    script = lua_check_script(actor->L, -1);
    actor_add_script(actor, script);

    lua_getfield(actor->L, -1, "load");
    utils_push_object(actor->L, script, SCRIPT_LIB);
    if (lua_pcall(actor->L, 1, 0, 0))
        luaL_error(L, "Script failed to load: %s", lua_tostring(actor->L, -1));

    script->ref = luaL_ref(actor->L, LUA_REGISTRYINDEX);

    return 0;
}

static int
lua_actor_probe (lua_State *L)
{
    Actor *actor = lua_check_actor(L, 1);
    int ret = 0, args = lua_gettop(L);

    if (args == 2) {
        utils_copy_table(L, actor->L, 2);
        lua_setglobal(actor->L, "n");

        lua_getglobal(actor->L, "print");
        lua_getglobal(actor->L, "n");
        lua_call(actor->L, 1, 0);
    } else {
        lua_getglobal(actor->L, "n");
        utils_copy_table(actor->L, L, lua_gettop(actor->L));
        ret = 1;
    }

    return ret;
}

/*
 * Create actor from table and add it as a child. Returns the child created.
 * player:child{ {"draw", 2, 4}, { "weapon", "knife" } }
 */
static int
lua_actor_child (lua_State *L)
{
    int table_ref;
    Actor *child, *actor = lua_check_actor(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);

    table_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    /* Dialogue.Actor{ } */
    lua_getglobal(L, "Dialogue");
    lua_getfield(L, -1, "Actor");
    lua_rawgeti(L, LUA_REGISTRYINDEX, table_ref);
    if (lua_pcall(L, 1, 1, 0))
        luaL_error(L, "Creating child failed: %s", lua_tostring(L, -1));

    child = lua_check_actor(L, -1);
    actor_add_child(actor, child);

    return 1;
}

/*
 * Return an array of children an Actor owns.
 */
static int
lua_actor_children (lua_State *L)
{
    int i;
    Actor *child, *actor = lua_check_actor(L, 1);

    lua_newtable(L);

    for (i = 1, child = actor->child; child != NULL; child = child->next, i++) {
        utils_push_object(L, child, ACTOR_LIB);
        lua_rawseti(L, -2, i);
    }

    return 1;
}

/*
 * If no argument is given, return list of scripts. Else purge currently owned
 * scripts and create new ones from the given list of scripts.
 *
 * actor:scripts() => { script, ... }
 * actor:script{ {"weapon", "axe", "up"}, {"draw", 2, 4}, {"collision", 2, 4} }
 */
static int
lua_actor_scripts (lua_State *L)
{
    int i, args = lua_gettop(L);
    Script *s;
    Actor *actor = lua_check_actor(L, 1);

    if (args == 1)
        goto list;
    else if (args == 2)
        goto purge_and_create;

purge_and_create:
    luaL_checktype(L, 2, LUA_TTABLE);

    for (s = actor->script; s != NULL; s = s->next)
        luaL_unref(actor->L, LUA_REGISTRYINDEX, s->ref);

    actor->script = NULL;

    lua_pushnil(L);
    while (lua_next(L, 2)) {
        luaL_checktype(L, -1, LUA_TTABLE);
        lua_getfield(L, 1, "give");
        utils_push_object(L, actor, ACTOR_LIB);
        lua_pushvalue(L, -3);
        lua_call(L, 2, 0);
        lua_pop(L, 1);
    }

    return 0;

list:
    lua_newtable(L);

    for (i = 1, s = actor->script; s != NULL; s = s->next, i++) {
        utils_push_object(L, s, SCRIPT_LIB);
        lua_rawseti(L, -2, i);
    }

    return 1;
}

static int
lua_actor_think (lua_State *L)
{
    Actor* actor = lua_check_actor(L, 1);
    Envelope *envelope = actor_envelope_create(L, post_tone_think, NULL);
    mailbox_add(actor->mailbox, envelope);
    return 0;
}

static int
lua_actor_yell (lua_State *L)
{
    Actor* actor = lua_check_actor(L, 1);
    Envelope *envelope = actor_envelope_create(L, post_tone_yell, NULL);
    mailbox_add(actor->mailbox, envelope);
    return 0;
}

static int
lua_actor_tostring (lua_State *L)
{
    Actor* actor = lua_check_actor(L, 1);
    lua_pushfstring(L, "%s %p", ACTOR_LIB, actor);
    return 1;
}

static int
lua_actor_gc (lua_State *L)
{
    Actor* actor = lua_check_actor(L, 1);
    lua_close(actor->L);
    luaL_unref(L, LUA_REGISTRYINDEX, actor->ref);
    return 0;
}

static const luaL_Reg actor_methods[] = {
    {"child",      lua_actor_child},
    {"children",   lua_actor_children},
    {"give",       lua_actor_give},
    {"scripts",    lua_actor_scripts},
    {"send",       lua_actor_think},
    {"think",      lua_actor_think},
    {"yell",       lua_actor_yell},
    {"probe",      lua_actor_probe},
    {"__tostring", lua_actor_tostring},
    {"__gc",       lua_actor_gc},
    { NULL, NULL }
};

int 
luaopen_Dialogue_Actor (lua_State *L)
{
    return utils_lua_meta_open(L, ACTOR_LIB, actor_methods, lua_actor_new);
}
