#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include "tree.h"

#define NODE_FAMILY_MAX 4

enum NodeType {
    NODE_ERROR = -2,
    NODE_INVALID = -1
};

enum NodeFamily { 
    NODE_PARENT, 
    NODE_NEXT_SIBLING, 
    NODE_PREV_SIBLING, 
    NODE_CHILD 
};

typedef struct Node {
    void *data;
    int attached;
    int benched;
    int family[NODE_FAMILY_MAX];
    pthread_rwlock_t rw_lock;
    pthread_rwlock_t ref_lock;
    int ref_count;
} Node;

typedef struct Tree {
    pthread_rwlock_t rw_lock;
    int list_size;
    int list_max_size;
    int list_resize_factor;
    int root;
    data_cleanup_func_t cleanup_func;
    data_lookup_func_t lookup_func;
    data_set_id_func_t set_id_func;
    Node *list;
} Tree;

static Tree *global_tree = NULL;

int
tree_read ()
{
    return pthread_rwlock_rdlock(&global_tree->rw_lock);
}

int
tree_write ()
{
    return pthread_rwlock_wrlock(&global_tree->rw_lock);
}

int
tree_unlock ()
{
    return pthread_rwlock_unlock(&global_tree->rw_lock);
}

int
tree_root ()
{
    int id = NODE_INVALID;
    if (tree_read() == 0) {
        id = global_tree->root;
        tree_unlock();
    }
    return id;
}

int
tree_list_size ()
{
    int ret = 0;
    if (tree_read() == 0) {
        ret = global_tree->list_size;
        tree_unlock();
    }
    return ret;
}

/*
 * Return 1 (true) or 0 (false) if the id is a valid index or not.
 *
 * Since we guarantee that the set of the valid indices always grows and never
 * shrinks, all we need to do is check that `id' is >= 0 and <= max-index and
 * the id will *always* be valid.
 */
int
tree_index_is_valid (int id)
{
    int ret = 0;

    if (id < 0)
        goto exit;

    if (tree_read() != 0)
        goto exit;

    ret = !(id > global_tree->list_size);
    tree_unlock();

exit:
    return ret;
}

int
node_ref_count (int id)
{
    int count = -1;

    if (pthread_rwlock_rdlock(&global_tree->list[id].ref_lock) != 0)
        goto exit;

    count = global_tree->list[id].ref_count;
    pthread_rwlock_unlock(&global_tree->list[id].ref_lock);;

exit:
    return count;
}

int 
node_write (int id)
{
    if (!tree_index_is_valid(id))
        return 1;
    return pthread_rwlock_wrlock(&global_tree->list[id].rw_lock);
}

int 
node_read (int id)
{
    if (!tree_index_is_valid(id))
        return 1;
    return pthread_rwlock_rdlock(&global_tree->list[id].rw_lock);
}

int 
node_unlock (int id)
{
    return pthread_rwlock_unlock(&global_tree->list[id].rw_lock);
}

/*
 * With read or write lock:
 * Returns 1 (true) or 0 (false) if the node is being used or not.
 */
int
node_is_used_rd (int id)
{
    return (global_tree->list[id].attached || global_tree->list[id].benched);
}

/*
 * With a write lock:
 * Attach (and unbench) the node to the tree system and give it a reference to
 * hold.
 */
void
node_mark_attached_wr (int id, void *data)
{
    global_tree->list[id].attached = 1;
    global_tree->list[id].benched = 0;
    global_tree->list[id].data = data;
    global_tree->set_id_func(data, id);
}

/*
 * With a write lock:
 * Mark a node as benched. This keeps its reference id valid and its data from
 * being cleaned-up even if it removed from the tree.
 */
void
node_mark_benched_wr (int id)
{
    global_tree->list[id].attached = 0;
    global_tree->list[id].benched = 1;
}

/*
 * With a write lock:
 * Mark a node unusued, telling to system it is free to be cleaned-up.
 */
void
node_mark_unused_wr (int id)
{
    global_tree->list[id].attached = 0;
    global_tree->list[id].benched = 0;
}

/*
 * With a write lock:
 * Initialize the node at the id. Typically is only called when the tree is
 * initially created and everytime a realloc occurs producing unitialized
 * pointers.
 */
void
node_init_wr (int id)
{
    int i;
    node_mark_unused_wr(id);

    pthread_rwlock_init(&global_tree->list[id].rw_lock, NULL);
    pthread_rwlock_init(&global_tree->list[id].ref_lock, NULL);

    global_tree->list[id].data = NULL;
    global_tree->list[id].ref_count = 0;

    for (i = 0; i < NODE_FAMILY_MAX; i++)
        global_tree->list[id].family[i] = NODE_INVALID;
}

