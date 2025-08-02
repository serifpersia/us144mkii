obj-m += snd-usb-us144mkii.o
snd-usb-us144mkii-y := us144mkii.o pcm.o playback.o capture.o midi.o controls.o

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
