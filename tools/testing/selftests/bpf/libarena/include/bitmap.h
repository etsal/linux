#pragma once

#define BITMAP_NLONG (512 / 8)

struct arena_bitmap {
	u64 bits[BITMAP_NLONG];
};

typedef struct arena_bitmap __arena * __arg_arena bitmap_t;

/* Mask size in 64-bit words. */
extern size_t mask_size;

#ifdef __BPF__

int bitmap_init(__u64 total_mask_size);
u64 bitmap_alloc_internal(void);
#define bitmap_alloc() ((bitmap_t)bitmap_alloc_internal())
int bitmap_free(bitmap_t __arg_arena mask);

int bitmap_set_cpu(u32 cpu, bitmap_t __arg_arena mask);
int bitmap_clear_cpu(u32 cpu, bitmap_t __arg_arena mask);
bool bitmap_test_cpu(u32 cpu, bitmap_t __arg_arena mask);
bool bitmap_test_and_clear_cpu(u32 cpu, bitmap_t __arg_arena mask);

int bitmap_clear(bitmap_t __arg_arena mask);
int bitmap_and(bitmap_t __arg_arena dst, bitmap_t __arg_arena src1, bitmap_t __arg_arena src2);
int bitmap_or(bitmap_t __arg_arena dst, bitmap_t __arg_arena src1, bitmap_t __arg_arena src2);
bool bitmap_empty(bitmap_t __arg_arena mask);
int bitmap_copy(bitmap_t __arg_arena dst, bitmap_t __arg_arena src);

int bitmap_from_cpumask(bitmap_t __arg_arena bmp, const cpumask_t *bpfmask __arg_trusted);

bool bitmap_intersects(bitmap_t __arg_arena arg1, bitmap_t __arg_arena arg2);
bool bitmap_subset(bitmap_t __arg_arena big, bitmap_t __arg_arena small);
int bitmap_print(bitmap_t __arg_arena mask);

#endif /* __BPF__ */
