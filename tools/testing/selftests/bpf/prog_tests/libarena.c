// SPDX-License-Identifier: LGPL-2.1 OR BSD-2-Clause
/* Copyright (c) 2026 Meta Platforms, Inc. and affiliates. */
#include <test_progs.h>
#include <unistd.h>

#define __arena
typedef uint64_t u64;
typedef uint8_t u8;

#include "libarena/include/common.h"
#include "libarena/include/asan.h"
#include "libarena/include/selftest_helpers.h"

#include "libarena/libarena.skel.h"

static void test_libarena_buddy(void)
{
	struct libarena *skel;
	int ret;

	skel = libarena__open_and_load();
	if (!ASSERT_OK_PTR(skel, "open_and_load"))
		return;

	ret = libarena__attach(skel);
	if (!ASSERT_OK(ret, "attach"))
		goto out;

	ret = libarena_run_prog(bpf_program__fd(skel->progs.arena_alloc_reserve));
	if (!ASSERT_OK(ret, "arena_alloc_reserve"))
		goto out;

	ret = libarena_run_prog(bpf_program__fd(skel->progs.test_buddy));
	ASSERT_OK(ret, "test_buddy");

out:
	libarena__destroy(skel);
}

static void test_libarena_minheap(void)
{
	struct libarena *skel;
	int ret;

	skel = libarena__open_and_load();
	if (!ASSERT_OK_PTR(skel, "open_and_load"))
		return;

	ret = libarena__attach(skel);
	if (!ASSERT_OK(ret, "attach"))
		goto out;

	ret = libarena_run_prog(bpf_program__fd(skel->progs.arena_alloc_reserve));
	if (!ASSERT_OK(ret, "arena_alloc_reserve"))
		goto out;

	ret = libarena_run_prog(bpf_program__fd(skel->progs.arena_alloc_init));
	if (!ASSERT_OK(ret, "arena_alloc_init"))
		goto out;

	ret = libarena_run_prog(bpf_program__fd(skel->progs.test_minheap));
	ASSERT_OK(ret, "test_minheap");

	libarena_run_prog(bpf_program__fd(skel->progs.arena_alloc_fini));
out:
	libarena__destroy(skel);
}


static void test_libarena_rbtree(void)
{
	struct libarena *skel;
	int ret;

	skel = libarena__open_and_load();
	if (!ASSERT_OK_PTR(skel, "open_and_load"))
		return;

	ret = libarena__attach(skel);
	if (!ASSERT_OK(ret, "attach"))
		goto out;

	ret = libarena_run_prog(bpf_program__fd(skel->progs.arena_alloc_reserve));
	if (!ASSERT_OK(ret, "arena_alloc_reserve"))
		goto out;

	ret = libarena_run_prog(bpf_program__fd(skel->progs.arena_alloc_init));
	if (!ASSERT_OK(ret, "arena_alloc_init"))
		goto out;

	ret = libarena_run_prog(bpf_program__fd(skel->progs.test_rbtree));
	ASSERT_OK(ret, "test_rbtree");

	libarena_run_prog(bpf_program__fd(skel->progs.arena_alloc_fini));
out:
	libarena__destroy(skel);
}


static void test_libarena_btree(void)
{
	struct libarena *skel;
	int ret;

	skel = libarena__open_and_load();
	if (!ASSERT_OK_PTR(skel, "open_and_load"))
		return;

	ret = libarena__attach(skel);
	if (!ASSERT_OK(ret, "attach"))
		goto out;

	ret = libarena_run_prog(bpf_program__fd(skel->progs.arena_alloc_reserve));
	if (!ASSERT_OK(ret, "arena_alloc_reserve"))
		goto out;

	ret = libarena_run_prog(bpf_program__fd(skel->progs.arena_alloc_init));
	if (!ASSERT_OK(ret, "arena_alloc_init"))
		goto out;

	ret = libarena_run_prog(bpf_program__fd(skel->progs.test_btree));
	ASSERT_OK(ret, "test_btree");

	libarena_run_prog(bpf_program__fd(skel->progs.arena_alloc_fini));
out:
	libarena__destroy(skel);
}


static void test_libarena_lvqueue(void)
{
	struct libarena *skel;
	int ret;

	skel = libarena__open_and_load();
	if (!ASSERT_OK_PTR(skel, "open_and_load"))
		return;

	ret = libarena__attach(skel);
	if (!ASSERT_OK(ret, "attach"))
		goto out;

	ret = libarena_run_prog(bpf_program__fd(skel->progs.arena_alloc_reserve));
	if (!ASSERT_OK(ret, "arena_alloc_reserve"))
		goto out;

	ret = libarena_run_prog(bpf_program__fd(skel->progs.arena_alloc_init));
	if (!ASSERT_OK(ret, "arena_alloc_init"))
		goto out;

	ret = libarena_run_prog(bpf_program__fd(skel->progs.test_lvqueue));
	ASSERT_OK(ret, "test_lvqueue");

	libarena_run_prog(bpf_program__fd(skel->progs.arena_alloc_fini));
out:
	libarena__destroy(skel);
}


static void test_libarena_bitmap(void)
{
	struct libarena *skel;
	int ret;

	skel = libarena__open_and_load();
	if (!ASSERT_OK_PTR(skel, "open_and_load"))
		return;

	ret = libarena__attach(skel);
	if (!ASSERT_OK(ret, "attach"))
		goto out;

	ret = libarena_run_prog(bpf_program__fd(skel->progs.arena_alloc_reserve));
	if (!ASSERT_OK(ret, "arena_alloc_reserve"))
		goto out;

	ret = libarena_run_prog(bpf_program__fd(skel->progs.arena_alloc_init));
	if (!ASSERT_OK(ret, "arena_alloc_init"))
		goto out;

	ret = libarena_run_prog(bpf_program__fd(skel->progs.test_bitmap));
	ASSERT_OK(ret, "test_bitmap");

	libarena_run_prog(bpf_program__fd(skel->progs.arena_alloc_fini));
out:
	libarena__destroy(skel);
}


void test_libarena(void)
{
	if (test__start_subtest("buddy"))
		test_libarena_buddy();
	if (test__start_subtest("minheap"))
		test_libarena_minheap();
	if (test__start_subtest("rbtree"))
		test_libarena_rbtree();
	if (test__start_subtest("btree"))
		test_libarena_btree();
	if (test__start_subtest("lvqueue"))
		test_libarena_lvqueue();
	if (test__start_subtest("bitmap"))
		test_libarena_bitmap();
}