/*
 * With the write lock on id:
 *
 * Acquire the write lock on the parent. Add the child to that parent. If the
 * parent already has children, append the child to the end of the sibling
 * list.  
 *
 * Returns 0 if successful, 1 if parent_id is invalid.
 */
int
node_add_parent_wr (int id, int parent_id)
{
    int sibling, next = 0, ret = 1;

    /*
     * Lock parent for the duration of the function because we can't assume 
     * that the parent won't get disabled if we free its lock.
     */
    if (node_write(parent_id) != 0)
        goto exit;

    global_tree->list[id].family[NODE_PARENT] = parent_id;
    sibling = global_tree->list[parent_id].family[NODE_CHILD];

    /* if the parent has no children (first child not set) */
    if (sibling == NODE_INVALID) {
        global_tree->list[parent_id].family[NODE_CHILD] = id;
    } else {
    /* else there are children, so find the last sibling and append new child */
        while (sibling >= 0) {
            node_read(sibling);
            next = global_tree->list[sibling].family[NODE_NEXT_SIBLING];
            node_unlock(sibling);
            if (next == NODE_INVALID)
                break;
            sibling = next;
        }

        node_write(sibling);
        global_tree->list[sibling].family[NODE_NEXT_SIBLING] = id;
        node_unlock(sibling);
    }

    ret = 0;
    node_unlock(parent_id);
exit:
    return ret;
}

/*
 * Acquire the read lock first and make sure the node is marked for cleanup. If
 * the node is being used, has any references, exit with 1 for "unable to
 * cleanup".
 *
 * Otherwise acquire the write lock and cleanup the node according to the
 * cleanup_func which was given at tree_init and return 0 for "cleaned up".
 *
 * If the node has already been cleaned up exit with 0.
 */
int
node_cleanup (int id)
{
    int i, ret = 1;

    if (node_read(id) != 0)
        goto exit;
    
    if (node_is_used_rd(id))
        goto unlock;

    if (node_ref_count(id) > 0)
        goto unlock;

    if (global_tree->list[id].data == NULL)
        goto clean_exit;

    node_unlock(id);
    node_write(id);

    printf("Node %d\n", id);
    printf("  data -> %p\n", global_tree->list[id].data);
    printf("  [fc] -> %d\n", global_tree->list[id].family[NODE_CHILD]);
    printf("  [nc] -> %d\n", global_tree->list[id].family[NODE_NEXT_SIBLING]);
    printf("  [pa] -> %d\n", global_tree->list[id].family[NODE_PARENT]);

    /* even tho we checked it above, we can't rely on it because we unlocked */
    if (global_tree->list[id].data) {
        global_tree->cleanup_func(global_tree->list[id].data);
        global_tree->list[id].data = NULL;

        for (i = 0; i < NODE_FAMILY_MAX; i++)
            global_tree->list[id].family[i] = NODE_INVALID;
    }

clean_exit:
    ret = 0;
unlock:
    node_unlock(id);
exit:
    return ret;
}

/*
 * Acquires the write-lock for the tree. Resizes the list by the factor given
 * at tree_init-- 2 doubles its size, 3 triples, etc. Returns 0 if successful.
 */
int
tree_resize ()
{
    Node *memory = NULL;
    int id, size, ret = 1;

    tree_write();

    if (global_tree->list_size >= global_tree->list_max_size)
        goto unlock;

    id = global_tree->list_size;
    size = global_tree->list_size * global_tree->list_resize_factor;

    /* ceiling the size as the factor may cause it to overflow */
    if (size > global_tree->list_max_size)
        size = global_tree->list_max_size;

    memory = realloc(global_tree->list, size * sizeof(Node));

    if (memory != NULL) {
        ret = 0;
        global_tree->list = memory;
        global_tree->list_size = size;
        for (; id < global_tree->list_size; id++)
            node_init_wr(id);
    }

unlock:
    tree_unlock();
    return ret;
}

/*
 * Have the tree take ownship of the pointer. The tree will cleanup that
 * pointer with the data_cleanup_func_t given in tree_init. 
 *
 * The tree attaches the pointer to a Node which is added as a child of
 * parent_id.  If parent_id <= NODE_INVALID then the Tree assumes that is
 * supposed to be the root Node and saves it (the root node has no parent).
 *
 * Returns the id of the Node inside the tree. 
 *
 * Returns NODE_INVALID if the tree was unable to allocate more memory for
 * Nodes to hold the reference.
 *
 * Returns NODE_ERROR 
 *      * if data is NULL
 *      * if parent_id > -1 *and* the parent_id isn't in use (a valid, 
 *        non-garbage reference).
 *      * write-lock fails while setting the root node
 */
