// SPDX-License-Identifier: LGPL-2.1 OR BSD-2-Clause

#include <libarena/common.h>

#include <libarena/asan.h>
#include <libarena/spmc.h>

#include "test_parallel_spmc.h"

static spmc_t spmc[TEST_SPMC_THREADS];

SEC("syscall")
__weak int parallel_test_spmc_init(void)
{
	int i, j;

	arena_subprog_init();

	for (i = 0; i < TEST_SPMC_THREADS && can_loop; i++) {
		spmc[i] = spmc_create();
		if (!spmc[i])
			goto error;
	}

	return 0;

error:
	for (j = 0; j < i && can_loop; j++)
		spmc_destroy(spmc[i]);

	return -ENOMEM;

}

SEC("syscall")
__weak int parallel_test_spmc_add(struct parallel_test_spmc_val_args *args)
{
	u64 id = args->id;

	arena_subprog_init();

	if (id >= TEST_SPMC_THREADS)
		return -EINVAL;

	return spmc_owned_add(spmc[id], args->val);
}

SEC("syscall")
__weak int parallel_test_spmc_remove(struct parallel_test_spmc_val_args *args)
{
	u64 id = args->id;

	arena_subprog_init();

	if (id >= TEST_SPMC_THREADS)
		return -EINVAL;

	return spmc_owned_remove(spmc[id], &args->val);
}

SEC("syscall")
__weak int parallel_test_spmc_steal(struct parallel_test_spmc_val_args *args)
{
	u64 id = args->id;

	arena_subprog_init();

	if (id >= TEST_SPMC_THREADS)
		return -EINVAL;

	return spmc_steal(spmc[id], &args->val);
}

SEC("syscall")
__weak int parallel_test_spmc_destroy(void)
{
	int i;

	arena_subprog_init();

	for (i = 0; i < TEST_SPMC_THREADS && can_loop; i++)
		spmc_destroy(spmc[i]);

	return 0;
}
