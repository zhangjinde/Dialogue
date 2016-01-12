#include <pthread.h>
#include "actor.h"
#include "script.h"
#include "audience.h"
#include "utils.h"

typedef struct Actor {
    lua_State *L;

    /* 
     * A read write lock for the structure of the Actor -- it's place in the
     * Dialogue tree -- it's parent, siblings, and children.
     */
    pthread_rwlock_t structure;

    /* For everything else including the state & scripts */
    pthread_mutex_t state_mutex;

    /* For everything else including the state & scripts */
    pthread_mutex_t reference_mutex;

    int reference_count;

    /*
     * Restrict to single thread. If is_star is true, then restrict to main 
     * thread. If is_lead is true, then restrict to a Postman thread.
     */
    int is_lead;
    int is_star;

    /* Tree nav: go up, horizontally, and down the tree */
    struct Actor *parent;
    struct Actor *next;
    struct Actor *child;

    /* Go directly to the head of the tree */
    struct Actor *dialogue;

    struct Script *script_head;
    struct Script *script_tail;

    /* If a Mailbox is set, the Post will exclusively send envelopes to it */
    struct Mailbox *mailbox;

    /* A place for a parent to set their reference */
    int ref;
} Actor;


void
actor_request_structure (Actor *actor)
{
    pthread_rwlock_rdlock(&actor->structure);
}

void
actor_return_structure (Actor *actor)
{
    pthread_rwlock_unlock(&actor->structure);
}

lua_State *
actor_request_state (Actor *actor)
{
    pthread_mutex_lock(&actor->state_mutex);
    return actor->L;
}

void
actor_return_state (Actor *actor)
{
    pthread_mutex_unlock(&actor->state_mutex);
}

int
actor_is_calling_thread (pthread_t pid)
{
    return 1;
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
 * Leave the Lead Actor table on top of the stack. Creates it if it doesn't
 * exist. Will throw an error if not called on Main thread. Returns its
 * position on the stack.
 */
int
actor_lead_table (lua_State *L)
{
    static short int defined = 0;

    lua_getglobal(L, "__main_thread");

    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        luaL_error(L, "Lead Actor table only exists in Main thread!");
    } else {
        lua_pop(L, 1);
    }

    if (!defined) {
        lua_newtable(L);
        lua_setglobal(L, "__lead_actor");
        defined = 1;
    }

    lua_getglobal(L, "__lead_actor");
    return lua_gettop(L);
}

/*
 * Put the Actor inside the Lead actor table.
 */
void
actor_assign_lead (Actor *actor, lua_State *L)
{
    const int top = actor_lead_table(L);
    const int len = luaL_len(L, top);

    lua_rawgeti(L, LUA_REGISTRYINDEX, actor->ref);
    lua_rawseti(L, top, len + 1);

    lua_pop(L, 1);
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
 *
 * Optionally, one can call like Actor{ "Lead", {"draw", 0, 0} }, which creates
 * the Actor as a Lead Actor and loads all of its Scripts here.
 */
static int
lua_actor_new (lua_State *L)
{
    const int table_arg = 1;
    const int actor_arg = 2;
    int script_arg;
    int len = luaL_len(L, table_arg);
    int i, start = 1;
    lua_State *A;
    pthread_mutexattr_t mutex_attr;

    Actor *actor = lua_newuserdata(L, sizeof(*actor));
    luaL_getmetatable(L, ACTOR_LIB);
    lua_setmetatable(L, -2);

    actor->ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_rawgeti(L, LUA_REGISTRYINDEX, actor->ref);

    /* 
     * An actor's Action may call one of its own methods, which typically want
     * lock the mutex already locked by doing the action. So, they are 
     * recursive.
     */
    pthread_mutexattr_init(&mutex_attr);
    pthread_mutexattr_settype(&mutex_attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&actor->state_mutex, &mutex_attr);
    pthread_mutex_init(&actor->reference_mutex, &mutex_attr);
    pthread_rwlock_init(&actor->structure, NULL);

    actor->parent = NULL;
    actor->next = NULL;
    actor->child = NULL;
    actor->dialogue = NULL;
    actor->script_head = NULL;
    actor->script_tail = NULL;
    actor->mailbox = NULL;
    actor->reference_count = 0; /* no envelopes or outside structures ref */

    actor->is_lead = 0;
    actor->is_star = 0;

    /* 
     * If the first element is a string, parse it for 'Lead' or 'Star' and
     * then increment start to skip it when doing scripts later.
     */
    lua_rawgeti(L, table_arg, start);
    if (lua_type(L, 3) == LUA_TSTRING) {
        start++;
        switch (lua_tostring(L, -1)[0]) {
        case 'S':
            actor->is_star = 1;
            break;
        case 'L':
            actor->is_lead = 1;
            break;
        default:
            break;
        }
    }
    lua_pop(L, 1);

    actor->L = luaL_newstate();
    A = actor->L;
    luaL_openlibs(A);

    /* load this module (the one you're reading) into the Actor's state */
    luaL_requiref(A, "Dialogue", luaopen_Dialogue, 1);
    lua_getfield(A, -1, "Post");
    utils_push_object(A, lua_getpost(L), POST_LIB);
    lua_setfield(A, -2, "__obj");
    lua_pop(A, 2);

    /* push Actor so Scripts can reference the Actor it belongs to. */
    utils_push_object(A, actor, ACTOR_LIB);
    lua_setglobal(A, "actor");

    lua_getglobal(L, "Dialogue");
    lua_getfield(L, -1, "Actor");
    lua_getfield(L, -1, "Script");
    script_arg = lua_gettop(L);

    for (i = start; i <= len; i++) {
        lua_getfield(L, script_arg, "new");
        lua_pushvalue(L, actor_arg);
        lua_rawgeti(L, table_arg, i);
        lua_call(L, 2, 1);
        lua_pop(L, 1);
    }
    lua_pop(L, 2);
    lua_getfield(L, -1, "Post");
    lua_getfield(L, -1, "send");
    lua_pushvalue(L, actor_arg);
    lua_pushstring(L, "load");
    lua_call(L, 2, 0);
    lua_pop(L, 2);

    return 1;
}

static int
lua_actor_audience (lua_State *L)
{
    Actor *actor = lua_check_actor(L, 1);
    const char *tone = luaL_checkstring(L, 2);
    audience_filter_tone(L, actor, tone);
    return 1;
}

static int
lua_actor_scripts (lua_State *L)
{
    int i = 0;
    Script *script;
    Actor *actor = lua_check_actor(L, -1);

    actor_request_state(actor);
    lua_newtable(L);
    for (script = actor->script_head; script != NULL; script = script->next) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, script->ref);
        lua_rawseti(L, -2, ++i);
    }
    actor_return_state(actor);

    return 1;
}

