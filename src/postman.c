#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include "postman.h"
#include "envelope.h"
#include "actor.h"
#include "utils.h"

void
postman_deliver (Postman *postman)
{
    Mailbox *mailbox = postman->mailbox;
    lua_State *B = mailbox->L;
    lua_State *P = postman->L;

    /* 
     * Wait for Mailbox access. We push the next Envelope and get the relevant
     * info and give up access as soon as possible. Other threads could be using
     * that access to deliver other envelopes!
     */

    int rc = pthread_mutex_lock(&mailbox->mutex);

    mailbox_push_next_envelope(mailbox);
    envelope_push_message(B, -1);
    utils_copy_top(P, B);
    lua_pop(B, 2);

    rc = pthread_mutex_unlock(&mailbox->mutex);

    /* print out the envelope from our stack */
    lua_pushnil(P);
    while (lua_next(P, 1)) {
        if (strcmp("amazing", lua_tostring(P, -1)) == 0)
            usleep(5000);

        printf("%s\n", lua_tostring(P, -1));
        lua_pop(P, 1);
    }
    lua_pop(P, 1);

    postman->needs_address = 0;
}

void *
postman_thread (void *arg)
{
    int rc;
    Postman *postman = arg;

    rc = pthread_mutex_lock(&postman->mutex);

    while (postman->delivering) {
        if (postman->needs_address) {
            postman_deliver(postman);
        } else {
            rc = pthread_cond_wait(&postman->get_address, &postman->mutex);
        }
    }

    return NULL;
}

/*
 * Create a postman which waits for the mailbox to tell it when to get a new
 * envelope and deliver it. Returns pointer if OK or NULL if not.
 */
Postman *
postman_new (Mailbox *mailbox)
{
    lua_State *P;
    pthread_mutexattr_t mutex_attr;

    Postman *postman = malloc(sizeof(Postman));

    if (postman == NULL)
        goto exit;

    postman->mailbox = mailbox;
    postman->delivering = 1;
    postman->needs_address = 0;
    postman->get_address = (pthread_cond_t) PTHREAD_COND_INITIALIZER;

    pthread_mutexattr_init(&mutex_attr);
    //pthread_mutexattr_settype(&mutex_attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&postman->mutex, &mutex_attr);

    postman->L = luaL_newstate();
    P = postman->L;
    luaL_openlibs(P);

    pthread_create(&postman->thread, NULL, postman_thread, postman);
    pthread_detach(postman->thread);

exit:
    return postman;
}

/*
 * Tell the postman to get an address (the next envelope) from the mailbox.
 * If the postman is busy (already delivering an envelope) then this returns 0.
 * If it wasn't busy it returns 1.
 */
int
postman_get_address (Postman *postman)
{
    int rc = pthread_mutex_trylock(&postman->mutex);

    if (rc != 0)
        goto busy;

    postman->needs_address = 1;

    rc = pthread_mutex_unlock(&postman->mutex);
    rc = pthread_cond_signal(&postman->get_address);

    return 1;

busy:
    return 0;
}

/*
 * Wait for the postman to get done delivering anything and then free him.
 */
void
postman_free (Postman *postman)
{
    int rc = pthread_mutex_lock(&postman->mutex);
    postman->delivering = 0;
    rc = pthread_mutex_unlock(&postman->mutex);

    lua_close(postman->L);
    free(postman);
}
