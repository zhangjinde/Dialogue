#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include "worker.h"
#include "mailbox.h"

struct Worker {
    lua_State *L;
    pthread_t thread;
    pthread_mutex_t mutex;
    Mailbox *mailbox;
    int processed;
    int working;
    int ref;
};

/*
 * Actually do the actions of our Dialogue.
 */
void *
worker_thread (void *arg)
{
    const char *error;
    const int dialogue_table = 1;
    int i, len, args, top;
    Worker *worker = arg;
    lua_State *W = worker->L;

    /* push the initial two tables onto the stack */
    lua_getglobal(W, "Dialogue");

    while (worker->working) {
        pthread_mutex_lock(&worker->mutex);

        if (mailbox_pop_all(W, worker->mailbox) == 0)
            goto unlock;
        
        for (top = lua_gettop(W); top > dialogue_table; top--) {
            len  = luaL_len(W, top);
            args = len - 1;

            /* push the action type to see if it exists */
            lua_rawgeti(W, top, 1);
            lua_gettable(W, dialogue_table);

            if (!lua_isfunction(W, -1)) {
                lua_rawgeti(W, top, 1);
                error = lua_tostring(W, -1);
                if (error == NULL)
                    printf("error is null\n");
                printf("`%s' is not an Action recognized by Dialogue!\n", error);
                lua_pop(W, 2); /* action type and the `function' */
                goto next;
            }

            /* push the rest of the table as arguments */
            for (i = 2; i <= len; i++)
                lua_rawgeti(W, top, i);

            /*
             * Returns a table of actions to resend and a boolean to determine if
             * the messages should be resent through the Director or if this Worker
             * should just redo it
             *
             * 0 - Normal case, can pus nil for table
             * 1 - Resend the current message
             * 2 - Resend the list of messages
             * 3 - Redo - push the list onto the Worker stack and loop
             */
            if (lua_pcall(W, args, 0, 0)) {
                error = lua_tostring(W, -1);
                printf("%s\n", error);
                lua_pop(W, 1); /* error string */
            }
next:
            worker->processed++;
            lua_pop(W, 1); /* the top action */
        }
unlock:
        pthread_mutex_unlock(&worker->mutex);
    }

    if (top > 1)
        printf("%p quit with %d left\n", worker, top);

    return NULL;
}

Worker *
worker_start (lua_State *L)
{
    /* TODO: check memory here */
    Worker *worker = malloc(sizeof(*worker));
    worker->L = lua_newthread(L);
    /* ref (which pops) the thread object so we control garbage collection */
    worker->ref = luaL_ref(L, LUA_REGISTRYINDEX);
    worker->mailbox = mailbox_create(L);
    worker->working = 1;
    worker->processed = 0;
    pthread_mutex_init(&worker->mutex, NULL);
    pthread_create(&worker->thread, NULL, worker_thread, worker);

    return worker;
}

/*
 * Call `mailbox_push_top' for the Worker's mailbox.
 */
int
worker_take_action (lua_State *L, Worker *worker)
{
    return mailbox_push_top(L, worker->mailbox);
}

/*
 * Wait for the Worker to wait for work, then join it back to the main thread.
 * Frees the worker and releases its reference.
 */
void
worker_stop (lua_State *L, Worker *worker)
{
    pthread_mutex_lock(&worker->mutex);
    worker->working = 0;
    pthread_mutex_unlock(&worker->mutex);
    pthread_join(worker->thread, NULL);
    printf("%p processed %d\n", worker, worker->processed);
    mailbox_destroy(L, worker->mailbox);
    free(worker);
}
