#ifndef SCHED_H
#define SCHED_H


#include <c_types.h>


#ifdef __cplusplus
extern "C" {
#endif


struct espconn;


typedef void(*sched_cb)(void*);


void sched_init(void);
void sched(sched_cb cb, void *arg);

void sched_espconn_disconnect(struct espconn *c);
void sched_espconn_delete(struct espconn *c);


#ifdef __cplusplus
}
#endif


#endif
