#include <linux/sched.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <asm/uaccess.h>
#include <linux/uaccess.h>
#include <linux/sched.h>
#include <asm/current.h>
#include <asm/atomic.h>
#include <linux/semaphore.h>
#include <linux/wait.h>
#include <linux/vt_kern.h>
#include <linux/tty.h>
#include <linux/tty_driver.h>
#include <linux/kd.h>
#include <linux/console_struct.h>
#include <linux/device.h>


#define AUTHOR "gurushida"
#define DESCRIPTION "This is a sample module that converts into morse characters written to the device"

#define DEVICE_NAME "morse"
#define CLASS_NAME "morse"

#define NORMAL_LEDS 0xFF
#define LEDS_ON 0x07


static int major;
static struct class* morseClass = NULL;
static struct device* morseDevice = NULL;


static atomic_t write_in_progress=ATOMIC_INIT(0);

DEFINE_SEMAPHORE(write_sem);
DEFINE_SEMAPHORE(read_sem);

// This is a queue for processes waiting for a blocking write on the device file
DECLARE_WAIT_QUEUE_HEAD(queue);

// And this one will be used to sleep a little bit
DECLARE_WAIT_QUEUE_HEAD(wait_queue);


#define BUFFER_MAX 1024
static char buffer[BUFFER_MAX];
static int buffer_size = 0;



/**
 * Our open function updates the module usage counter.
 */
static int my_open(struct inode* in, struct file* f) {
	try_module_get(THIS_MODULE);
	return 0;
}



static int my_release(struct inode* in, struct file* f) {
	module_put(THIS_MODULE);
	return 0;
}



/**
 * We fill the user buffer with data taken from the current morse buffer.
 */
static ssize_t my_read(struct file* filp, char __user* user_buffer, size_t length, loff_t *offset) {
	int n, available;

	// We have to avoid race condition with 'write' that may modify the buffer
	down(&read_sem);
	if (*offset >= buffer_size) {
		// Forget this unlock and you just freeze forever your system
		up(&read_sem);
		return 0;
	}

	available = buffer_size - (*offset);
	if (available < length) {
		n = available;
	} else {
		n = length;
	}

	// We cannot use a normal copy, since 'user_buffer' has a kernel-space address and
	// 'buffer' as a user-space one
	if (0 != copy_to_user(user_buffer, buffer + (*offset), n)) return -EFAULT;

	// Don't forget to release the spinlock, and do it as soon as possible
	up(&read_sem);

	*offset = *offset + n;
	return n;
}



/**
 * This function converts the morse buffer into keyboard led flashes.
 */
static void blink(void) {
	int i;
	// Doing evil: modifying led state requires privilege that we have not unless we have
	//             the CAP_SYS_TTY_CONFIG capability, but as we are in kernel code, we can
	//             temporarily get it
	struct cred* backup_cred=prepare_creds();
	struct cred* new_cred=prepare_creds();
	struct tty_driver* driver;
	if (backup_cred == NULL || new_cred == NULL) {
		printk(KERN_INFO "prepare_creds failed\n");
		return;
	}

	// Getting all root powers
	memset(&(new_cred->cap_effective), 0xFF, sizeof(new_cred->cap_effective));
	commit_creds(new_cred);

	// We get the driver associated to the foreground console. With it,
	// we will be able to modify the keyboard led settings
	driver = vc_cons[fg_console].d->port.tty->driver;
	for (i = 0 ; i < buffer_size ; i++) {
		switch(buffer[i]) {
		case ' ': {
			// Silence: we turn the led to normal mode for a little while
			wait_event_interruptible_timeout(wait_queue, 0, 100);
			break;
		}
		case '.': {
			// We enlight all leds for a short while
			driver->ops->ioctl(vc_cons[fg_console].d->port.tty, KDSETLED, LEDS_ON);
			wait_event_interruptible_timeout(wait_queue, 0, 50);
			driver->ops->ioctl(vc_cons[fg_console].d->port.tty, KDSETLED, NORMAL_LEDS);
			wait_event_interruptible_timeout(wait_queue, 0, 50);
			break;
		}
		case '-': {
			// We enlight all leds for a longer while
			driver->ops->ioctl(vc_cons[fg_console].d->port.tty, KDSETLED, LEDS_ON);
			wait_event_interruptible_timeout(wait_queue, 0, 150);
			driver->ops->ioctl(vc_cons[fg_console].d->port.tty, KDSETLED, NORMAL_LEDS);
			wait_event_interruptible_timeout(wait_queue,0,50);
			break;
		}
		}
	}

	// And we make sure that we return to normal led state
	driver->ops->ioctl(vc_cons[fg_console].d->port.tty, KDSETLED, NORMAL_LEDS);

	// We don't want to keep root powers, do we :-) ?
	commit_creds(backup_cred);
}



static const char* convert(char c) {
	if (c >= 'a' && c <= 'z') {
		c = c - 'a' + 'A';
	}

	switch(c) {
	case 'A': return ".-";
	case 'B': return "-...";
	case 'C': return "-.-.";
	case 'D': return "-..";
	case 'E': return ".";
	case 'F': return "..-.";
	case 'G': return "--.";
	case 'H': return "....";
	case 'I': return "..";
	case 'J': return ".---";
	case 'K': return "-.-";
	case 'L': return ".-..";
	case 'M': return "--";
	case 'N': return "-.";
	case 'O': return "---";
	case 'P': return ".--.";
	case 'Q': return "--.-";
	case 'R': return ".-.";
	case 'S': return "...";
	case 'T': return "-";
	case 'U': return "..-";
	case 'V': return "...-";
	case 'W': return ".--";
	case 'X': return "-..-";
	case 'Y': return "-.--";
	case 'Z': return "--..";
	case '0': return "-----";
	case '1': return ".----";
	case '2': return "..---";
	case '3': return "...--";
	case '4': return "....-";
	case '5': return ".....";
	case '6': return "-....";
	case '7': return "--...";
	case '8': return "---..";
	case '9': return "----.";
	default: return 0;
	}
}



