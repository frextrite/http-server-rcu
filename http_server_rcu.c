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
	struct rcu_head rcu;
};

struct time {
	int time;
};

struct client {
	int id;
	struct list_head __rcu *clients_list;
};

struct web_data {
	int message;
	struct rcu_head rcu;
};

struct server {
	struct list_head		clients;
	struct web_data		__rcu	*web_data;
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
	rcu_head_init(&state->rcu);

	rcu_assign_pointer(server.state, state);

	return 0;
}

static inline int initialize_web_data(void) {
	struct web_data *web_data;

	web_data = kmalloc(sizeof(*web_data), GFP_KERNEL);

	if(web_data == NULL) {
		return -ENOMEM;
	}

	web_data->message = 0;
	rcu_head_init(&web_data->rcu);

	rcu_assign_pointer(server.web_data, web_data);

	return 0;
}

static inline int initialize_server(void) {
	int err;

	INIT_LIST_HEAD(&server.clients);

	err = initialize_web_data();
	if(err) goto err;

	err = initialize_state();
	if(err) goto err;

	err = initialize_time();
	if(err) goto err;

	return 0;

err:
	return err;
}

/*
 * This probably means we are in recovery, hence server.web_data may be in an
 * inconsistent state hence cannot dereference the data. */
static inline void send_data_carefully() {
	printk(KERN_INFO "Data:\nStatus Code: 438\nMode: Recovery\n");
}

/*
 * Conditions are normal, and we are being executed in a read section
 * we can dereference the data and send it. */
static inline void send_data() {
	struct web_data *web_data = rcu_dereference_check(server.web_data,
							rcu_read_lock_held());

	printk(KERN_INFO "Data:\nStatus Code: 200\nMode: Normal\nData: %d\n",
			web_data->message);
}

static inline int setup_client(void *data) {
	struct web_data *web_data;
	bool is_in_recovery;
	int timeout;

	while(!kthread_should_stop()) {
		rcu_read_lock();
		is_in_recovery = rcu_dereference(server.state)->is_in_recovery;
		if(is_in_recovery) {
			send_data_carefully();
		} else {
			sent_data();
		}
		rcu_read_unlock();
	
		msleep(RECOVERY_SLEEP_TIME*1000);
	}

	do_exit(0);
}

static inline int set_mode_recovery(bool flag) {
	struct state *current_state;

	spin_lock(&state_mutex);
	current_state = rcu_dereference_protected(server.state,
						lockdep_is_held(&state_mutex));

	if(current_state->is_in_recovery == flag) {
		spin_unlock(&state_mutex);
		goto exit;
	}

	current_state->is_in_recovery = flag;
	spin_unlock(&state_mutex);

exit:
	return 0;
}

static int __init http_server_rcu_init(void) {
	if(initialize_server()) {
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
