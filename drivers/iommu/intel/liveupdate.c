// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025, Google LLC
 * Author: Samiullah Khawaja <skhawaja@google.com>
 */

#define pr_fmt(fmt)    "iommu: liveupdate: " fmt

#include <linux/liveupdate.h>
#include <linux/module.h>

static int intel_liveupdate_prepare(struct liveupdate_subsystem *handle, u64 *data)
{
	pr_warn("Not implemented\n");
	return 0;
}

static void intel_liveupdate_cancel(struct liveupdate_subsystem *handle, u64 data)
{
	pr_warn("Not implemented\n");
}

static void intel_liveupdate_finish(struct liveupdate_subsystem *handle, u64 data)
{
	pr_warn("Not implemented\n");
}

static struct liveupdate_subsystem_ops intel_liveupdate_subsystem_ops = {
	.prepare = intel_liveupdate_prepare,
	.finish = intel_liveupdate_finish,
	.cancel = intel_liveupdate_cancel,
};

static struct liveupdate_subsystem intel_liveupdate_subsystem = {
	.name = "intel-iommu",
	.ops = &intel_liveupdate_subsystem_ops,
};

static int __init intel_liveupdate_init(void)
{
	WARN_ON_ONCE(liveupdate_register_subsystem(&intel_liveupdate_subsystem));
	return 0;
}

late_initcall(intel_liveupdate_init);
