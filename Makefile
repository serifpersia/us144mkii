obj-m += snd-usb-us122mkii.o
snd-usb-us122mkii-y := us122mkii.o

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
