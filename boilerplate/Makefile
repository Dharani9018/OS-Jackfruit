obj-m += monitor.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)
WORKLOAD_LDFLAGS ?= -static

USER_TARGETS := engine memory_hog cpu_hog io_pulse

all: $(USER_TARGETS) module

module: monitor.ko

ENGINE_SRCS := engine.c \
               supervisor/supervisor.c \
               supervisor/container.c \
               supervisor/signal_handler.c \
               cli/cli.c \
               logging/log_buffer.c

engine: $(ENGINE_SRCS) engine.h monitor_ioctl.h
	gcc -O2 -Wall -Wextra -I. -o engine $(ENGINE_SRCS) -lpthread

memory_hog: workloads/memory_hog.c
	gcc -O2 -Wall $(WORKLOAD_LDFLAGS) -o memory_hog workloads/memory_hog.c

cpu_hog: workloads/cpu_hog.c
	gcc -O2 -Wall $(WORKLOAD_LDFLAGS) -o cpu_hog workloads/cpu_hog.c

io_pulse: workloads/io_pulse.c
	gcc -O2 -Wall $(WORKLOAD_LDFLAGS) -o io_pulse workloads/io_pulse.c

monitor.ko: monitor.c monitor_ioctl.h
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	if [ -d "$(KDIR)" ]; then $(MAKE) -C $(KDIR) M=$(PWD) clean; fi
	rm -f $(USER_TARGETS) *.o *.mod *.mod.c *.symvers *.order
	rm -f *.log
	rm -rf logs
	rm -f /tmp/mini_runtime.sock

.PHONY: all module clean
