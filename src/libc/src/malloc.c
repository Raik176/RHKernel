#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define PAGE_SIZE 4096UL
#define ARENA_SIZE (64UL * 1024UL)
#define LARGE_LIMIT (32UL * 1024UL)
#define ALIGNMENT 16UL
#define MAGIC_ALLOC 0x6d616c6c6f63424cULL
#define MAGIC_FREE 0x66726565424c4b21ULL

typedef struct block_header block_header;
struct block_header {
    size_t size;
    block_header *prev;
    block_header *next;
    void *map_base;
    size_t map_size;
    uint64_t magic;
    uint8_t free;
    uint8_t large;
    uint16_t reserved;
    uint32_t pad;
    uint64_t align_pad;
};

static block_header *heap_head;

static size_t align_up(size_t v, size_t a) {
    return (v + a - 1) & ~(a - 1);
}

static int checked_add(size_t a, size_t b, size_t *out) {
    if (a > SIZE_MAX - b) return -1;
    *out = a + b;
    return 0;
}

static block_header *payload_to_block(void *ptr) {
    return (block_header *)((uint8_t *)ptr - sizeof(block_header));
}

static void *block_to_payload(block_header *b) {
    return (void *)((uint8_t *)b + sizeof(block_header));
}

static void link_block(block_header *b) {
    b->prev = 0;
    b->next = heap_head;
    if (heap_head) heap_head->prev = b;
    heap_head = b;
}

static void unlink_block(block_header *b) {
    if (b->prev) b->prev->next = b->next;
    else heap_head = b->next;
    if (b->next) b->next->prev = b->prev;
}

static block_header *new_mapping(size_t payload, int large) {
    size_t need = 0;
    if (checked_add(sizeof(block_header), payload, &need) != 0) return 0;
    size_t mapping = align_up(need, PAGE_SIZE);
    if (!large && mapping < ARENA_SIZE) mapping = ARENA_SIZE;

    void *mem = mmap(0, mapping, PROT_READ | PROT_WRITE, MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) return 0;

    block_header *b = (block_header *)mem;
    b->size = mapping - sizeof(block_header);
    b->map_base = mem;
    b->map_size = mapping;
    b->magic = MAGIC_FREE;
    b->free = 1;
    b->large = (uint8_t)large;
    b->reserved = 0;
    b->pad = 0;
    b->align_pad = 0;
    link_block(b);
    return b;
}

static void split_block(block_header *b, size_t size) {
    size_t remain = b->size - size;
    if (remain < sizeof(block_header) + ALIGNMENT) return;

    block_header *tail = (block_header *)((uint8_t *)block_to_payload(b) + size);
    tail->size = remain - sizeof(block_header);
    tail->prev = b;
    tail->next = b->next;
    tail->map_base = 0;
    tail->map_size = 0;
    tail->magic = MAGIC_FREE;
    tail->free = 1;
    tail->large = 0;
    tail->reserved = 0;
    tail->pad = 0;
    tail->align_pad = 0;
    if (tail->next) tail->next->prev = tail;
    b->next = tail;
    b->size = size;
}

static block_header *find_free(size_t size) {
    for (block_header *b = heap_head; b; b = b->next) {
        if (b->free && !b->large && b->size >= size) return b;
    }
    return 0;
}

static void merge_next(block_header *b) {
    block_header *n = b->next;
    if (!n || !n->free || n->large) return;
    uint8_t *bend = (uint8_t *)block_to_payload(b) + b->size;
    if (bend != (uint8_t *)n) return;

    b->size += sizeof(block_header) + n->size;
    b->next = n->next;
    if (b->next) b->next->prev = b;
}

void *malloc(size_t size) {
    if (size == 0) return 0;
    if (size > SIZE_MAX - ALIGNMENT + 1) return 0;
    size = align_up(size, ALIGNMENT);

    int large = size >= LARGE_LIMIT;
    block_header *b = large ? 0 : find_free(size);
    if (!b) b = new_mapping(size, large);
    if (!b) return 0;

    if (!large) split_block(b, size);
    b->free = 0;
    b->magic = MAGIC_ALLOC;
    return block_to_payload(b);
}

void free(void *ptr) {
    if (!ptr) return;
    block_header *b = payload_to_block(ptr);
    if (b->magic != MAGIC_ALLOC || b->free) abort();

    b->free = 1;
    b->magic = MAGIC_FREE;
    if (b->large) {
        void *map_base = b->map_base;
        size_t map_size = b->map_size;
        unlink_block(b);
        munmap(map_base, map_size);
        return;
    }

    merge_next(b);
    if (b->prev && b->prev->free && !b->prev->large) {
        merge_next(b->prev);
    }
}

void *calloc(size_t nmemb, size_t size) {
    if (nmemb == 0 || size == 0) return 0;
    if (size > SIZE_MAX / nmemb) return 0;
    size_t total = nmemb * size;
    void *ptr = malloc(total);
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) {
        free(ptr);
        return 0;
    }

    block_header *b = payload_to_block(ptr);
    if (b->magic != MAGIC_ALLOC || b->free) abort();
    size_t wanted = align_up(size, ALIGNMENT);
    if (b->size >= wanted) {
        if (!b->large) split_block(b, wanted);
        return ptr;
    }

    if (!b->large && b->next && b->next->free && !b->next->large) {
        uint8_t *bend = (uint8_t *)block_to_payload(b) + b->size;
        if (bend == (uint8_t *)b->next && b->size + sizeof(block_header) + b->next->size >= wanted) {
            merge_next(b);
            split_block(b, wanted);
            b->free = 0;
            b->magic = MAGIC_ALLOC;
            return ptr;
        }
    }

    void *np = malloc(size);
    if (!np) return 0;
    memcpy(np, ptr, b->size < size ? b->size : size);
    free(ptr);
    return np;
}

void *aligned_alloc(size_t alignment, size_t size) {
    if (alignment < ALIGNMENT || (alignment & (alignment - 1)) != 0) return 0;
    if (size == 0) return 0;
    if (size > SIZE_MAX - alignment - sizeof(block_header)) return 0;
    if (alignment <= ALIGNMENT) return malloc(size);

    size_t total = 0;
    if (checked_add(sizeof(block_header), size, &total) != 0 || checked_add(total, alignment, &total) != 0) return 0;
    size_t mapping = align_up(total, PAGE_SIZE);
    void *mem = mmap(0, mapping, PROT_READ | PROT_WRITE, MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) return 0;

    uintptr_t payload = ((uintptr_t)mem + sizeof(block_header) + alignment - 1) & ~(uintptr_t)(alignment - 1);
    block_header *b = (block_header *)(payload - sizeof(block_header));
    b->size = size;
    b->map_base = mem;
    b->map_size = mapping;
    b->magic = MAGIC_ALLOC;
    b->free = 0;
    b->large = 1;
    b->reserved = 0;
    b->pad = 0;
    b->align_pad = 0;
    link_block(b);
    return (void *)payload;
}
