#include <stdio.h>
#include "dialogue.h"
#include "actor.h"
#include "mailbox.h"
#include "script.h"
#include "tone.h"
#include "utils.h"

/*
 * Check for an Actor at index. Errors if it isn't an Actor.
 */
Actor *
lua_check_actor (lua_State *L, int index)
{
    return (Actor *) luaL_checkudata(L, index, ACTOR_LIB);
}

/*
 * Returns 1 if an Actor is part of Dialogue and 0 if not.
 */
int
actor_is_dialogue (Actor *actor)
{
    return (actor->dialogue != NULL);
}

/*
 * Add a child to the end of the Actor's linked-list of children.
 */
void
actor_add_child (Actor *actor, Actor *child)
{
    Actor *sibling;

    if (actor->child == NULL) {
        actor->child = child;
        goto set_actor_child;
    }

    for (sibling = actor->child; child != NULL; sibling = sibling->next) {
        if (sibling->next == NULL) {
            sibling->next = child;
            goto set_actor_child;
        }
    }

set_actor_child:
    child->parent = actor;
    child->dialogue = actor->dialogue;
}

/*
 * Add the Script to the end of the Actor's linked-list of Scripts.
 */
void
actor_add_script (Actor *actor, Script *script)
{
    if (actor->script_head == NULL) {
        actor->script_head = script;
        actor->script_tail = script;
    } else {
        actor->script_tail->next = script;
        script->prev = actor->script_tail;
        actor->script_tail = script;
    }
}

/*
 * Remove the Script from the Actor's linked-list.
 */
void
actor_remove_script (Actor *actor, Script *script)
{
    if (script->prev == NULL && script->next) {
        /* it is the head */
        actor->script_head = script->next;
        actor->script_head->prev = NULL;
    } else if (script->next == NULL && script->prev) {
        /* it is the tail */
        actor->script_tail = script->prev;
        actor->script_tail->next = NULL;
    } else {
        /* a normal node */
        script->prev->next = script->next;
        script->next->prev = script->prev;
    }
}

/*
 * Create an Actor which has its own thread. It initializes all Scripts and 
 * handles all messages (send/receive) in its own thread because many Lua 
 * modules and objects aren't thread-safe.
 *
 * Create the Actor by sending a table of tables of the Lua module to load and
 * any variables needed to initialize it.
 *
 * Actor{ {"draw", 400, 200}, {"weapon", "longsword"} }
 */
static int
lua_actor_new (lua_State *L)
{
    const int table_arg = 1;
    const int actor_arg = 2;

    int is_manual_call = 0;
    int table_index;
    int script_index;
    lua_State *A;
    Actor *actor;
    Script *script;
    pthread_mutexattr_t mutex_attr;

    luaL_checktype(L, table_arg, LUA_TTABLE);

    /* An optional bool can be passed to make this Actor be called manually */
    if (lua_gettop(L) == 2) {
        luaL_checktype(L, 2, LUA_TBOOLEAN);
        is_manual_call = lua_toboolean(L, -1);
        lua_pop(L, 1);
    }

    actor = lua_newuserdata(L, sizeof(*actor));
    luaL_getmetatable(L, ACTOR_LIB);
    lua_setmetatable(L, -2);

    actor->parent = NULL;
    actor->next = NULL;
    actor->child = NULL;
    actor->script_head = NULL;
    actor->script_tail = NULL;
    actor->mailbox = NULL;
    actor->dialogue = NULL;

    actor->new_action = (pthread_cond_t) PTHREAD_COND_INITIALIZER;
    actor->action = LOAD;
    actor->on = 1;
    actor->manual_call = is_manual_call;

    /* 
     * init mutexes to recursive because its own thread might call a method
     * which expects to be called from an outside thread sometimes and askes
     * for a mutex.
     */
    pthread_mutexattr_init(&mutex_attr);
    pthread_mutexattr_settype(&mutex_attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&actor->state_mutex, &mutex_attr);

    actor->L = luaL_newstate();
    A = actor->L;
    luaL_openlibs(A);

    /* load this module (the one you're reading) into the Actor's state */
    luaL_requiref(A, "Dialogue", luaopen_Dialogue, 1);
    lua_pop(A, 1);

    /* push Actor so Scripts can reference the Actor it belongs to. */
    utils_push_object(A, actor, ACTOR_LIB);
    lua_setglobal(A, "actor");

    lua_getglobal(L, "Dialogue");
    lua_getfield(L, -1, "Actor");

    lua_getfield(L, -1, "Mailbox");
    lua_getfield(L, -1, "new");
    lua_pushvalue(L, actor_arg);
    if (lua_pcall(L, 1, 1, 0))
        luaL_error(L, "Creating Mailbox failed: %s", lua_tostring(L, -1));

    actor->mailbox = lua_check_mailbox(L, -1);
    actor->mailbox->ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_pop(L, 1);

    lua_getfield(L, -1, "Script");
    script_index = lua_gettop(L);

    /* Create all the Scripts in this Lua state */
    lua_pushnil(L);
    while (lua_next(L, table_arg)) {
        luaL_checktype(L, -1, LUA_TTABLE);
        table_index = lua_gettop(L);

        lua_getfield(L, script_index, "new");
        lua_pushvalue(L, actor_arg);
        lua_pushvalue(L, table_index);

        if (lua_pcall(L, 2, 1, 0))
            luaL_error(L, "Creating Script failed: %s", lua_tostring(L, -1));

        script = lua_check_script(L, -1);
        script->ref = luaL_ref(L, LUA_REGISTRYINDEX);
        actor_add_script(actor, script);

        lua_pop(L, 1); /* Key */
    }
    lua_pop(L, 3); /* Dialogue, Actor, Script */

    if (!is_manual_call) {
        pthread_create(&actor->thread, NULL, actor_thread, actor);
        pthread_detach(actor->thread);
    }

    return 1;
}

