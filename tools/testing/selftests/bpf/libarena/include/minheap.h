#pragma once

struct minheap_elem {
	u64 elem;
	u64 weight;
};

struct minheap {
	u64				size;
	u64				capacity;
	struct minheap_elem		__arena *helems;
};

typedef struct minheap __arena minheap_t;

#ifdef __BPF__

u64 minheap_alloc_internal(size_t capacity);
#define minheap_alloc(capacity) (minheap_t *)minheap_alloc_internal(capacity)

int minheap_balance_top_down(void __arena *heap_ptr __arg_arena);
int minheap_insert(void __arena *heap_ptr __arg_arena, u64 elem, u64 weight);
int minheap_dump(minheap_t *heap __arg_arena);
int minheap_pop(void __arena *heap_ptr __arg_arena, struct minheap_elem *helem __arg_trusted);

#endif /* __BPF__ */
