CC = gcc
CFLAGS = -Wall -Wextra -O2 -D_GNU_SOURCE
LDFLAGS = -pthread

KERNEL_DIR ?= /lib/modules/$(shell uname -r)/build
obj-m += monitor.o

TARGETS = engine
WORKLOADS = workloads/cpu_hog workloads/io_pulse workloads/memory_hog

.PHONY: all clean ci workloads load-module unload-module

all: $(TARGETS) workloads

engine: engine.c monitor_ioctl.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

workloads/cpu_hog: workloads/cpu_hog.c
	$(CC) $(CFLAGS) -static -o $@ $<

workloads/io_pulse: workloads/io_pulse.c
	$(CC) $(CFLAGS) -static -o $@ $<

workloads/memory_hog: workloads/memory_hog.c
	$(CC) $(CFLAGS) -static -o $@ $<

workloads: $(WORKLOADS)

monitor.ko: monitor.c monitor_ioctl.h
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) modules

ci: $(TARGETS) workloads
	@echo "CI smoke check passed: user-space binaries built successfully"

load-module: monitor.ko
	sudo insmod monitor.ko
	@echo "Module loaded. Device: /dev/container_monitor"

unload-module:
	sudo rmmod monitor || true
	@echo "Module unloaded"

clean:
	rm -f engine $(WORKLOADS)
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) clean
	rm -f *.o *.ko *.mod.c *.mod *.order *.symvers *.mod.d
