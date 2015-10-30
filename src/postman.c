#include <stdlib.h>
#include <unistd.h>
#include "mailbox.h"
#include "postman.h"
#include "actor.h"

void
postman_deliver (Postman *postman)
{
    lua_State *B = mailbox_request_stack(postman->mailbox);
    mailbox_return_stack(postman->mailbox);
}

void *
postman_thread (void *arg)
{
    Postman *postman = arg;

    while (postman->delivering)
        while (postman->has_address)
            postman_deliver(postman);

    return NULL;
}

Postman *
postman_create (Mailbox *box)
{
    Postman *postman = malloc(sizeof(Postman));

    postman->delivering = 1;
    postman->has_address = 0;
    postman->address = NULL;
    postman->mailbox = box;

    pthread_create(&postman->thread, NULL, postman_thread, postman);
    pthread_detach(postman->thread);

    return postman;
}

void
postman_give_address (Postman *postman, Actor *address)
{
    postman->address = address;
    postman->has_address = 1;
}

void
postman_free (Postman *postman)
{
    postman->has_address = 0;
    postman->delivering = 0;
    usleep(1000);
}
