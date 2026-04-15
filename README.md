# OS-Jackfruit
Supervised Multi-Container Runtime with Kernel Memory Monitor
MALAVIKA ARUN PES1UG24CS257
ADVIKA RAJ PES1UG24CS906

Project Overview
This project implements a lightweight Linux container runtime in C with:

A user-space supervisor (engine.c) that manages multiple containers
A kernel module (monitor.c) that enforces memory limits
A bounded-buffer logging system for container output
A CLI interface for container lifecycle management

Engineering Analysis
1. Isolation Mechanisms
Modern operating systems provide isolation using namespaces, which allow processes to have independent views of system resources while sharing the same kernel.
In this project:
->PID namespace ensures each container has its own process tree starting at PID 1
->UTS namespace isolates hostnames, allowing each container to have a unique identity
->Mount namespace isolates filesystem views

Additionally, chroot() restricts the container’s root directory, ensuring it cannot access files outside its assigned root filesystem.

2. Supervisor and Process Lifecycle
A long-running supervisor process is essential for managing container lifecycles.
In Linux:
Every process has a parent
The parent is responsible for reaping child processes

Without a supervisor:
terminated containers would become zombies
metadata tracking would be lost

The supervisor in this project:
->creates containers using clone()
->maintains metadata (PID, state, limits)
->handles SIGCHLD to reap children


3. IPC, Threads, and Synchronization
The project uses two distinct IPC mechanisms, reflecting separation of concerns:

Control Path 
Implemented using a UNIX domain socket
Provides structured request-response communication
Logging Path 
Implemented using pipes
Streams stdout/stderr efficiently
Bounded Buffer Design

A producer-consumer model is used:
Producers: read container output
Consumer: writes logs to files

Synchronization:
Mutex ensures mutual exclusion
Condition variables prevent busy waiting


4. Memory Management and Enforcement
The kernel module tracks RSS (Resident Set Size), which represents the actual physical memory used by a process.

Two policies are implemented:
Soft limit → generates a warning
Hard limit → enforces termination

5. Scheduling Behavior
This project uses the Completely Fair Scheduler ,containers as an experimental platform to observe scheduling behavior under:

different priorities (nice values)
different workload types

The results show how:
CPU-bound processes compete for CPU time
I/O-bound processes get better responsiveness
priority adjustments influence CPU allocation

5.Design Decisions and Tradeoffs
1. Namespace Isolation
Choice: Used clone() with PID, UTS, and mount namespaces

Tradeoff:
Simpler than full virtualization
Less isolation compared to virtual machines

2. Supervisor Architecture
Choice: Single long-running supervisor process

Tradeoff:
Centralized design simplifies management
But introduces a single point of failure

3. IPC and Logging System
Choice:
UNIX socket for control
Pipes + bounded buffer for logging

Tradeoff:
More complex than direct file writes
Requires synchronization

6.Scheduler Experiment Results
Container	Nice Value	Completion Time
A	            0	        12 sec
B	            10	        20 sec
Observation:
Lower nice value → higher CPU priority
Container A finished faster
