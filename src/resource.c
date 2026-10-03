/*
 * resource.c - Global XID lookup table for the Y11 display server.
 *
 * A chained hash map from yid_t to typed resources (windows, pixmaps,
 * GCs), so any request referencing a resource id resolves in O(1).
 */

#include <stdlib.h>
#include <string.h>

#include "y11.h"
#include "y11_wire.h"

#define Y11_RESOURCE_BUCKETS 512u

struct y11_resource {
    yid_t id;
    int type;
    void *ptr;
    struct y11_resource *next;
};

static struct y11_resource *y11_resource_table[Y11_RESOURCE_BUCKETS];

static uint32_t y11_resource_hash(yid_t id)
{
    return (uint32_t)((id * 2654435761u) % Y11_RESOURCE_BUCKETS);
}

int y11_resource_init(void)
{
    return 0;                   /* static table needs no setup */
}

void y11_resource_shutdown(void)
{
    unsigned i;

    for (i = 0; i < Y11_RESOURCE_BUCKETS; i++) {
        struct y11_resource *r = y11_resource_table[i];
        while (r != NULL) {
            struct y11_resource *next = r->next;
            free(r);
            r = next;
        }
        y11_resource_table[i] = NULL;
    }
}

/* Register id -> ptr.  Returns -1 if the id is already in use. */
int y11_resource_add(yid_t id, int type, void *ptr)
{
    uint32_t bucket = y11_resource_hash(id);
    struct y11_resource *r = y11_resource_table[bucket];

    for (; r != NULL; r = r->next) {
        if (r->id == id)
            return -1;
    }

    r = malloc(sizeof(*r));
    if (r == NULL)
        return -1;
    r->id = id;
    r->type = type;
    r->ptr = ptr;
    r->next = y11_resource_table[bucket];
    y11_resource_table[bucket] = r;
    return 0;
}

/* Return the resource pointer for id, or NULL when absent or mistyped. */
void *y11_resource_get(yid_t id, int type)
{
    uint32_t bucket = y11_resource_hash(id);
    const struct y11_resource *r = y11_resource_table[bucket];

    for (; r != NULL; r = r->next) {
        if (r->id == id && r->type == type)
            return r->ptr;
    }
    return NULL;
}

void y11_resource_remove(yid_t id)
{
    uint32_t bucket = y11_resource_hash(id);
    struct y11_resource *r = y11_resource_table[bucket];
    struct y11_resource **link = &y11_resource_table[bucket];

    while (r != NULL) {
        if (r->id == id) {
            *link = r->next;
            free(r);
            return;
        }
        link = &r->next;
        r = r->next;
    }
}
