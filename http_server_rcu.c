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
#define TIME_TO_RECOVER 25
#define TIME_BEFORE_RECOVERY 60
#define NUM_CLIENTS 3
#define TIMEOUT_MULTIPLIER 5

struct state {
	bool is_in_recovery;
	struct rcu_head rcu;
};

struct time {
	int time;
};

struct client {
	int id;
	struct task_struct	*task;
	struct list_head	clients_list;
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
static DEFINE_SPINLOCK(server_mutex);
static DEFINE_SPINLOCK(state_mutex);

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
static inline void send_data_carefully(int id) {
	printk(KERN_INFO "Data:\nid: %d\nStatus Code: 438\nMode: Recovery\n", id);
}

/*
 * Conditions are normal, and we are being executed in a read section
 * we can dereference the data and send it. */
static inline void send_data(int id) {
	struct web_data *web_data = rcu_dereference_check(server.web_data,
							rcu_read_lock_held());

	printk(KERN_INFO "Data:\nid: %d\nStatus Code: 200\nMode: Normal\nData: %d\n",
			id, web_data->message);
}

/*
 * Client thread */
static inline int setup_client(void *data) {
	int timeout = *(int*)data;
	bool is_in_recovery;

	while(!kthread_should_stop()) {
		rcu_read_lock();
		is_in_recovery = rcu_dereference(server.state)->is_in_recovery;
		if(is_in_recovery) {
			send_data_carefully(timeout/TIMEOUT_MULTIPLIER);
		} else {
			send_data(timeout/TIMEOUT_MULTIPLIER);
		}
		rcu_read_unlock();
	
		msleep_interruptible(timeout*1000);
	}

	return 0;
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
		spin_unlock(&server_mutex);
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
	msleep_interruptible(TIME_TO_RECOVER*1000);

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
		msleep_interruptible(TIME_BEFORE_RECOVERY*1000);

		printk(KERN_INFO "HTTP-SERVER: [FATAL] Some error occured. Initializing recovery procedure.\n");
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

		printk(KERN_INFO "HTTP-SERVER: Starting server secovery\n");

		/*
		 * Fix the corrupt data.
		 * */
		recover_server();

		printk(KERN_INFO "HTTP-SERVER: Server successfully recovered\n");

		/*
		 * Recovery is done. Readers can now access server.web_data.
		 * */
		set_mode_recovery(false);

		set_current_state(TASK_INTERRUPTIBLE);
		schedule();
	}

	return 0;
}

static inline int updater_thread(void *data) {
	struct web_data *web_data;
	struct web_data *new_web_data;

	rcu_read_lock();
	if(rcu_dereference(server.state)->is_in_recovery) {
		rcu_read_unlock();
		goto exit;
	}
	rcu_read_unlock();

	spin_lock(&server_mutex);
	web_data = rcu_dereference_protected(server.web_data,
			lockdep_is_held(&server_mutex));

	new_web_data = kmalloc(sizeof(*new_web_data), GFP_KERNEL);

	if(new_web_data == NULL) {
		spin_unlock(&server_mutex);
		return -ENOMEM;
	}

	new_web_data->message = (web_data->message)+3;
	rcu_head_init(&new_web_data->rcu);
	rcu_assign_pointer(server.web_data, new_web_data);
	spin_unlock(&server_mutex);

	kfree_rcu(web_data, rcu);

	while(!kthread_should_stop()) {
		set_current_state(TASK_INTERRUPTIBLE);
		schedule();
	}

exit:
	return 0;
}

static inline void clean_up_threads(void) {
	struct client *client, *tclient;
	list_for_each_entry_safe(client, tclient, &server.clients,
			clients_list) {
		if(client->task != NULL) {
			kthread_stop(client->task);
		}
		kfree(client);
	}
}

/*
 * Initializes client processes
 * @n - number of threads to create
 * */
static inline int initialize_clients(int n) {
	int i, *timeout;
	char *name;
	struct client *client;

	name = kmalloc(8, GFP_KERNEL);
	if(name == NULL) return -ENOMEM;

	for(i = 0; i < n; i++) {
		client = kmalloc(sizeof(*client), GFP_KERNEL);

		if(client == NULL) {
			clean_up_threads();
			return -ENOMEM;
		}

		sprintf(name, "thread%d", i);

		timeout = kmalloc(sizeof(int), GFP_ATOMIC);
		*timeout = (i+1) * TIMEOUT_MULTIPLIER;

		client->id = i+1;
		client->task = kthread_create(setup_client, (void*)timeout, name);

		if(client->task == NULL) {
			clean_up_threads();
			return -ENOMEM;
		}

		list_add(&client->clients_list, &server.clients);
	}

	return 0;
}

static inline int initialize_crash(void) {
	struct client *client;
	char *name;

	name = kmalloc(128, GFP_ATOMIC);
	if(name ==  NULL) return -ENOMEM;
	sprintf(name, "recovery_thread_rcu");

	client = kmalloc(sizeof(*client), GFP_KERNEL);

	if(client == NULL) {
		goto no_mem;
	}

	client->id = 7234;
	client->task = kthread_create(recover_system_thread, NULL, name);

	if(client->task == NULL) {
		goto no_mem;
	}

	list_add(&client->clients_list, &server.clients);

	return 0;

no_mem:
	clean_up_threads();
	return -ENOMEM;
}

static int __init http_server_rcu_init(void) {
	struct client *client;

	if(initialize_server()) {
		return -EFAULT;
	}

	if(initialize_clients(NUM_CLIENTS)) {
		return -EFAULT;
	}

	if(initialize_crash()) {
		return -EFAULT;
	}

	printk(KERN_ERR "Initializing server!");
	printk(KERN_ERR "Initial Server Status\nMessage: %d\nRecovery: %d\nTimestamp: %d\n",
			server.web_data->message,
			server.state->is_in_recovery,
			server.update_timestamp->time);


	list_for_each_entry(client, &server.clients, clients_list) {
		if(client->task) {
			wake_up_process(client->task);
		}
	}

	return 0;
}

static void __exit http_server_rcu_exit(void) {
	printk(KERN_ERR "Destroying server!");
	clean_up_threads();
	printk(KERN_ERR "Cleanup done!");
}

module_init(http_server_rcu_init);
module_exit(http_server_rcu_exit);