int
tree_add_reference (void *data, int parent_id)
{
    int max_id, set_root = 0, id = NODE_ERROR;

    if (data == NULL)
        goto exit;

    if (parent_id > NODE_INVALID) {
        if (node_read(parent_id) != 0)
            goto exit;

        if (!node_is_used_rd(parent_id)) {
            node_unlock(parent_id);
            goto exit;
        }
        node_unlock(parent_id);
    } else {
        set_root = 1;
    }

find_unused_node:
    max_id = tree_list_size();

    /* find first unused node and clean it up if needed */
    for (id = 0; id < max_id; id++)
        if (node_cleanup(id) == 0)
            goto write;

    /* if we're here, no unused node was found */
    if (tree_resize() == 0) {
        goto find_unused_node;
    } else {
        id = NODE_INVALID;
        goto exit;
    }

write:
    node_write(id);

    /*
     * The id we found could theoretically be `found' by another thread, so
     * after acquiring the write-lock on it, we double-check it is unused or we
     * loop back to find another unused one.
     */
    if (node_is_used_rd(id)) {
        node_unlock(id);
        goto find_unused_node;
    }

    if (set_root) {
        if (tree_write() != 0) {
            id = NODE_ERROR;
            goto exit;
        }
        global_tree->root = id;
        tree_unlock();
    }

    node_mark_attached_wr(id, data);
    node_add_parent_wr(id, parent_id);
    node_unlock(id);

exit:
    return id;
}

/*
 * Call the write capable function recursively over the tree given. Any node is
 * potentially a sub-tree. Calling this on the root of the entire tree will call
 * the function for each node of the tree.
 */
void
tree_write_map (int root, void (*write_capable_function) (int))
{
    int next, child;

    if (node_write(root) != 0)
        return;

    child = global_tree->list[root].family[NODE_CHILD];
    next = global_tree->list[root].family[NODE_NEXT_SIBLING];

    write_capable_function(root);
    node_unlock(root);

    if (child > NODE_INVALID)
        tree_write_map(child, write_capable_function);

    if (next > NODE_INVALID)
        tree_write_map(next, write_capable_function);
}

/*
 * Unlink the reference Node (by id) and all of its descendents in the tree.
 *
 * This function doesn't cleanup the reference data (given in
 * tree_add_reference) for any of the nodes unlinked. 
 *
 * If `is_delete' is true, then the nodes will be marked for cleanup (which
 * happens in tree_add_reference) and the reference id will be invalid (should
 * be discarded).
 *
 * If `is_delete' is false, the nodes aren't marked for cleanup and the
 * reference id will still be valid meaning the node is unlinked from the tree
 * but still exists. This may be used for temporarily removing a reference from
 * the tree and then adding it back.
 *
 * Returns 0 if successful, 1 if the id is invalid.
 */
int
tree_unlink_reference (int id, int is_delete)
{
    int ret = 1;
    int prev, next, parent, child, family, is_first_child;
    void (*unlink_func)(int);

    if (node_write(id) != 0)
        goto exit;

    if (is_delete)
        unlink_func = node_mark_unused_wr;
    else
        unlink_func = node_mark_benched_wr;

    parent = global_tree->list[id].family[NODE_PARENT];
    prev = global_tree->list[id].family[NODE_PREV_SIBLING];
    next = global_tree->list[id].family[NODE_NEXT_SIBLING];
    child = global_tree->list[id].family[NODE_CHILD];
    is_first_child = !(prev >= 0);

    unlink_func(id);

    /* map the unlink_func to the sub-`tree` of children */
    if (child > NODE_INVALID)
        tree_write_map(child, unlink_func);

    /* 
     * So we can keep being DRY, The first child's prev `pointer' (which is -1)
     * ends up being the parent if you think of the parent as the head of a
     * doubly linked list. 
     */
    if (is_first_child) {
        prev = parent;
        family = NODE_CHILD;
    } else {
        family = NODE_NEXT_SIBLING;
    }

    /*
     *      P
     *      |
     *      V
     *   <- A <=> B <=> C ->
     *
     * Example: we need to remove B while some other part of the program is
     * reading down the list of P's children.
     *
     * If we don't lock A and B at the same time, we risk a read on A getting
     * B's id, then a read on B. Since we *must* have a write lock on B, that
     * read on B blocks and waits on what will become an invalid Node. When B 
     * is done blocking, it is invalid and it won't be able to point to C.  For
     * a period of time, all of P's children past B are unreachable.
     *
     * Locking both A and B at the same time prevents this from happening. Same
     * applies to C and B (for the reverse).
     */

    if (node_write(prev) == 0) {
        global_tree->list[prev].family[family] = next;
        node_unlock(prev);
    }

    if (node_write(next) == 0) {
        if (is_first_child)
            global_tree->list[next].family[NODE_PREV_SIBLING] = NODE_INVALID;
        else
            global_tree->list[next].family[NODE_PREV_SIBLING] = prev;
        node_unlock(prev);
    }

    ret = 0;
    node_unlock(id);
exit:
    return ret;
}

