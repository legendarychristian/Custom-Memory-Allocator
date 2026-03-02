#include <cstddef>
#include <sys/mman.h>
#include <cassert>
#include <cstdio>

struct BlockHeader { 
    size_t size;  
    bool free; 
    BlockHeader* next; 
}; 

static constexpr size_t POOL_SIZE = 1024 * 1024;
static void* g_pool = nullptr; 
static BlockHeader* g_head = nullptr;

static inline size_t align8(size_t n) {
    return (n + 7) & ~(size_t)7;
}

static void init_pool() {
    g_pool = mmap (
        nullptr, 
        POOL_SIZE,
        PROT_READ|PROT_WRITE,
        MAP_PRIVATE|MAP_ANONYMOUS,
        -1, 0
    );

    assert(g_pool != MAP_FAILED && "mmap failed"); 

    g_head = static_cast<BlockHeader*>(g_pool); 
    g_head->size = POOL_SIZE - sizeof(BlockHeader); 
    g_head->free = true; 
    g_head->next = nullptr; 
}

void* my_malloc(size_t size) { 
    if (size == 0) return nullptr; 
    if (!g_pool) init_pool(); 

    size = align8(size); 

    BlockHeader* curr = g_head; 
    
    while (curr) { 
        if (curr->free && curr->size >= size) {
            size_t leftover = curr->size - size; 
            
            if (leftover > sizeof(BlockHeader)+8){
                auto* split      = reinterpret_cast<BlockHeader*>(
                    reinterpret_cast<char*>(curr + 1) + size
                );

                split->size = leftover - sizeof(BlockHeader);
                split->free = true; 
                split->next = curr->next; 

                curr->size = size; 
                curr->next = split; 
            }
            curr->free = false; 
            return static_cast<void*>(curr + 1); 
        }
        curr = curr->next; 
    }
    return nullptr; 
}

void my_free(void* ptr) {
    if (!ptr) return;

    // Recover the header sitting just before the user pointer
    BlockHeader* header = static_cast<BlockHeader*>(ptr) - 1;
    header->free = true;

    // Coalesce: merge with the next block if it's also free
    // (forward coalescing only — sufficient for a basic allocator)
    while (header->next && header->next->free) {
        header->size += sizeof(BlockHeader) + header->next->size;
        header->next  = header->next->next;
    }
}

void dump_heap() {
    printf("\n=== Heap Dump ===\n");
    BlockHeader* curr = g_head;
    int i = 0;
    while (curr) {
        printf("  [%d] addr=%p  size=%-6zu  %s\n",
            i++,
            static_cast<void*>(curr + 1),
            curr->size,
            curr->free ? "FREE" : "USED"
        );
        curr = curr->next;
    }
    printf("=================\n\n");
}

int main() {
    dump_heap();  // initial state: one big free block

    void* a = my_malloc(64);
    void* b = my_malloc(128);
    void* c = my_malloc(32);
    printf("Allocated: a=%p  b=%p  c=%p\n", a, b, c);
    dump_heap();

    my_free(a);
    dump_heap();  // a is free, but b is still used — no coalescing yet

    my_free(b);
    dump_heap();  // a+b should coalesce into one block

    my_free(c);
    dump_heap();  // everything merged back into one block

    return 0;
}
