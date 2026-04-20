# Multi-Container Runtime

A lightweight Linux container runtime in C with a long-running supervisor and a kernel-space memory monitor.

This project involves building a lightweight Linux container runtime in C with a long-running parent supervisor and a kernel-space memory monitor. The container runtime must manage multiple containers at once, coordinate concurrent logging safely, expose a small supervisor CLI, and include controlled experiments related to Linux scheduling.

The project has two integrated parts:

1. **User-Space Runtime + Supervisor (`engine.c`)**  
   Launches and manages multiple isolated containers, maintains metadata for each container, accepts CLI commands, captures container output through a bounded-buffer logging system, and handles container lifecycle signals correctly.
2. **Kernel-Space Monitor (`monitor.c`)**  
   Implements a Linux Kernel Module (LKM) that tracks container processes, enforces soft and hard memory limits, and integrates with the user-space runtime through `ioctl`.
---

## Install dependencies:

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```
## 1. Prepare the Root Filesystem

```bash
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

# Make one writable copy per container you plan to run
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta
```
## 2. Build

```bash
cd boilerplate
make clean
make all
make monitor.ko
```

## 3. Build kernel module
```bash
sudo insmod monitor.ko
```
## 4. Start supervisor:
```bash
sudo ./engine supervisor ./rootfs-base
```
## 5. Multi-container:
### Container 1:
```bash
sudo ./engine start alpha ./rootfs-alpha "/bin/sh"
```
### Container 2:
```bash
sudo ./engine start beta ./rootfs-beta "/bin/sh"
```
## 4. Meta data:
```bash
sudo ./engine ps
```
## 5. Logs:

```bash
sudo ./engine start alpha ./rootfs-alpha "/cpu_hog 300"
sudo ./engine start beta ./rootfs-beta "/cpu_hog 300"
```

### Check logs:
```bash
cat logs/alpha.log
```
```bash
cat logs/beta.log
```
## 6. Cli and PC:
### check Working of Control plane IPC
```bash
ls -la /tmp/mini_runtime.sock
```
### Cli -> supervisor communication:
```bash
sudo ./engine run temp ./rootfs-alpha "echo Hello World"
```
### Supervisor -> container communication
```bash
cat logs/temp.log
```
## 7. Soft limit warning:
### Clear dmesg:
```bash
sudo dmesg -c > /dev/null
```
### Start memory hog with soft limit
```bash
sudo ./engine start soft_test ./rootfs-gamma "/memory_hog 2 100" --soft-mib 10 --hard-mib 50
```
###  Wait for warning
```bash
sleep 8
```
### Show soft limit warning:
```bash
sudo dmesg | grep "SOFT LIMIT"
```
## 8. Show hard limit :
```bash
sudo ./engine ps | grep soft_test
```


