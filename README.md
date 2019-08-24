# morse
A Linux kernel module to convert text into morse by blinking the capslock LED Cryptonomicon's style.

## How does it work
When installed, this module creates a `/dev/morse` character device. If you send ASCII text to it, it will only retain ASCII letters and digits and convert them to morse by blinking the capslock light. After that, you can read from the device to get the morse translation:

```
$ echo sos > /dev/morse
<blinks>
$ cat /dev/morse
... --- ...
```

## How to install
This has been tested on Ubuntu 18.04 and 19.04 on which all the headers and binaries needed to build kernel modules are already installed by default. Just run `make` and then `sudo insmod morse.ko`. You can verify that the installation went ok by looking at the logs with `dmesg`.
