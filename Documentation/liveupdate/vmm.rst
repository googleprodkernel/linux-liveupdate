.. SPDX-License-Identifier: GPL-2.0-or-later

=============================
VM & Guest_Memfd Preservation
=============================

.. kernel-doc:: virt/kvm/kvm_luo.c
   :doc: KVM VM Preservation via LUO

.. kernel-doc:: virt/kvm/guest_memfd_luo.c
   :doc: Guest_Memfd Preservation via LUO

VMM Instructions
================

This section describes the requirements, scope, conditions, and
ordering constraints that a Virtual Machine Monitor (VMM) must adhere
to for successful preservation and retrieval of guest_memfd files
across a Live Update Orchestrator (LUO) sequence.

Scope and Limitations
---------------------

At this stage, the scope of guest_memfd preservation is restricted to:

1. **Fully Shared guest_memfd**:
   At this time only fully shared guest_memfd is supported. Any system that
   supports coco vm (which uses private guest_memfd), will not support
   the preservation.

2. **Standard Page Size**:
   Only guest_memfd backed by standard page size (``PAGE_SIZE``,
   order-0) pages is supported. Large/huge page backing (e.g.,
   hugetlb guest_memfd) is not supported.

Any Virtual Machine (VM) whose memory is fully backed by such
guest_memfd files can be preserved across live update.

VMM Actions and Conditions during Live Update
---------------------------------------------

During the live update sequence, the kernel introduces a *freezing*
phase for the guest_memfd inode. Freezing prevents any modifications to
the guest_memfd page cache. Specifically, once a guest_memfd mapping is
frozen:

- Any subsequent ``fallocate`` calls on the guest_memfd file descriptor
  will fail and return ``-EPERM``.
- Any new page faults (guest-side or host-userspace-side) that require
  folio allocation will fail and return ``-EPERM``.

To prevent vCPUs or VMM helper threads from failing due to these
``-EPERM`` errors, the VMM must implement one of the following
strategies:

1. **Pause the VM (Recommended)**:
   The VMM should pause/suspend all vCPUs before invoking the
   preservation or freezing of the VM and guest_memfd files. This
   ensures no new page faults or memory accesses can occur while the
   guest_memfd is frozen.

2. **Handle Fault Failures**:
   If the VM is not paused, the VMM must be prepared to handle VM
   exits or user page fault errors resulting from the ``-EPERM``
   failures. The VMM must take appropriate action, such as
   immediately pausing the VM, or aborting the live update sequence
   (by tearing down or unpreserving the live update session).

Preservation and Retrieval Ordering
-----------------------------------

Preservation Order
~~~~~~~~~~~~~~~~~~

The KVM VM file must be preserved into the LUO session before any of
its associated guest_memfd files are preserved. During guest_memfd
preservation, KVM verifies that the parent VM file descriptor has
already been preserved in the session and records its token into
``ser->vm_token``. If guest_memfd preservation is attempted before the
VM file is preserved, it fails with ``-ENOENT``. guest_memfd file
descriptors do not require a separate freeze step.

Retrieval Order
~~~~~~~~~~~~~~~

Similarly, there is no strict ordering required for retrieving the VM
and guest_memfd files. Any file can be retrieved at any order.

If guest_memfd file is retrieved and VM file is not retrieved, and
luo_finish is called, then vm_file will be lost and guest_memfd file
will be hanging around.

NOTE: Before Initiating the preservation/retrieval, it is necessary to make
sure that the kvm module is loaded (/dev/kvm must be available).


vCPU Preservation
=================

In addition to preserving the VM file descriptor and ``guest_memfd``
files, a VMM can preserve individual vCPU file descriptors
(``kvm-vcpu:<id>``) across a live update using the architecture-specific
``kvm_vcpu_luo_x86_v1`` or ``kvm_vcpu_luo_arm64_v1`` LUO file handler:

