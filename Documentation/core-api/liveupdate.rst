.. SPDX-License-Identifier: GPL-2.0

========================
Live Update Orchestrator
========================
:Author: Pasha Tatashin <pasha.tatashin@soleen.com>

.. kernel-doc:: kernel/liveupdate/luo_core.c
   :doc: Live Update Orchestrator (LUO)

LUO Subsystems Participation
============================
.. kernel-doc:: kernel/liveupdate/luo_subsystems.c
   :doc: LUO Subsystems support

LUO Preserving File Descriptors
===============================
.. kernel-doc:: kernel/liveupdate/luo_files.c
   :doc: LUO file descriptors

The following types of file descriptors can be preserved

.. toctree::
   :maxdepth: 1

   ../mm/memfd_preservation

Public API
==========
.. kernel-doc:: include/linux/liveupdate.h

.. kernel-doc:: kernel/liveupdate/luo_core.c
   :export:

.. kernel-doc:: kernel/liveupdate/luo_subsystems.c
   :export:

.. kernel-doc:: kernel/liveupdate/luo_files.c
   :export:

Internal API
============
.. kernel-doc:: kernel/liveupdate/luo_core.c
   :internal:

.. kernel-doc:: kernel/liveupdate/luo_subsystems.c
   :internal:

.. kernel-doc:: kernel/liveupdate/luo_files.c
   :internal:

See Also
========

- :doc:`Live Update uAPI </userspace-api/liveupdate>`
- :doc:`Live Update SysFS </admin-guide/liveupdate>`
- :doc:`/core-api/kho/concepts`
