# my_malloc

A custom memory allocator built in C++ with a thread-local cache and a central heap, with built-in benchmarks comparing against system `malloc`.

## How it works

Small allocations (≤ 262144 bytes) are served from a **thread-local cache** — a set of per-size-class linked lists. When a list runs dry, the cache fetches a batch of slots from the **central heap**, which carves 64 KB slabs from the OS via `mmap`. When a cache grows too large, it returns excess slots back to the central heap. Large allocations bypass the cache entirely and go straight to `mmap`, tracked with a small header prepended to the allocation.

```
my_malloc(size)
  ├── small → thread-local cache → central heap → mmap (slab)
  └── large → mmap directly

my_free(ptr)
  ├── small → thread-local cache → (drain if over cap) → central heap
  └── large → munmap
```

## Size classes

Allocations are rounded up to the nearest size class. There are 30 classes ranging from 8 to 262144 bytes. Each class has its own linked list in the thread cache and its own lock in the central heap, so threads needing different size classes never contend with each other.

## Metadata

Each small allocation stores its size class index in the first 8 bytes of the raw slot. `my_malloc` returns a pointer 8 bytes past this, and `my_free` steps back to read it. This is how free knows which linked list to return the slot to without any external bookkeeping.

## Batching

Rather than fetching or returning one slot at a time, the thread cache operates in batches of 32. This keeps lock hold time short and reduces how often threads need to touch the central heap. If a thread cache accumulates more than 256 slots for a given size class, it proactively returns a batch — preventing unbounded memory hoarding across threads.

## Build & run

```bash
g++ -O2 -std=c++17 -pthread custom-memory-allocator.cpp -o allocator && ./allocator
```

The benchmark runs scalability (total allocations/sec across 1–N threads) and latency (p50/p99/p99.9 ns per allocation) tests for both `my_malloc` and system `malloc`, printing a side-by-side comparison.

## Key constants

| Constant | Default | Description |
|----------|---------|-------------|
| `kBatchSize` | 32 | Slots fetched/returned per central heap trip |
| `kMaxCacheCount` | 256 | Max slots a thread cache holds before draining |
| `kSlabSize` | 65536 | Bytes per slab carved from OS |
| `kMaxSmall` | 262144 | Max size served by thread cache; larger goes to mmap |
