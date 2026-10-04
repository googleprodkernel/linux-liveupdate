/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2026, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 *
 * Force-included into every preserved-runtime object, after the kernel
 * configuration.  The runtime keeps running outside the kernel that built it,
 * across kexec, so it must not emit bug table entries, irq-flag tracing calls,
 * tracepoints or kCFI type references: build it as if these were not
 * configured.  BUG() is then a bare trap, which the runtime's exception
 * handler records, and WARN() only evaluates its condition.
 */
#ifndef _LINUX_CPU_PRESERVE_COMPAT_H
#define _LINUX_CPU_PRESERVE_COMPAT_H

#undef CONFIG_BUG
#undef CONFIG_GENERIC_BUG
#undef CONFIG_TRACE_IRQFLAGS
#undef CONFIG_TRACEPOINTS

/* C code keeps CONFIG_CFI, which changes the layout of shared structures */
#ifdef __ASSEMBLY__
#undef CONFIG_CFI
#endif

#endif /* _LINUX_CPU_PRESERVE_COMPAT_H */
