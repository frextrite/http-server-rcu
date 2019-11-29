obj-m += http_server_rcu.o

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) C=1

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
