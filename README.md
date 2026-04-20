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
---
## Implementation of Each OS Concept:


## 1. Process Isolation (Namespaces & chroot)

**How we implemented it:**

We used the `clone()` system call which creates a new process but allows us to specify what resources it shares with the parent. We passed three namespace flags:

| Namespace | What it isolates |
|-----------|-----------------|
| **PID namespace** | Container sees only its own processes. Inside the container, its process appears as PID 1, even though on the host it's PID 5000. |
| **Mount namespace** | Container gets its own mount points. Changes inside don't affect the host. |
| **UTS namespace** | Container can have its own hostname different from the host. |

After creating the isolated process, we call `chroot()` which changes the root directory for that process. The container now sees its own rootfs (like `rootfs-alpha/`) as `/` and cannot access files outside it.

Finally, we mount `/proc` inside the container so commands like `ps` work properly within the isolated environment.

**The result:** A process that believes it's running as PID 1 in its own Linux system with its own filesystem, completely unaware of other containers or the host.

---

## 2. Supervisor Process (Long-running Daemon)

**How we implemented it:**

The supervisor is a continuously running program that never exits unless explicitly stopped. It does three main things:

**First:** It creates a UNIX socket file at `/tmp/mini_runtime.sock`. This socket acts like a "phone line" that CLI clients can call to send commands.

**Second:** It sets up signal handlers. When a container exits, the kernel sends SIGCHLD to the supervisor. Our handler catches this and updates the container's state from "running" to "exited" while also recording the exit code. When someone presses Ctrl+C, SIGINT is caught and triggers graceful shutdown.

**Third:** It enters an infinite loop using `select()` to wait for incoming connections on the socket. When a CLI client connects, the supervisor accepts the connection, reads the command, processes it (start container, stop container, list containers, etc.), sends back a response, and closes the connection - then goes back to waiting.

**The result:** A central manager that stays alive, tracks all containers, handles CLI requests, and ensures clean shutdown.

---

## 3. Bounded Buffer (Producer-Consumer)

**How we implemented it:**

We created a circular buffer - think of it as a ring of fixed-size slots (64 slots of 4KB each). Multiple threads can add data (producers) and one thread removes data (consumer).

**The challenge:** Producers and consumer run simultaneously. Without coordination:
- Producer might add data to a slot the consumer is reading
- Consumer might try to read from an empty buffer
- Two producers might try to write to the same slot

**Our solution:** We used three synchronization tools:

| Tool | Purpose |
|------|---------|
| **Mutex (lock)** | Only one thread can access the buffer at a time - like a bathroom lock |
| **not_full condition variable** | Producers wait here if buffer is full; consumer wakes them when space available |
| **not_empty condition variable** | Consumer waits here if buffer is empty; producers wake it when data available |

**The flow:**
- Producer: Locks buffer → If full, waits on not_full → Adds data → Signals not_empty → Unlocks
- Consumer: Locks buffer → If empty, waits on not_empty → Removes data → Signals not_full → Unlocks

**The result:** No data corruption, no lost data, no busy-waiting wasting CPU. Threads sleep when they can't proceed and wake exactly when needed.

---

## 4. Container Logging Pipeline

**How we implemented it:**

Each container's stdout and stderr are redirected through a **pipe** - a unidirectional communication channel that acts like a tube connecting the container to the supervisor.

**Step 1:** Before creating the container, we call `pipe()` which gives us two file descriptors - a read end and a write end.

**Step 2:** In the container child process, we use `dup2()` to redirect stdout and stderr to the write end of the pipe. Now anything the container prints goes into the pipe instead of the terminal.

**Step 3:** In the supervisor, we create a **producer thread** for each container. This thread continuously reads from the read end of the pipe, packages the data with the container ID, and pushes it into the shared bounded buffer.

**Step 4:** A single **consumer thread** runs forever, pulling log chunks from the bounded buffer and writing them to the appropriate per-container log file (like `logs/alpha.log`).

**The result:** Container output is captured asynchronously. Even if the container produces output faster than the disk can write, the bounded buffer absorbs the burst without blocking the container.

---

## 5. Kernel Memory Monitor (LKM)

**How we implemented it:**

We wrote a Linux Kernel Module that runs in kernel space with full access to system internals.

**Registration:** When a container starts, the supervisor sends the container's host PID and memory limits to the module via `ioctl()`. The module stores this in a kernel linked list.

