obj-m += tascam_raw.o

CC = gcc
CFLAGS = -Wall -O2
LDFLAGS = -ljack

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all: module client

module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

client: jack_tascam

jack_tascam: jack_tascam.c
	$(CC) $(CFLAGS) -o jack_tascam jack_tascam.c $(LDFLAGS)

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f jack_tascam
