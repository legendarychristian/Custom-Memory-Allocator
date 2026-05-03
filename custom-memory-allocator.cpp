#include <cstddef>
#include <sys/mman.h>
#include <cstdio>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <cstring>

static constexpr size_t kSizeClasses[] = {
    8, 16, 24, 32, 48, 64, 80, 96, 112, 128,
    160, 192, 224, 256, 320, 384, 448, 512,
    640, 768, 896, 1024, 2048, 4096, 8192, 16384, 32768, 65536, 131072, 262144
};
static constexpr int    kNumClasses    = sizeof(kSizeClasses) / sizeof(kSizeClasses[0]);
static constexpr size_t kMaxSmall      = 262144;
static constexpr size_t kSlabSize      = 65536;
static constexpr size_t byteBuffer     = 8;
static constexpr int    kMaxCacheCount = 256; // max slots a thread cache holds before returning excess
static constexpr int    kBatchSize     = 32;  // slots fetched or returned per batch

struct LargeHeader { size_t mapping_size; };
struct FreeNode    { FreeNode* next; };

static int size_to_class(size_t size) {
    for (int i = 0; i < kNumClasses; i++)
        if (size <= kSizeClasses[i]) return i;
    return -1;
}

struct CentralHeap {
    std::mutex mtx[kNumClasses];        // one lock per size class
    FreeNode*  free_lists[kNumClasses];

    CentralHeap() {
        for (int i = 0; i < kNumClasses; i++)
            free_lists[i] = nullptr;
    }

    FreeNode* alloc_slab(size_t obj_size) {
        if (obj_size < sizeof(FreeNode)) obj_size = sizeof(FreeNode);
        void* mem = mmap(nullptr, kSlabSize,
                         PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mem == MAP_FAILED) return nullptr;
        size_t    n    = kSlabSize / obj_size;
        FreeNode* head = nullptr;
        char*     p    = static_cast<char*>(mem);
        for (size_t i = 0; i < n; i++) {
            auto* node = reinterpret_cast<FreeNode*>(p + i * obj_size);
            node->next = head;
            head       = node;
        }
        // printf("  [CentralHeap] carved new slab: %zu slots of %zu bytes\n", n, obj_size);
        return head;
    }

    FreeNode* fetch_batch(int cls, int count) {
        std::lock_guard<std::mutex> lk(mtx[cls]);

        // replenish from OS if empty
        if (!free_lists[cls])
            free_lists[cls] = alloc_slab(kSizeClasses[cls]);
        if (!free_lists[cls]) return nullptr;

        // detach 'count' slots from the front of the central list
        FreeNode* batch_head = free_lists[cls];
        FreeNode* curr       = free_lists[cls];
        int taken = 1;
        while (taken < count && curr->next) { curr = curr->next; taken++; }
        free_lists[cls] = curr->next;
        curr->next      = nullptr;

        printf("  [CentralHeap] handed %d slots of size %zu to thread cache\n",
               taken, kSizeClasses[cls]);
        return batch_head;
    }

    void return_batch(FreeNode*& cache_list, int cls, int count) {
        std::lock_guard<std::mutex> lk(mtx[cls]);

        // walk to the end of the batch being returned
        FreeNode* curr = cache_list;
        int walked = 1;
        while (walked < count && curr->next) { curr = curr->next; walked++; }

        // detach from thread cache and prepend onto central list
        FreeNode* to_return = cache_list;
        cache_list          = curr->next;
        curr->next          = free_lists[cls];
        free_lists[cls]     = to_return;

        printf("  [CentralHeap] received %d slots of size %zu back from thread cache\n",
               walked, kSizeClasses[cls]);
    }
};

static CentralHeap g_central;

struct ThreadCache {
    // Array of pointers - each index points to the head of a LinkedList
    FreeNode* linked_lists[kNumClasses] = {};
    int       counts[kNumClasses]       = {};

    void* allocate(int size_class) {
        printf("\n  Step 2.a: Entering Thread Local Cache\n");

        // 1. If the LinkedList is empty, fetch a batch from the central heap
        if (!linked_lists[size_class]) {
            printf("\n  Step 2.b: LinkedList is empty -> Fetching batch from central heap\n");
            linked_lists[size_class] = g_central.fetch_batch(size_class, kBatchSize);
            counts[size_class]       = kBatchSize;
            if (!linked_lists[size_class]) return nullptr;
        }

        // 2. Get pointer to head of the LinkedList
        FreeNode* node = linked_lists[size_class];
        printf("\n  Step 2.c: Accessing LinkedList for size class [%zu] at address: [%p]\n",
               kSizeClasses[size_class], node);

        // 3. Pop from the head of the LinkedList and bring 'next' to the head
        linked_lists[size_class] = node->next;
        counts[size_class]--;

        printf("\n  Step 2.d: Pop head - this is where our variable will be stored: [%p]\n", node);
        printf("\n  Step 2.e: Set new head of LinkedList: [%p]\n", linked_lists[size_class]);

        // 4. Return the address of the allocated memory
        return node;
    }

