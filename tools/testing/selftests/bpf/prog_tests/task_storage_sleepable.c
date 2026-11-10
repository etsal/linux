// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026 Meta Platforms, Inc. and affiliates. */

#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <test_progs.h>
#include "task_storage_sleepable.skel.h"

static void *run_storage(void *arg)
{
	struct task_storage_sleepable *skel = arg;

	/* A new thread has no task storage from earlier tests. */
	skel->bss->target_pid = syscall(SYS_gettid);
	syscall(SYS_getpgid, 0);
	skel->bss->target_pid = 0;
	return NULL;
}

static void test_alloc(const char *prog_name, int regular_allocs, int nolock_allocs)
{
	struct task_storage_sleepable *skel;
	struct bpf_link *link = NULL;
	struct bpf_program *prog;
	pthread_t thread;
	int err;

	skel = task_storage_sleepable__open_and_load();
	if (!ASSERT_OK_PTR(skel, "open_and_load"))
		return;

	skel->links.on_regular_alloc = bpf_program__attach(skel->progs.on_regular_alloc);
	if (!ASSERT_OK_PTR(skel->links.on_regular_alloc, "attach_regular_alloc"))
		goto out;
	skel->links.on_nolock_alloc = bpf_program__attach(skel->progs.on_nolock_alloc);
	if (!ASSERT_OK_PTR(skel->links.on_nolock_alloc, "attach_nolock_alloc"))
		goto out;

	prog = bpf_object__find_program_by_name(skel->obj, prog_name);
	if (!ASSERT_OK_PTR(prog, "find_program"))
		goto out;
	link = bpf_program__attach(prog);
	if (!ASSERT_OK_PTR(link, "attach_program"))
		goto out;

	err = pthread_create(&thread, NULL, run_storage, skel);
	if (!ASSERT_OK(err, "pthread_create"))
		goto out;
	err = pthread_join(thread, NULL);
	ASSERT_OK(err, "pthread_join");
	ASSERT_EQ(skel->bss->errors, 0, "storage_errors");
	ASSERT_EQ(skel->bss->regular_allocs, regular_allocs, "regular_allocs");
	ASSERT_EQ(skel->bss->nolock_allocs, nolock_allocs, "nolock_allocs");
out:
	bpf_link__destroy(link);
	task_storage_sleepable__destroy(skel);
}

void test_task_storage_sleepable(void)
{
	if (test__start_subtest("sleepable"))
		test_alloc("sleepable", 3, 0);
	if (test__start_subtest("non_sleepable"))
		test_alloc("non_sleepable", 0, 3);
	if (test__start_subtest("sleepable_rcu"))
		test_alloc("sleepable_rcu", 0, 3);
	if (test__start_subtest("mixed_contexts"))
		test_alloc("mixed_contexts", 0, 4);
}
