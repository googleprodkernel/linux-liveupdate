.. SPDX-License-Identifier: GPL-2.0

==============================
Live Update Orchestrator (LUO)
==============================
:Author: Pasha Tatashin <pasha.tatashin@soleen.com>

.. kernel-doc:: drivers/misc/liveupdate/luo_core.c
   :doc: Live Update Orchestrator (LUO)

LUO Subsystems Participation
============================
.. kernel-doc:: drivers/misc/liveupdate/luo_subsystems.c
   :doc: LUO Subsystems support

LUO Preserving File Descriptors
===============================
.. kernel-doc:: drivers/misc/liveupdate/luo_files.c
   :doc: LUO file descriptors

LUO ioctl interface
===================
.. kernel-doc:: drivers/misc/liveupdate/luo_ioctl.c
   :doc: LUO ioctl Interface

LUO sysfs interface
===================
.. kernel-doc:: drivers/misc/liveupdate/luo_sysfs.c
   :doc: LUO sysfs interface

LUO selftests ioctl
===================
.. kernel-doc:: drivers/misc/liveupdate/luo_selftests.c
   :doc: LUO Selftests

ioctl uAPI
===========
.. kernel-doc:: include/uapi/linux/liveupdate.h

Public API
==========
.. kernel-doc:: include/linux/liveupdate.h

.. kernel-doc:: drivers/misc/liveupdate/luo_core.c
   :export:

.. kernel-doc:: drivers/misc/liveupdate/luo_subsystems.c
   :export:

.. kernel-doc:: drivers/misc/liveupdate/luo_files.c
   :export:

Internal API
============
.. kernel-doc:: drivers/misc/liveupdate/luo_core.c
   :internal:

.. kernel-doc:: drivers/misc/liveupdate/luo_subsystems.c
   :internal:

.. kernel-doc:: drivers/misc/liveupdate/luo_files.c
   :internal:
