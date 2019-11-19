#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/rculist.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/kthread.h>
#include <linux/delay.h>

#define RECOVERY_SLEEP_TIME 30

struct state {
	bool is_in_recovery;
};

struct time {
	int time;
};

struct client {
	int id;
	struct list_head __rcu *clients_list;
};

struct headers {
	bool cors;
	int content_type;
	int timeout;
};

struct server {
	struct list_head		clients;
	struct headers		__rcu	*headers;
	struct state		__rcu	*state;
	struct time		__rcu	*update_timestamp;
};

static struct server server;
DEFINE_SPINLOCK(server_mutex);
DEFINE_SPINLOCK(state_mutex);

static inline int initialize_time(void) {
	struct time *time;

	time = kmalloc(sizeof(*time), GFP_KERNEL);

	if(time == NULL) {
		return -ENOMEM;
	}

	time->time = 0;

	rcu_assign_pointer(server.update_timestamp, time);

	return 0;
}

static inline int initialize_state(void) {
	struct state *state;

	state = kmalloc(sizeof(*state), GFP_KERNEL);

	if(state == NULL) {
		return -ENOMEM;
	}

	state->is_in_recovery = false;

	rcu_assign_pointer(server.state, state);

	return 0;
}

static inline int initialize_headers(void) {
	struct headers *headers;

	headers = kmalloc(sizeof(*headers), GFP_KERNEL);

	if(headers == NULL) {
		return -ENOMEM;
	}

	headers->cors = true;
	headers->content_type = 3;
	headers->timeout = 5;

	rcu_assign_pointer(server.headers, headers);

	return 0;
}

static inline int initialize_server(void) {
	int err;

	INIT_LIST_HEAD(&server.clients);

	err = initialize_headers();
	if(err) goto err;

	err = initialize_state();
	if(err) goto err;

	err = initialize_time();
	if(err) goto err;

	return 0;

err:
	return err;
}

static inline int setup_client(void *data) {
	struct headers *headers;
	bool is_in_recovery;
	int timeout;

	while(!kthread_should_stop()) {
		rcu_read_lock();
		is_in_recovery = rcu_dereference(server.state)->is_in_recovery;
		if(is_in_recovery) {
			rcu_read_unlock();
			msleep(RECOVERY_SLEEP_TIME*1000);
			continue;
		}

		headers = rcu_dereference(server.headers);
		printk(KERN_INFO "CORS: %d\nContent-Type: %d\nTimeout:%d",
				headers->cors, headers->content_type, headers->timeout);

		timeout = headers->timeout;
		rcu_read_unlock();

		msleep(timeout*1000);
	}

	do_exit(0);
}

static int __init http_server_rcu_init(void) {
	if(!initialize_server()) {
		return -EFAULT;
	}
	printk(KERN_ERR "Initializing server!");
	return 0;
}

static void __exit http_server_rcu_exit(void) {
	printk(KERN_ERR "Destroying server!");
}

module_init(http_server_rcu_init);
module_exit(http_server_rcu_exit);