/*
 * Initialze the tree.
 *
 * The tree's size will start as `length`. It will never exceed `max_length`.
 * It will resize itself by `scale_factor` (e.g. 2 doubles, 3 triples, etc).
 *
 * `cu` is the cleanup function called on all data given for reference. `lu` is
 * the lookup function on the data. 
 *
 * and the cleanup function for the
 * references (pointers it owns) it holds. 
 *
 * The `initial_length' also serves as
 * the base for resizing by a factor. So, if the length is 10, it is resized by
 * factors of 10.
 *
 * Returns 0 if no errors.
 */
int
tree_init (
        int length, 
        int max_length, 
        int scale_factor, 
        data_set_id_func_t set_id,
        data_cleanup_func_t cleanup,
        data_lookup_func_t lookup)
{
    int id, ret = 1;

    global_tree = malloc(sizeof(*global_tree));
    if (!global_tree)
        goto exit;

    global_tree->list = malloc(sizeof(Node) * length);
    if (!global_tree->list)
        goto exit;

    global_tree->cleanup_func = cleanup;
    global_tree->lookup_func = lookup;
    global_tree->set_id_func = set_id;
    global_tree->list_size = length;
    global_tree->list_max_size = max_length;
    global_tree->list_resize_factor = scale_factor;
    global_tree->root = NODE_INVALID;
    pthread_rwlock_init(&global_tree->rw_lock, NULL);

    for (id = 0; id < length; id++)
        node_init_wr(id);

    ret = 0;
exit:
    return ret;
}

/*
 * Mark all active Nodes as garbage and clean them up. Then free the memory for
 * the Tree and the list of Nodes.
 */
void
tree_cleanup ()
{
    int id, max_id = tree_list_size();

    /* The cleanup requires the Nodes to be garbage first */
    tree_unlink_reference(tree_root(), 1);

    for (id = 0; id < max_id; id++)
        node_cleanup(id);

    free(global_tree->list);
    free(global_tree);
}

/*
 * Get the pointer associated with the reference id. This function doesn't pass
 * ownership. 
 *
 * Returns NULL if the given id is bad either by having an invalid index or by
 * pointing to garbage data.
 *
 * Increments the ref_count for the Node at id. See node_cleanup.
 */
void *
tree_dereference (int id)
{
    void *ptr = NULL;

    if (node_read(id) != 0)
        goto exit;

    if (!node_is_used_rd(id))
        goto unlock;
    
    ptr = global_tree->list[id].data;

    pthread_rwlock_wrlock(&global_tree->list[id].ref_lock);
    global_tree->list[id].ref_count++;
    pthread_rwlock_unlock(&global_tree->list[id].ref_lock);

unlock:
    node_unlock(id);
exit:
    return ptr;
}

/*
 * Using the lookup_func, get the id for the Node from the data. Errors should
 * be handled through the lookup function.
 *
 * Decrements the ref_count for the Node at id. See node_cleanup.
 */
int
tree_reference (void *data)
{
    int id = global_tree->lookup_func(data);

    pthread_rwlock_wrlock(&global_tree->list[id].ref_lock);
    global_tree->list[id].ref_count--;
    pthread_rwlock_unlock(&global_tree->list[id].ref_lock);

    return id;
}

/*
 * Make, Lookup, Set Id, and Remove. Allocate some data and free it. Used to test we
 * don't have memory leaks.
 */

void* mk (int id) {
    return malloc(sizeof(int));
}

void set (void *data, int id) {
    *((int*)data) = id;
}

int lk (void *data) {
    return *((int*)data);
}

void rm (void *data) {
    free(data);
}

int 
main (int argc, char **argv)
{
    int status = 1;

    if (tree_init(10, 20, 2, set, rm, lk) != 0)
        goto exit;
    
    tree_add_reference(mk(0), NODE_INVALID);
    tree_add_reference(mk(1), 0);
    tree_add_reference(mk(2), 1);
    tree_add_reference(mk(3), 1);
    tree_add_reference(mk(4), 3);
    tree_add_reference(mk(5), 0);

    status = 0;
    tree_cleanup();
exit:
    return status;
}