/**
 * This is the write function for my own device. It updates
 * the number of bytes written to the device file.
 */
static ssize_t my_write(struct file* filp, const char __user* user_buffer, size_t length, loff_t* offset) {
	int i;
	char c;
	const char* value;
	int n;

	if (atomic_read(&write_in_progress)) {
		if (filp->f_flags & O_NONBLOCK) {
			// Non blocking operation ? We return
			return -EAGAIN;
		}

		// In blocking mode, we have to sleep
		while (atomic_read(&write_in_progress)) {
			int i, is_signal = 0;

			// We wait until the condition is true or a signal has been received
			wait_event_interruptible(queue, !atomic_read(&write_in_progress));

			// If we have been woken up by a signal that is not blocked by the process,
			// then we must return -ERESTARTSYS. If not, the process could not been interrupted
			// by signals like the Ctrl-C SIGINT
			for (i = 0 ; i < _NSIG_WORDS && !is_signal ; i++) {
				is_signal = current->pending.signal.sig[i] & ~current->blocked.sig[i];
			}

			if (is_signal) return -ERESTARTSYS;
		}
	}

	// Here, we can hold the locks
	atomic_set(&write_in_progress, 1);
	down(&write_sem);
	down(&read_sem);
	buffer_size = 0;

	for (i = 0 ; i < length ; i++) {
		get_user(c, user_buffer + i);
		value = convert(c);
		if (value == NULL) continue;
		n = strlen(value);
		if (buffer_size + n + 1 >= BUFFER_MAX) break;

		// We copy the morse value of the caracter to the buffer
		memcpy(buffer + buffer_size, value, n);

		// And we add a space to separate characters
		buffer[buffer_size + n] = ' ';
		buffer_size = buffer_size + n + 1;
	}

	// We release the read spinlock so that read operations can now occur
	// on the updated buffer
	up(&read_sem);

	// Let's flash keyboard's LEDs
	blink();

	// And once blinking is terminated, a new write operation may occur,
	// so we release the write spinlock
	up(&write_sem);

	// Now, we set back the condition to false and we wake up processes that are waiting
	// for writing into the device file
	atomic_set(&write_in_progress, 0);
	wake_up(&queue);

	// And we ask for a rescheduling, since otherwise, our process may perform a new
	// blocking write operation before the waiting processes can actually check the condition
	schedule();

	// Finally, we return the number of characters actually written to the device
	return i;
}



/**
 * I define here the file operations associated to my own character device file.
 * Note that I use here a C structure initialization style which is more
 * readable than the standard one with no field name. Fields that are not initialized
 * are set to NULL by gcc.
 */
static struct file_operations operations = {
		.open=my_open,
		.release=my_release,
		.read=my_read,
		.write=my_write
};



/**
 * This is meant to set the permissions to rw-rw-rw- for /dev/morse
 */
static char *my_devnode(struct device *dev, umode_t *mode) {
	if (!mode) return NULL;
	if (dev->devt == MKDEV(major, 0)) *mode = 0666;
	return NULL;
}


static int __init my_module_init(void) {
	major = register_chrdev(0, DEVICE_NAME, &operations);
	if (major < 0) {
		printk(KERN_ALERT "Error #%d occurred while registering the char device\n", major);
		return major;
	}

	// Register the device class
	morseClass = class_create(THIS_MODULE, CLASS_NAME);
	if (IS_ERR(morseClass)) {
		unregister_chrdev(major, DEVICE_NAME);
		printk(KERN_ALERT "Failed to register device class\n");
		return PTR_ERR(morseClass);
	}
	printk(KERN_INFO "'morse' device class registered correctly\n");

	morseClass->devnode = my_devnode;

	// Register the device driver in /dev/morse
	morseDevice = device_create(morseClass, NULL, MKDEV(major, 0), NULL, DEVICE_NAME);
	if (IS_ERR(morseDevice)) {
		class_destroy(morseClass);
		unregister_chrdev(major, DEVICE_NAME);
		printk(KERN_ALERT "Failed to create the device /dev/%s\n", DEVICE_NAME);
		return PTR_ERR(morseDevice);
	}

	printk(KERN_INFO "%s is registered with major number %d\n", DEVICE_NAME, major);
	printk(KERN_INFO "See it with 'cat /proc/device'\n");
	printk(KERN_INFO "You can now write ASCII text to /dev/%s and read from it to get the morse translation\n", DEVICE_NAME);
	return 0;
}



static void __exit my_module_exit(void) {
	device_destroy(morseClass, MKDEV(major, 0));
	class_unregister(morseClass);
	class_destroy(morseClass);
	unregister_chrdev(major, DEVICE_NAME);
	printk(KERN_INFO "Unloading module, bye.\n");
}

module_init(my_module_init);
module_exit(my_module_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR(AUTHOR);
MODULE_DESCRIPTION(DESCRIPTION);