    void deallocate(void* ptr, int size_class) {
        printf("\n  Step 5.a: [ThreadCache] Deallocate memory for size class: %zu\n",
               kSizeClasses[size_class]);

        // 1. Treat pointer as a FreeNode object (LinkedList node)
        auto* node = static_cast<FreeNode*>(ptr);

        // 2. Point the current node to the front of the respective LinkedList
        node->next = linked_lists[size_class];

        // 3. Push the node into the front of the LinkedList
        linked_lists[size_class] = node;
        counts[size_class]++;

        printf("\n  Step 5.b: ThreadCache returned slot to size class %zu\n",
               kSizeClasses[size_class]);

        // 4. If we're holding too many slots, return a batch to the central heap
        if (counts[size_class] > kMaxCacheCount) {
            printf("\n  Step 5.c: Cache over capacity -> Returning batch to central heap\n");
            g_central.return_batch(linked_lists[size_class], size_class, kBatchSize);
            counts[size_class] -= kBatchSize;
        }
    }
};

static thread_local ThreadCache local_thread_cache;

void* allocate_large_variable(size_t size) {
    printf("  [my_malloc] large allocation — going direct to mmap\n");

    // 1. Measure total memory needed (header + variable)
    size_t total = sizeof(LargeHeader) + size;

    // 2. Request OS for memory
    void* memory = mmap(nullptr, total,
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) return nullptr;

    // 3. Treat the received bytes as a LargeHeader object
    auto* header = static_cast<LargeHeader*>(memory);

    // 4. Store size of mapping in header
    header->mapping_size = total;

    // 5. Return memory address of the variable (skip past header)
    return header + 1;
}

void* my_malloc(size_t size) {
    printf("\n  Step 1.a: Allocating memory with 'my_malloc()'\n");

    if (size == 0) return nullptr;

    // Non-Optimal Case: too large for thread local cache, go straight to mmap
    if (size > kMaxSmall) return allocate_large_variable(size);

    // Optimal Case: store inside thread local cache

    // 1. Identify size class
    int size_class = size_to_class(size + byteBuffer);
    if (size_class < 0) size_class = kNumClasses - 1;
    printf("\n  Step 1.b: 'my_malloc()' allocating %zu bytes → size class %zu\n",
           size, kSizeClasses[size_class]);

    // 2. Allocate memory from thread local cache
    void* memory_address = local_thread_cache.allocate(size_class);
    if (!memory_address) return nullptr;

    // 3. Store list index in the first byte (used by my_free to find the size class)
    auto* linked_list_idx = static_cast<uint8_t*>(memory_address);
    *linked_list_idx = static_cast<uint8_t>(size_class);
    printf("\n  Step 3.a: start of memory: %p, start of usable memory: %p\n",
           memory_address, linked_list_idx + byteBuffer);

    // 4. Return address of USABLE data (skip past the metadata byte)
    return linked_list_idx + byteBuffer;
}

void my_free(void* ptr) {
    if (!ptr) return;

    // 1. Get the memory address of the pointer
    uint8_t* memory_address = static_cast<uint8_t*>(ptr);

    // 2. Use pointer arithmetic to read the size class stored in the metadata byte
    uint8_t size_class = *(memory_address - byteBuffer);

    // 3. Deallocate back to thread local cache (small allocation)
    if (size_class < kNumClasses) {
        printf("\n  Step 4.a: usable memory address: %p\n", memory_address);
        printf("\n  Step 4.b: [my_free] returning slot → size class %zu\n",
               kSizeClasses[size_class]);
        local_thread_cache.deallocate(memory_address - byteBuffer, size_class);

    // 4. Deallocate large allocation directly back to OS
    } else {
        auto* header = reinterpret_cast<LargeHeader*>(memory_address) - 1;
        printf("\n[my_free] large allocation — munmap\n");
        munmap(header, header->mapping_size);
    }
}

// Benchmark infrastructure

