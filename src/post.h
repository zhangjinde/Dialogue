#ifndef DIALOGUE_POST
#define DIALOGUE_POST

#define POST_LIB "Dialogue.Post"

#include "actor.h"
#include "mailbox.h"

/*
 * Every Postman has its own thread and Lua state. It delivers all the 
 * Envelopes in its state. It then waits and its state fills up with 
 * Envelopes and then it is told to process them.
 */
typedef struct Postman {
    lua_State *L;

    pthread_t thread;
    short int working;

    pthread_mutex_t lock;
    pthread_cond_t wait_cond;
    short int waiting;

    Actor *author;
    Actor *recipient;
    const char *tone;
} Postman;

/*
 * The Post is a collection of Postmen. It provides a way to quickly send
 * Envelopes to a waiting Postman.
 */
typedef struct Post {
    Postman **postmen;
    int postmen_count;
    int ref;
} Post;

Post *
lua_check_post (lua_State *L, int index);

/*
 * Expects a message on top of the given Lua state. The Post finds a free
 * Postman, copies the message from the Lua stack to the Postman's stack.  Then
 * it delivers it to the correct audience, given by the tone. Pops the message
 * on top.
 */
void
post_deliver_lua_top (lua_State *L, Post *post, Actor *author, const char *tone);

int 
luaopen_Dialogue_Post (lua_State *L);

#endif