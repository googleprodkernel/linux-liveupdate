.. SPDX-License-Identifier: GPL-2.0-or-later

====================
Kexec Handover Usage
====================

Kexec HandOver (KHO) is a mechanism that allows Linux to preserve state -
arbitrary properties as well as memory locations - across kexec.

This document expects that you are familiar with the base KHO
:ref:`concepts <concepts>`. If you have not read
them yet, please do so now.

Prerequisites
=============

KHO is available when the ``CONFIG_KEXEC_HANDOVER`` config option is set to y
at compile time. Every KHO producer may have its own config option that you
need to enable if you would like to preserve their respective state across
kexec.

To use KHO, please boot the kernel with the ``kho=on`` command line
parameter. You may use ``kho_scratch`` parameter to define size of the
scratch regions. For example ``kho_scratch=16M,512M,256M`` will reserve a
16 MiB low memory scratch area, a 512 MiB global scratch region, and 256 MiB
per NUMA node scratch regions on boot.

Perform a KHO kexec
===================

First, before you perform a KHO kexec, you can optionally move the system into
the :ref:`KHO finalization phase <finalization_phase>` ::

  $ echo 1 > /sys/kernel/debug/kho/out/finalize

After this command, the KHO FDT is available in
``/sys/kernel/debug/kho/out/fdt``.

Next, load the target payload and kexec into it. It is important that you
use the ``-s`` parameter to use the in-kernel kexec file loader, as user
space kexec tooling currently has no support for KHO with the user space
based file loader ::

  # kexec -l Image --initrd=initrd -s
  # kexec -e

If you skipped finalization in the first step, ``kexec -e`` triggers
FDT finalization automatically. The new kernel will boot up and contain
some of the previous kernel's state.

For example, if you used ``reserve_mem`` command line parameter to create
an early memory reservation, the new kernel will have that memory at the
same physical address as the old kernel.

Unfreeze KHO FDT data
=====================

You can move the system out of KHO finalization phase by calling ::

  $ echo 0 > /sys/kernel/debug/kho/out/finalize

After this command, the KHO FDT is no longer available in
``/sys/kernel/debug/kho/out/fdt``, and the states kept in KHO can be
modified by other kernel subsystems again.

debugfs Interfaces
==================

Currently KHO creates the following debugfs interfaces. Notice that these
interfaces may change in the future. They will be moved to sysfs once KHO is
stabilized.

``/sys/kernel/debug/kho/out/finalize``
    Kexec HandOver (KHO) allows Linux to transition the state of
    compatible drivers into the next kexec'ed kernel. To do so,
    device drivers will serialize their current state into an FDT.
    While the state is serialized, they are unable to perform
    any modifications to state that was serialized, such as
    handed over memory allocations.

    When this file contains "1", the system is in the transition
    state. When contains "0", it is not. To switch between the
    two states, echo the respective number into this file.

``/sys/kernel/debug/kho/out/fdt_max``
    KHO needs to allocate a buffer for the FDT that gets
    generated before it knows the final size. By default, it
    will allocate 10 MiB for it. You can write to this file
    to modify the size of that allocation.

``/sys/kernel/debug/kho/out/fdt``
    When KHO state tree is finalized, the kernel exposes the
    flattened device tree blob that carries its current KHO
    state in this file. Kexec user space tooling can use this
    as input file for the KHO payload image.

``/sys/kernel/debug/kho/out/scratch_len``
    To support continuous KHO kexecs, we need to reserve
    physically contiguous memory regions that will always stay
    available for future kexec allocations. This file describes
    the length of these memory regions. Kexec user space tooling
    can use this to determine where it should place its payload
    images.

``/sys/kernel/debug/kho/out/scratch_phys``
    To support continuous KHO kexecs, we need to reserve
    physically contiguous memory regions that will always stay
    available for future kexec allocations. This file describes
    the physical location of these memory regions. Kexec user space
    tooling can use this to determine where it should place its
    payload images.

``/sys/kernel/debug/kho/in/fdt``
    When the kernel was booted with Kexec HandOver (KHO),
    the state tree that carries metadata about the previous
    kernel's state is in this file in the format of flattened
    device tree. This file may disappear when all consumers of
    it finished to interpret their metadata.