**Periodic Checking:** The module sets up a timer that fires every 1 second. In the timer callback, it walks through the linked list of monitored processes.

**RSS Measurement:** For each process, it locates the `task_struct` (kernel's representation of a process), gets its `mm_struct` (memory management info), and calls `get_mm_rss()` which returns the number of physical pages in RAM. This is converted to bytes.

**Enforcement:**
- If RSS exceeds soft limit: Print a warning using `printk()` but do nothing else
- If RSS exceeds hard limit: Send SIGKILL using `send_sig()` and remove from monitoring list

**Cleanup:** If a process no longer exists (RSS returns -1), it's removed from the list. When the module unloads, all remaining entries are freed.

**The result:** Kernel-level enforcement that containers cannot bypass. The kernel knows exact memory usage and can kill processes instantly.

---

## 6. CLI and Supervisor Communication (IPC)

**How we implemented it:**

We used **UNIX domain sockets** - a form of IPC that works like network sockets but stays within the same machine.

**Supervisor side:** Creates a socket, binds it to a file path (`/tmp/mini_runtime.sock`), and listens for connections. This socket file appears in the filesystem and acts as the rendezvous point.

**CLI side:** The CLI is a short-lived program. It creates a socket, connects to the same file path, sends a structured request (containing command type, container ID, rootfs path, memory limits), waits for a response, prints it, and exits.

**Why this design:** The CLI doesn't need to stay running. Each command is a fresh process that connects, communicates, and terminates. This is lightweight and prevents resource leaks. The supervisor remains the single source of truth.

**The result:** Clean separation between client and server. Multiple CLI commands can be issued from different terminals simultaneously because the supervisor handles connections one at a time.

---

## 7. Container Metadata Management

**How we implemented it:**

We maintain a **linked list** of container records in the supervisor's memory. Each node contains: container ID, host PID, start time, current state, memory limits, exit code, and log file path.

**Concurrency protection:** Multiple parts of the supervisor access this list:
- CLI handler adds new containers when start command received
- SIGCHLD handler updates state when container exits
- CLI handler reads list for `ps` command

Without protection, one thread could modify the list while another is reading it, causing crashes or corrupted data. We use a **single pthread mutex** (mutual exclusion lock) that must be acquired before accessing the list.

**The rule:** Any function that reads or modifies the linked list must first lock the mutex, do its work, then unlock. If a thread tries to lock an already-locked mutex, it sleeps until the mutex is available.

**The result:** Thread-safe access to shared data. No race conditions, no corrupted pointers.

---

## 8. Signal Handling and Process Reaping

**How we implemented it:**

When a child process exits, it doesn't completely disappear - it becomes a **zombie** until the parent calls `wait()` or `waitpid()` to collect its exit status.

We installed a **SIGCHLD handler** - a function that the kernel calls whenever a child process terminates. Inside this handler, we call `waitpid(-1, &status, WNOHANG)` in a loop.

| Parameter | Meaning |
|-----------|---------|
| `-1` | Wait for any child process |
| `&status` | Store exit information here |
| `WNOHANG` | Don't block if no children have exited |

The handler checks whether the child exited normally (`WIFEXITED`) or was killed by a signal (`WIFSIGNALED`). It then updates the container's record:
- State becomes `CONTAINER_EXITED` or `CONTAINER_KILLED`
- Exit code or signal number is recorded
- If the `stop_requested` flag was set, we know it was a manual stop

**The result:** No zombie processes. All containers are properly cleaned up. The supervisor always knows the final status of every container it launched.

---

## Summary Table: Implementation Methods

| OS Concept | How We Implemented It |
|------------|----------------------|
| Process Isolation | `clone()` with PID/UTS/Mount namespace flags + `chroot()` to separate rootfs |
| Supervisor | Infinite select() loop on UNIX socket, signal handlers for lifecycle events |
| Bounded Buffer | Circular array with mutex lock + two condition variables (not_full, not_empty) |
| Container Logging | Pipe from container to producer thread → bounded buffer → consumer thread → file |
| Kernel Module | LKM with timer callback checking RSS every 1s, ioctl interface for registration |
| IPC | UNIX domain socket at `/tmp/mini_runtime.sock` with structured request/response |
| Metadata | Linked list protected by single pthread mutex |
| Signal Handling | SIGCHLD handler calling waitpid() in loop to reap children |
| Memory Enforcement | Soft limit = printk warning, Hard limit = send_sig(SIGKILL) |
