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
#define TIME_TO_RECOVER 10
#define TIME_BEFORE_RECOVERY 10

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
static inline void send_data_carefully(void) {
	printk(KERN_INFO "Data:\nStatus Code: 438\nMode: Recovery\n");
}

/*
 * Conditions are normal, and we are being executed in a read section
 * we can dereference the data and send it. */
static inline void send_data(void) {
	struct web_data *web_data = rcu_dereference_check(server.web_data,
							rcu_read_lock_held());

	printk(KERN_INFO "Data:\nStatus Code: 200\nMode: Normal\nData: %d\n",
			web_data->message);
}

/*
 * Client thread */
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
			send_data();
		}
		rcu_read_unlock();
	
		msleep(RECOVERY_SLEEP_TIME*1000);
	}

	do_exit(0);
}

static inline void set_mode_recovery(bool flag) {
	struct state *current_state;

	spin_lock(&state_mutex);
	current_state = rcu_dereference_protected(server.state,
			lockdep_is_held(&state_mutex));

	if(current_state->is_in_recovery == flag) {
		spin_unlock(&state_mutex);
		return;
	}

	current_state->is_in_recovery = flag;
	spin_unlock(&state_mutex);
}

static inline int recover_server(void) {
	struct web_data *web_data;
	struct web_data *new_web_data;
	struct time *update_timestamp;

	/*
	 * No concurrent readers hence we can directly update the data
	 * */
	spin_lock(&server_mutex);
	web_data = rcu_dereference_protected(server.web_data,
			lockdep_is_held(&server_mutex));

	new_web_data = kmalloc(sizeof(*new_web_data), GFP_KERNEL);

	if(new_web_data == NULL) {
		return -ENOMEM;
	}

	new_web_data->message = (1<<(web_data->message));
	rcu_head_init(&new_web_data->rcu);

	rcu_assign_pointer(server.web_data, new_web_data);

	/*
	 * Note: we cannot use the following assignment since,
	 * below we need to use web_data->message to update the timestamp.
	 *
	 * If we'd used the following assignment and later used
	 * update_timestamp->time = web_data->message+1
	 * we might have gotten inconsistent data due to CPU/compiler
	 * reordering.
	 *
	 * PS: creating a simple variable would've solved this problem but RCU
	 * can also handle this!
	 * */
	// web_data->message = (1<<web_data->message);

	/*
	 * This is a simple example, but sadly recovering a failed system
	 * doesn't take a few nanoseconds.
	 * */
	msleep(TIME_TO_RECOVER*1000);

	update_timestamp = rcu_dereference_protected(server.update_timestamp,
			lockdep_is_held(&server_mutex));
	update_timestamp->time = web_data->message ^ update_timestamp->time;

	spin_unlock(&server_mutex);

	kfree_rcu(web_data, rcu);

	return 0;
}

/*
 * Thread created for recovering the server.
 *
 * The recovery of the system takes a lot of time to complete,
 * during which server.web_data may remain in an inconsistent state,
 * i.e., sending the data from server.web_data may be disastrous.
 *
 * To overcome this we first set the state to recovery, so that,
 * the server doesn't send the data from server.web_data
 * (send_data_carefully()),
 * post which we can work on fixing the problem. During this time, the system
 * will never read from server.web_data (inconsistent data).
 * */
static inline int recover_system_thread(void *data) {
	while(!kthread_should_stop()) {
		msleep(TIME_BEFORE_RECOVERY);

		set_mode_recovery(true);

		/*
		 * This synchronize_rcu() is important before modifying server.web_data
		 *
		 * This instructs all the on going reader sections to exit.
		 *
		 * If this were not the case, the system would continue to access and
		 * send data from server.web_data which now is in inconsistent state.
		 *
		 * But now, after this statement is executed, all the readers will see
		 * the updated state (recovery) and none of them would send inconsistent
		 * data.
		 *
		 * See setup_client()
		 * */
		synchronize_rcu();

		/*
		 * Fix the corrupt data.
		 * */
		recover_server();

		/*
		 * Recovery is done. Readers can now access server.web_data.
		 * */
		set_mode_recovery(false);

		set_current_state(TASK_INTERRUPTIBLE);
		schedule();
	}

	do_exit(0);
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