static int
lua_actor_children (lua_State *L)
{
    int i = 0;
    Actor *child, *actor = lua_check_actor(L, -1);

    actor_request_structure(actor);
    lua_newtable(L);
    for (child = actor->child; child != NULL; child = child->next) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, child->ref);
        lua_rawseti(L, -2, ++i);
    }
    actor_return_structure(actor);

    return 1;
}

static int
lua_actor_gc (lua_State *L)
{
    lua_State *A;
    Script *script;
    Actor* actor = lua_check_actor(L, 1);

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

/*
 * Given a tone to filter an Actor's audience, send a given message to that
 * audience. Expects a prepended recipient if the tone is whisper.
 *
 * actor:think("move") => Post.send(actor, 'send', 'move', actor)
 * actor:whisper(graphic, "draw") => Post.send(graphics, 'send', 'draw', actor)
 * 
 */
int
actor_send (lua_State *L, const char *tone)
{
    const int actor_arg = 1;
    Actor *actor = lua_check_actor(L, actor_arg);
    int audience_table, audience_len;
    int arg_bottom = 2, arg_offset = 2;
    int arg_top = lua_gettop(L);
    int i, j;

    /* if true, the tone was a 'whisper' so we must supply the audience */
    if (audience_filter_tone(L, actor, tone)) {
        lua_pushvalue(L, arg_bottom);
        lua_rawseti(L, -2, 1);
        arg_bottom++;
        arg_offset--;
    }

    audience_table = lua_gettop(L);
    audience_len = luaL_len(L, -1);

    lua_getglobal(L, "Dialogue");
    lua_getfield(L, -1, "Post");

    /*
     * Send each audience member a message in the form of
     *  Post.send(recipient, 'send' [, arg1 [, ... [, argn]]], author)
     */
    for (i = 1; i <= audience_len; i++) {
        lua_getfield(L, -1, "send");
        lua_rawgeti(L, audience_table, i);
        lua_pushstring(L, "send");
        
        /* We can use the top of the stack to determine # of args passed to
         * 'send'. 'whisper' always has an extra argument, so we decrease the
         * arg offset by one. All other tones always have at least 2.
         *
         * actor:whisper(graphic, "draw", 2, 2) : top 5
         * => Post.send(graphics, 'send', 'draw', 2, 2, actor) top + 1 = 6 args
         *
         * actor:think("move") : top 2
         * => Post.send(actor, 'send', 'move', actor) top + 2 = 6 args
         *
         * actor:think("move", 2, 2) : top 4
         * => Post.send(actor, 'send', 'move', 2, 2, actor) top + 2 = 6 args
         */
        for (j = arg_bottom; j <= arg_top; j++)
            lua_pushvalue(L, j);
        lua_pushvalue(L, actor_arg);
        lua_call(L, arg_top + arg_offset, 0);
    }
    lua_pop(L, 3);

    return 0;
}

static int
lua_actor_think (lua_State *L)
{
    return actor_send(L, "think");
}

static int
lua_actor_whisper (lua_State *L)
{
    return actor_send(L, "whisper");
}

static int
lua_actor_say (lua_State *L)
{
    return actor_send(L, "say");
}

static int
lua_actor_command (lua_State *L)
{
    return actor_send(L, "command");
}

static int
lua_actor_yell (lua_State *L)
{
    return actor_send(L, "yell");
}

static const luaL_Reg actor_methods[] = {
    {"audience",   lua_actor_audience},
    {"scripts",    lua_actor_scripts},
    {"children",   lua_actor_children},
    {"think",      lua_actor_think},
    {"whisper",    lua_actor_whisper},
    {"say",        lua_actor_say},
    {"command",    lua_actor_command},
    {"yell",       lua_actor_yell},
    {"__gc",       lua_actor_gc},
    {"__tostring", lua_actor_tostring},
    { NULL, NULL }
};

int 
luaopen_Dialogue_Actor (lua_State *L)
{
    lua_pushstring(L, "Foo");
    return 1;
}
