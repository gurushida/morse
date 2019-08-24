LINUX_SRC_DIR=/lib/modules/`uname -r`/build
EXTRA_CFLAGS += -I/usr/src/linux-headers-`uname -r`/include/

obj-m := morse.o

all:
	make -C $(LINUX_SRC_DIR)  M=`pwd` modules

clean:
	make -C $(LINUX_SRC_DIR) M=`pwd` clean

