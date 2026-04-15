# Kernel module object
obj-m += monitor.o

# Compiler
CC = gcc
CFLAGS = -Wall -pthread

# Kernel build directory
KDIR := /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

# Default target
all: engine monitor

# Build user-space runtime
engine: engine.c monitor_ioctl.h
	$(CC) $(CFLAGS) engine.c -o engine

# Build kernel module
monitor:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

# Optional: build workload if you have source
memory_hog: memory_hog.c
	$(CC) memory_hog.c -o memory_hog

# Clean build files
clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f engine memory_hog
