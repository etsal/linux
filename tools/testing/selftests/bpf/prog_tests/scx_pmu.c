// SPDX-License-Identifier: GPL-2.0
#include <test_progs.h>
#include "scx_pmu.skel.h"

void test_scx_pmu(void)
{
	struct scx_pmu *skel;

	skel = scx_pmu__open_and_load();
	if (!ASSERT_OK_PTR(skel, "open_and_load"))
		return;

	scx_pmu__destroy(skel);
}
