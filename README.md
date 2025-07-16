# Simple Messaging Kernel Module

This project implements a Linux kernel loadable module (KLM) that provides a basic messaging system between user-space processes, using circular queues managed in kernel space.

It was developed as part of the Operating Systems course (Prof. Sérgio Johann Filho) and cross-compiled with **Buildroot** for use on an embedded Linux environment.

## Overview

The module exposes a character device `/dev/mq` that allows user-space applications to:

* **Register** and **unregister** a process under a name.
* **Send messages** to specific registered processes.
* **Broadcast messages** to all registered processes.
* **Read messages** from the process's own queue.

A simple C client (`client.c`) is provided to interact with the device and test its behavior.

## How It Works

### Data Structures

In kernel space, the module maintains:

* A **control block list**, which holds:

  * The process **PID**.
  * An assigned **name** (up to 8 chars).
  * A **message queue** (circular buffer).
  * A spinlock for concurrent access.

Each message queue holds up to `QUEUE_LEN` messages, storing:

* The message **data** (dynamically allocated).
* The **sender's name**.
* The message **size**.

### Main Operations

* `/reg <name>`  — Registers the calling process's PID and name.
* `/unr` — Unregisters the calling PID and cleans up its message queue.
* `/msg <dest> <msg>` — Sends a message to the named destination, if registered.
* `/all <msg>` — Sends the same message to all other registered processes.
* `/read` — Reads (and removes) the next message in the calling process's queue.

### Parameters

The module can be configured at load time with:

* `MAX_DEVICES`: maximum number of registered processes.
* `QUEUE_LEN`: number of messages per process queue.
* `CMD_BUF_SIZE`: maximum command size.

Example:

```bash
modprobe mq_driver MAX_DEVICES=8 QUEUE_LEN=8 CMD_BUF_SIZE=256
```

## Notes on Implementation

### Use of PID

**Disclaimer:**  In a more formal or production-grade Linux driver, one would typically associate user-space interactions via file descriptors, file pointers (`struct file`), or inode references to track context.

In this project, for simplicity and as an educational abstraction, the process PID is used as the unique identifier. This avoids managing file structures and focuses on learning kernel data structures and synchronization.

This choice makes it easier to reason about process-specific state but limits interactions to one registration per PID and assumes stable PID handling.

### Memory Management

* Memory is dynamically allocated using `kmalloc`/`kfree`.
* Message queues are properly cleaned up on process unregistration.
* Circular buffer logic ensures that if a queue overflows, the oldest message is overwritten.
* Spinlocks protect both the control block list and per-process queues against race conditions.

### Build and Deployment

The system was cross-compiled using **Buildroot** to generate a minimal Linux environment for testing on embedded hardware.

Scripts are included to:

* Build the kernel module.
* Install and load it (`insmod`/`rmmod`).
* Compile the client application.

## Repository Structure

```
.
├── client.c         # User-space C client for testing
├── mq_driver.c      # Kernel module source
├── Makefile         # Build rules
├── README.md        # This documentation
└── tp2.pdf         # Project description and assignment details
```

## Final Remarks

This project is an educational exercise aimed at deepening understanding of:

* Kernel-space vs user-space boundaries.
* Character device drivers.
* Process communication.
* Memory and concurrency management at kernel level.

## Limitations and Ideas for Expansion

### Current Limitations

* **PID-based Identification** — Only one registration per PID; no multithreaded support; risk with PID reuse.
* **No File Descriptor Context** — Lacks per-connection tracking, limits flexibility.
* **Simple Error Handling** — Mainly logs to kernel, no rich user-space error feedback.
* **Fixed Load-Time Configuration** — No runtime parameter adjustment.
* **Limited Metadata** — Messages have no timestamps, priorities, or headers.
* **No Async Notification** — Relies on polling; no `poll()`, signals, or epoll integration.

### Ideas for Expansion

* Use `struct file` or inodes to track connections, allowing multiple registrations per process and better cleanup.
* Add `poll()`, `select()`, or `fasync` support for asynchronous notifications.
* Extend messages with metadata (timestamps, priorities, etc.).
* Add sysfs or procfs interfaces for runtime configuration.
* Improve user-space error reporting with ioctl interfaces or structured codes.
* Add access control or namespace integration for security.
* Develop unit tests, stress tests, and fault injection mechanisms.
