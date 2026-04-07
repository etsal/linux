/*
 * SPDX-License-Identifier: GPL-2.0
 * Copyright (c) 2025-2026 Meta Platforms, Inc. and affiliates.
 */

#include <common.h>

#include <asan.h>
#include <buddy.h>
#include <bitmap.h>

#include "selftest.h"

/* TODO: Implement bitmap selftests. */

SEC("syscall")
__weak
int test_bitmap(void)
{
	return 0;
}
