/* SPDX-License-Identifier: LGPL-2.1 OR BSD-2-Clause */

#pragma once

struct spmc_arr;

#define SPMC_ARR_BASESZ 128
#define SPMC_ARR_ORDERS 10

struct spmc_arr {
	u64 __arena *data;
	u64 order;
};

typedef volatile struct spmc_arr __arena *spmc_arr_t;

struct spmc {
	spmc_arr_t cur;
	volatile u64 top;
	volatile u64 bottom;
	struct spmc_arr arr[SPMC_ARR_ORDERS];
};

typedef struct spmc __arena *spmc_t;

int spmc_owned_add(spmc_t spmc, u64 val);
int spmc_owned_remove(spmc_t spmc, u64 *val);
int spmc_steal(spmc_t spmc, u64 *val);

spmc_t spmc_create(void);
int spmc_destroy(spmc_t spmc);