using Clock = std::chrono::steady_clock;
using ns    = std::chrono::nanoseconds;

static constexpr size_t kTestSizes[]  = { 16, 32, 64, 128, 256, 512 };
static constexpr int    kNumSizes     = sizeof(kTestSizes) / sizeof(kTestSizes[0]);
static constexpr int    kOpsPerThread = 100'000;
static constexpr int    kLatencyBatch = 100;

struct ScalabilityResult {
    int    num_threads;
    double allocs_per_sec;
};

void scalability_worker(int ops, std::atomic<bool>& start_flag) {
    while (!start_flag.load(std::memory_order_acquire));
    for (int i = 0; i < ops; i++) {
        size_t sz  = kTestSizes[i % kNumSizes];
        void*  ptr = my_malloc(sz);
        if (ptr) {
            memset(ptr, 1, sz);
            asm volatile("" : : "r"(ptr) : "memory");
        }
        my_free(ptr);
    }
}

ScalabilityResult run_scalability(int num_threads) {
    std::atomic<bool>        start_flag{false};
    std::vector<std::thread> threads;
    threads.reserve(num_threads);
    for (int t = 0; t < num_threads; t++)
        threads.emplace_back(scalability_worker, kOpsPerThread, std::ref(start_flag));
    auto t0 = Clock::now();
    start_flag.store(true, std::memory_order_release);
    for (auto& th : threads) th.join();
    auto t1      = Clock::now();
    double elapsed   = std::chrono::duration<double>(t1 - t0).count();
    double total_ops = static_cast<double>(num_threads) * kOpsPerThread;
    return { num_threads, total_ops / elapsed };
}

void scalability_worker_baseline(int ops, std::atomic<bool>& start_flag) {
    while (!start_flag.load(std::memory_order_acquire));
    for (int i = 0; i < ops; i++) {
        size_t sz  = kTestSizes[i % kNumSizes];
        void*  ptr = malloc(sz);
        if (ptr) {
            memset(ptr, 1, sz);
            asm volatile("" : : "r"(ptr) : "memory");
            free(ptr);
        }
    }
}

ScalabilityResult run_scalability_baseline(int num_threads) {
    std::atomic<bool>        start_flag{false};
    std::vector<std::thread> threads;
    threads.reserve(num_threads);
    for (int t = 0; t < num_threads; t++)
        threads.emplace_back(scalability_worker_baseline,
                             kOpsPerThread, std::ref(start_flag));
    auto t0 = Clock::now();
    start_flag.store(true, std::memory_order_release);
    for (auto& th : threads) th.join();
    auto t1      = Clock::now();
    double elapsed   = std::chrono::duration<double>(t1 - t0).count();
    double total_ops = static_cast<double>(num_threads) * kOpsPerThread;
    return { num_threads, total_ops / elapsed };
}

struct LatencyResult {
    int    num_threads;
    double p50_ns;
    double p99_ns;
    double p999_ns;
};

void latency_worker(int ops,
                    std::atomic<bool>& start_flag,
                    std::vector<double>& out_samples) {
    out_samples.reserve(ops / kLatencyBatch);
    while (!start_flag.load(std::memory_order_acquire));
    void* ptrs[kLatencyBatch];
    int i = 0;
    while (i + kLatencyBatch <= ops) {
        size_t sz = kTestSizes[(i / kLatencyBatch) % kNumSizes];

        // time a batch of allocations to amortize clock overhead
        auto t0 = Clock::now();
        for (int j = 0; j < kLatencyBatch; j++) {
            ptrs[j] = my_malloc(sz);
            asm volatile("" : : "r"(ptrs[j]) : "memory");
        }
        auto t1 = Clock::now();

        // record average ns per allocation for this batch
        double batch_ns = static_cast<double>(
            std::chrono::duration_cast<ns>(t1 - t0).count());
        out_samples.push_back(batch_ns / kLatencyBatch);

        for (int j = 0; j < kLatencyBatch; j++) my_free(ptrs[j]);
        i += kLatencyBatch;
    }
}

LatencyResult run_latency(int num_threads) {
    std::atomic<bool>                start_flag{false};
    std::vector<std::thread>         threads;
    std::vector<std::vector<double>> per_thread(num_threads);
    threads.reserve(num_threads);
    for (int t = 0; t < num_threads; t++)
        threads.emplace_back(latency_worker, kOpsPerThread,
                             std::ref(start_flag), std::ref(per_thread[t]));
    start_flag.store(true, std::memory_order_release);
    for (auto& th : threads) th.join();

    // merge per-thread samples and sort to get percentiles
    std::vector<double> all;
    all.reserve(num_threads * (kOpsPerThread / kLatencyBatch));
    for (auto& v : per_thread)
        all.insert(all.end(), v.begin(), v.end());
    std::sort(all.begin(), all.end());
    size_t n = all.size();
    return { num_threads, all[n*50/100], all[n*99/100], all[n*999/1000] };
}