/*
 * This is a blocking method. It puts the the given message inside the 
 * Envelopes table and tells the Actor to process it.
 */
static int
lua_actor_send (lua_State *L)
{
    Actor *actor = lua_check_actor(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);

    mailbox_send(actor->mailbox, L);
    actor_alert_action(actor, RECEIVE);
    
    return 0;
}

/*
 * This is a blocking method. Returns an array of Script references of an actor.
 */
static int
lua_actor_scripts (lua_State *L)
{
    int i = 0;
    Script *script;
    Actor *actor = lua_check_actor(L, 1);

    actor_request_state(actor);
    lua_newtable(L);
    for (script = actor->script_head; script != NULL; script = script->next) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, script->ref);
        lua_rawseti(L, -2, ++i);
    }
    actor_return_state(actor);

    return 1;
}

/*
 * Reload all Scripts. If the Actor is a Lead Actor the load happens on this
 * call. Otherwise is it an asynchronous operation.
 */
int
lua_actor_load (lua_State *L)
{
    int manual_call;
    Script *script;
    Actor *actor = lua_check_actor(L, 1);

    actor_request_state(actor);

    for (script = actor->script_head; script != NULL; script = script->next) {
        utils_push_objref_method (L, script->ref, "load");
        lua_call(L, 1, 0);
    }

    manual_call = actor->manual_call;
    actor_return_state(actor);

    /* 
     * If this was a manual call, we make the LOAD action happen here, now.
     * Otherwise, all the scripts have told the Actor to load them.
     */
    if (manual_call)
        actor_call_action(actor, LOAD);

    return 0;
}

/*
 * Allows a Lead Actor to process all the messages it its Mailbox.
 */
int
lua_actor_receive (lua_State *L)
{
    Actor *actor = lua_check_actor(L, 1);
    
    actor_request_state(actor);
    if (!actor->manual_call) {
        actor_return_state(actor);
        luaL_error(L, "%s %p is not a lead actor!", ACTOR_LIB, actor);
    }
    actor_return_state(actor);

    actor_call_action(actor, RECEIVE);

    return 0;
}

/*
 * Make an Actor a Lead actor. This closes its thread and adds the 'receive' 
 * method. With no thread, this Actor calls 'receive' to process its Mailbox.
 *
 * By shutting down the thread gracefully and making some of these concessions,
 * we now have an Actor which can be called from the Main thread synchronously.
 */
int
lua_actor_lead (lua_State *L)
{
    Actor *actor = lua_check_actor(L, 1);

    actor_alert_action(actor, STOP);

    usleep(1000);

    actor_request_state(actor);
    actor->manual_call = 1;
    actor_return_state(actor);

    utils_add_method(L, 1, lua_actor_receive, "receive");

    return 0;
}

/*
 * Return a list of Actors which are the audience filtered by the tone given as
 * a string.
 * actor:audience("say") => { actor, actor, ... }
 * actor:audience("command") => { child, child, ... }
 */
static int
lua_actor_audience (lua_State *L)
{
    Actor *actor = lua_check_actor(L, 1);
    const char *tone = luaL_checkstring(L, 2);

    if (!actor_is_dialogue(actor))
        luaL_error(L, "Actor must be part of a Dialogue!");

    audience_filter_tone(L, actor, tone);

    return 1;
}

/*
 * Return an array of children an Actor owns.
 */
static int
lua_actor_children (lua_State *L)
{
    int i = 0;
    Actor *child, *actor = lua_check_actor(L, 1);

    actor_request_state(actor);
    lua_newtable(L);
    for (child = actor->child; child != NULL; child = child->next) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, child->ref);
        lua_rawseti(L, -2, ++i);
    }
    actor_return_state(actor);

    return 1;
}

/*
 * Set the thread's condition to false and close the Lua stack.
 */
static int
lua_actor_gc (lua_State *L)
{
    lua_State *A;
    Script *script;
    Actor *actor = lua_check_actor(L, 1);

    actor_alert_action(actor, STOP);
    usleep(1000);

    A = actor_request_state(actor);
    for (script = actor->script_head; script != NULL; script = script->next)
        script_unload(script);
    lua_close(A);
    actor_return_state(actor);

    return 0;
}

static int
lua_actor_tostring (lua_State *L)
{
    Actor* actor = lua_check_actor(L, 1);
    lua_pushfstring(L, "%s %p", ACTOR_LIB, actor);
    return 1;
}

static const luaL_Reg actor_methods[] = {
    {"audience",   lua_actor_audience},
    {"children",   lua_actor_children},
    {"send",       lua_actor_send},
    {"lead",       lua_actor_lead},
    {"scripts",    lua_actor_scripts},
    {"load",       lua_actor_load},
    {"__gc",       lua_actor_gc},
    {"__tostring", lua_actor_tostring},
    { NULL, NULL }
};

int 
luaopen_Dialogue_Actor (lua_State *L)
{
    utils_lua_meta_open(L, ACTOR_LIB, actor_methods, lua_actor_new);
    
    luaL_requiref(L, SCRIPT_LIB, luaopen_Dialogue_Actor_Script, 1);
    lua_setfield(L, -2, "Script");

    luaL_requiref(L, MAILBOX_LIB, luaopen_Dialogue_Actor_Mailbox, 1);
    lua_setfield(L, -2, "Mailbox");

    return 1;
}
