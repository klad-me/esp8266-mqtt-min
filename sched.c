#include "sched.h"

#include <osapi.h>
#include <user_interface.h>
#include <mem.h>
#include <espconn.h>


#define PRIO	 1


typedef void(*sched_cb)(void*);


struct arg
{
	sched_cb cb;
	void *arg;
};


static os_event_t Q[32];


static void sched_task(os_event_t *evt)
{
	struct arg *arg=(struct arg*)evt->par;
	
	arg->cb(arg->arg);
	os_free(arg);
}


void sched_init(void)
{
	system_os_task(sched_task, PRIO, Q, sizeof(Q) / sizeof(os_event_t));
}


void sched(sched_cb cb, void *arg)
{
	struct arg *a=(struct arg*)os_malloc(sizeof(struct arg));
	a->cb=cb;
	a->arg=arg;
	
	system_os_post(PRIO, 0, (os_param_t)a);
}


static void close_cb(void *arg)
{
	espconn_disconnect( (struct espconn*)arg );
}


void sched_espconn_disconnect(struct espconn *c)
{
	sched(close_cb, c);
}


static void delete_cb(void *arg)
{
	espconn_delete( (struct espconn*)arg );
}


void sched_espconn_delete(struct espconn *c)
{
	sched(delete_cb, c);
}