- **Preserve** (``kvm_vcpu_luo_preserve()``): KVM acquires ``vcpu->mutex``
  via ``mutex_trylock()`` (failing with ``-EBUSY`` if the vCPU is running
  or already preserved), verifies that the parent VM file descriptor has
  already been preserved in the session, resolves the parent VM's token via
  ``liveupdate_get_token_outgoing()`` (storing it in ``ser->vm_token``),
  allocates a ``struct kvm_vcpu_ser`` descriptor in KHO memory recording
  ``vcpu_id``, completes any pending userspace I/O, calls
  ``kvm_arch_vcpu_luo_preserve()`` to serialize the vCPU's architectural state
  into ``struct kvm_vcpu_arch_ser``, and marks the vCPU preserved
  (``vcpu->luo_preserved = true``) so subsequent ``kvm_vcpu_ioctl()`` calls
  are rejected with ``-EBUSY``.
- **Freeze** (``kvm_vcpu_luo_freeze()``): If the live update session has
  preserved physical CPUs and Caretaker is enabled, KVM activates the
  Caretaker On-Core job for the vCPU via ``kvm_caretaker_vcpu_freeze()``,
  transitioning the vCPU into orphaned mode and kicking the assigned
  preserved physical CPU to begin on-core execution.
- **Retrieve** (``kvm_vcpu_luo_retrieve()``): In the incoming kernel,
  KVM resolves the parent VM file from the session via
  ``liveupdate_get_file_incoming()`` using ``ser->vm_token``, creates a
  vCPU with ``ser->vcpu_id`` under the retrieved VM via
  ``kvm_create_vcpu_file()``, and invokes ``kvm_arch_vcpu_luo_retrieve()``
  to restore the serialized architectural state before publishing the vCPU
  to ``kvm->vcpu_array`` or exposing its file descriptor.
- **Finish** (``kvm_vcpu_luo_finish()``): After retrieval completes and
  the LUO session finishes, KVM frees the restored KHO serialization
  buffers via ``kho_restore_free()``.

VMM Requirements and Ordering
-----------------------------

Preservation and Freeze Ordering
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The parent VM file descriptor must be preserved into the LUO session
before any of its vCPU file descriptors are preserved. When a vCPU file
descriptor is preserved into a LUO session, KVM verifies that the parent VM
file descriptor is already present in the session and retrieves its token;
if the parent VM has not yet been preserved, ``kvm_vcpu_luo_preserve()``
fails with ``-ENOENT``.

When Caretaker is configured, preserved vCPUs remain paused in host memory
during the preservation phase and only transition to orphaned on-core
execution when the LUO session is frozen (either explicitly via
``LIVEUPDATE_SESSION_FREEZE`` or automatically during the reboot transition).

Retrieval Ordering
~~~~~~~~~~~~~~~~~~

There is no strict ordering requirement between retrieving the VM file
descriptor, ``guest_memfd`` file descriptors, and vCPU file descriptors.
When a vCPU file descriptor is retrieved in the incoming kernel,
``liveupdate_get_file_incoming()`` automatically retrieves the parent VM
file if it has not yet been retrieved, creates the vCPU under that VM,
and restores its architectural state before publishing the vCPU. Before
resuming vCPU execution, the VMM must re-establish non-preserved VM state
such as memory slots backed by the retrieved ``guest_memfd`` files.

vCPU Execution During Preservation
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``kvm_vcpu_luo_preserve()`` captures a snapshot of the vCPU's
architectural state under ``vcpu->mutex`` at preserve time and sets
``vcpu->luo_preserved = true``. The VMM must stop running the vCPU
before preserving it; while preserved, ``kvm_vcpu_ioctl()`` rejects any
vCPU ioctl with ``-EBUSY`` until the live update is cancelled.

Cancellation (Unpreserve)
~~~~~~~~~~~~~~~~~~~~~~~~~

If the live update is cancelled before kexec
(``kvm_vcpu_luo_unpreserve()``), KVM clears ``vcpu->luo_preserved`` and
frees the KHO-allocated ``struct kvm_vcpu_arch_ser`` and
``struct kvm_vcpu_ser`` buffers via ``kho_unpreserve_free()``. The
in-memory ``struct kvm_vcpu`` in the outgoing kernel remains intact,
allowing the VMM to resume vCPU execution without recreating the vCPU.

Preserved Architectural State
-----------------------------

x86 (``CONFIG_X86_64``)
~~~~~~~~~~~~~~~~~~~~~~~

On x86, ``struct kvm_vcpu_arch_ser`` (along with trailing
``struct kvm_msrs`` and ``struct kvm_cpuid2`` payloads in the same KHO
allocation) preserves:

