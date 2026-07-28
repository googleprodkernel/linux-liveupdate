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

There is no strict ordering requirement for initiating the
preservation of the KVM VM file and the guest_memfd files; they are
preserved independently. If kexec is triggered with guest_memfd
preservation without preserving the vm file, kexec will fail.

Retrieval Order
~~~~~~~~~~~~~~~

Similarly, there is no strict ordering required for retrieving the VM
and guest_memfd files. Any file can be retrieved at any order.

If guest_memfd file is retrieved and VM file is not retrieved, and
luo_finish is called, then vm_file will be lost and guest_memfd file
will be hanging around.

NOTE: Before Initiating the preservation/retrieval, it is necessary to make
sure that the kvm module is loaded (/dev/kvm must be available).


VM & Guest_Memfd Preservation ABI
=================================

.. kernel-doc:: include/linux/kho/abi/kvm.h
   :doc: DOC: guest_memfd Live Update ABI

.. kernel-doc:: include/linux/kho/abi/kvm.h
   :internal:

See Also
========

- :doc:`/core-api/liveupdate`
- :doc:`/userspace-api/liveupdate`
