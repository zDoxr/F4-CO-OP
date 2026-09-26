#ifndef __ENET_CALLBACKS_H__
#define __ENET_CALLBACKS_H__

#include <stdlib.h>

typedef struct _ENetCallbacks
{
    void * (ENET_CALLBACK * malloc) (size_t size);
    void (ENET_CALLBACK * free) (void * memory);
    void (ENET_CALLBACK * no_memory) (void);
} ENetCallbacks;

#ifdef __cplusplus
extern "C"
{
#endif

extern void * enet_malloc (size_t);
extern void   enet_free (void *);

#ifdef __cplusplus
}
#endif

#endif