void latency_worker_baseline(int ops,
                             std::atomic<bool>& start_flag,
                             std::vector<double>& out_samples) {
    out_samples.reserve(ops / kLatencyBatch);
    while (!start_flag.load(std::memory_order_acquire));
    void* ptrs[kLatencyBatch];
    int i = 0;
    while (i + kLatencyBatch <= ops) {
        size_t sz = kTestSizes[(i / kLatencyBatch) % kNumSizes];
        auto t0 = Clock::now();
        for (int j = 0; j < kLatencyBatch; j++) {
            ptrs[j] = malloc(sz);
            asm volatile("" : : "r"(ptrs[j]) : "memory");
        }
        auto t1 = Clock::now();
        double batch_ns = static_cast<double>(
            std::chrono::duration_cast<ns>(t1 - t0).count());
        out_samples.push_back(batch_ns / kLatencyBatch);
        for (int j = 0; j < kLatencyBatch; j++) free(ptrs[j]);
        i += kLatencyBatch;
    }
}

LatencyResult run_latency_baseline(int num_threads) {
    std::atomic<bool>                start_flag{false};
    std::vector<std::thread>         threads;
    std::vector<std::vector<double>> per_thread(num_threads);
    threads.reserve(num_threads);
    for (int t = 0; t < num_threads; t++)
        threads.emplace_back(latency_worker_baseline, kOpsPerThread,
                             std::ref(start_flag), std::ref(per_thread[t]));
    start_flag.store(true, std::memory_order_release);
    for (auto& th : threads) th.join();
    std::vector<double> all;
    all.reserve(num_threads * (kOpsPerThread / kLatencyBatch));
    for (auto& v : per_thread)
        all.insert(all.end(), v.begin(), v.end());
    std::sort(all.begin(), all.end());
    size_t n = all.size();
    return { num_threads, all[n*50/100], all[n*99/100], all[n*999/1000] };
}

int main() {
    int max_threads = static_cast<int>(std::thread::hardware_concurrency());
    if (max_threads == 0) max_threads = 8;

    printf("Cores detected: %d\n", max_threads);
    printf("Ops per thread: %d\n", kOpsPerThread);
    printf("Latency batch:  %d allocations per sample\n", kLatencyBatch);
    printf("Cache cap:      %d slots | Batch size: %d slots\n\n",
           kMaxCacheCount, kBatchSize);

    // Scalability
    printf("=== SCALABILITY (allocations/sec) ===\n");
    printf("%-10s  %-20s  %-20s  %-10s\n",
           "Threads", "Custom (alloc/s)", "Baseline (alloc/s)", "Speedup");
    printf("%-10s  %-20s  %-20s  %-10s\n",
           "-------", "----------------", "------------------", "-------");
    for (int t = 1; t <= max_threads; t++) {
        auto custom   = run_scalability(t);
        auto baseline = run_scalability_baseline(t);
        printf("%-10d  %-20.0f  %-20.0f  %-10.2fx\n",
               t, custom.allocs_per_sec, baseline.allocs_per_sec,
               custom.allocs_per_sec / baseline.allocs_per_sec);
    }

    // Latency
    printf("\n=== LATENCY (nanoseconds per allocation, batched) ===\n");
    printf("%-10s  %-8s  %-8s  %-8s  |  %-8s  %-8s  %-10s\n",
           "Threads", "p50", "p99", "p99.9", "base p50", "base p99", "base p99.9");
    printf("%-10s  %-8s  %-8s  %-8s  |  %-8s  %-8s  %-10s\n",
           "-------", "---", "---", "-----", "--------", "--------", "----------");
    for (int t = 1; t <= max_threads; t++) {
        auto custom   = run_latency(t);
        auto baseline = run_latency_baseline(t);
        printf("%-10d  %-8.1f  %-8.1f  %-8.1f  |  %-8.1f  %-8.1f  %-10.1f\n",
               t, custom.p50_ns, custom.p99_ns, custom.p999_ns,
               baseline.p50_ns, baseline.p99_ns, baseline.p999_ns);
    }

    return 0;
}