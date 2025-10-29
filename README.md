# Cascade

[![CMake](https://img.shields.io/badge/CMake-3.26+-blue.svg)](https://cmake.org/)
[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C++](https://img.shields.io/badge/C++-20-blue.svg)](https://en.cppreference.com/w/cpp/compiler_support)

Cascade Runtime.(In start of work lower content maybe changed)

A high-performance, lock-free runtime for parallel task execution in C++20.  
Designed for complex dependency graphs where you need predictable scheduling, fine-grained control over parallelism, and efficient memory management without the overhead of traditional task systems.

Modern runtime for parallel task execution in C++20:

* Advanced work-stealing with hierarchical stealing (L1→L2→L3) and dual priorities

* Lock-free memory allocator with per-thread arenas and QSBR reclamation

* Efficient task representation with small_function for zero-allocation closures

* Lightweight DAG execution with token-based concurrency limits and back-pressure

* High-level API: submit/then/when_all/parallel_for via TaskScope

## Design

* **Scheduler**: Per-thread work stealing queues with aging + sharded global MPMC queues
* **Memory Allocator**: Lock-free arenas with three-tier allocation (bump_ptr → local_free → remote_free)
* **Synchronization**: Optimized memory_order with atomic_wait/atomic_notify for notifications
* **Memory Safety**: QSBR-based safe memory reclamation across threads
* **Graph Safety**: Workers maintain Core references to prevent use-after-free

## Features

### Efficient Task Management
- `small_function<void(), 64>` for most tasks with minimal overhead
- Batch task submission and stealing for reduced contention
- Dual priority system (HI/LO) with automatic aging of stolen tasks

### Advanced Memory Management
- Lock-free per-thread arenas eliminate allocation contention
- Three-tier allocation strategy for optimal performance
- QSBR-based safe memory reclamation without stop-the-world pauses

### Flexible DAG Execution
- Token-based execution model with per-node concurrency limits
- Built-in back-pressure with configurable overflow policies
- Dynamic dependency resolution with efficient ready-set management

### Work Stealing Optimizations
- Cache-aware stealing hierarchy (local → socket → global)
- Affinity-aware task distribution
- Batch steal operations to amortize synchronization costs