- General-purpose registers (``struct kvm_regs``).
- Segment registers, descriptor tables, control registers, and PAE PDPTRs
  (``struct kvm_sregs2``).
- Multiprocessor state (``struct kvm_mp_state``), captured first so any
  pending APIC INIT/SIPI events are processed before saving registers.
- Extended control registers, including ``XCR0`` (``struct kvm_xcrs``).
- Extended FPU and processor state (``struct kvm_xsave``). The host CPU
  must support ``X86_FEATURE_XSAVE`` and the guest's XSAVE area
  (``uabi_size``) must fit within ``sizeof(struct kvm_xsave)``; otherwise
  preservation fails with ``-EOPNOTSUPP`` (for example, when extended
  XSAVE states such as AMX exceed ``struct kvm_xsave``). Confidential or
  protected-state guests are also rejected with ``-EOPNOTSUPP``.
- Hardware debug registers (``struct kvm_debugregs``).
- VM ``kvmclock`` data (``struct kvm_clock_data``), restored when
  retrieving ``vcpu_id == 0``.
- Pending exceptions, interrupts, NMIs, and SMIs
  (``struct kvm_vcpu_events``). Any stale injected maskable hardware
  interrupt with ``RFLAGS.IF == 0`` is cleared in the serialized copy.
- In-kernel local APIC state (``struct kvm_lapic_state``) when
  ``lapic_in_kernel(vcpu)`` is true.
- Architectural and paravirtual MSRs (``struct kvm_msrs``).
- Guest CPUID table (``struct kvm_cpuid2``).

Nested virtualization state is not preserved on x86: preserving a vCPU
with nested virtualization active (``is_guest_mode(vcpu)``,
``EFER.SVME``, ``!gif_set(vcpu)``, ``CR4.VMXE``, or active VMXON state)
fails with ``-EOPNOTSUPP``.

arm64 (``CONFIG_ARM64``)
~~~~~~~~~~~~~~~~~~~~~~~~

On arm64, ``struct kvm_vcpu_arch_ser`` (along with a trailing
``struct kvm_arm64_sysregs_ser`` array in the same KHO allocation)
preserves initialized non-protected vCPUs:

- Core general-purpose registers, ``SP_EL1``, ``ELR_EL1``, banked
  ``SPSR_*`` registers, and FP/SIMD registers (``struct kvm_regs``),
  after servicing any pending ``KVM_REQ_VCPU_RESET``.
- Multiprocessor state (``struct kvm_mp_state``).
- Pending exception and SError injection state
  (``struct kvm_vcpu_events``).
- CPU target (``KVM_ARM_TARGET_GENERIC_V8``) and the VM's vCPU feature
  bitmap (``struct kvm_vcpu_init``), which are applied via
  ``kvm_arm_vcpu_init()`` to initialize and reset the vCPU before
  restoring register values.
- Architectural system registers and firmware pseudo-registers visible via
  ``kvm_arm_get_sys_reg_indices()`` (serialized as a
  ``struct kvm_one_reg`` array in ``struct kvm_arm64_sysregs_ser``),
  including the GICv3 CPU interface registers when an in-kernel VGICv3
  irqchip is configured. On retrieval, the in-kernel VGICv3 CPU
  interface is reset before writing back the system registers, marked
  restored so lazy ``vgic_init()`` does not clobber ``vgic_vmcr``, and
  the architected virtual and physical timer VGIC IRQs are re-enabled.

State not exposed through ``kvm_arm_get_sys_reg_indices()`` or the
structures above (such as VGIC distributor/redistributor/ITS state,
protected VM state, or nested virtualization state) is not preserved by
the vCPU handler.

vCPU Preservation ABI
---------------------

.. kernel-doc:: include/linux/kho/abi/kvm_x86.h
   :doc: x86 KVM Live Update ABI

.. kernel-doc:: include/linux/kho/abi/kvm_arm64.h
   :doc: arm64 KVM vCPU Live Update ABI


VM & Guest_Memfd Preservation ABI
=================================

.. kernel-doc:: include/linux/kho/abi/kvm.h
   :doc: KVM and guest_memfd Live Update ABI

.. kernel-doc:: include/linux/kho/abi/kvm.h
   :internal:

See Also
========

- :doc:`/core-api/liveupdate`
- :doc:`/userspace-api/liveupdate`
