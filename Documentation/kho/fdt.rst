.. SPDX-License-Identifier: GPL-2.0-or-later

=======
KHO FDT
=======

KHO uses the flattened device tree (FDT) container format and libfdt
library to create and parse the data that is passed between the
kernels. The properties in KHO FDT are stored in native format and can
include any data KHO users need to preserve. Parsing of FDT subnodes is
responsibility of KHO users, except for nodes and properties defined by
KHO itself.

KHO nodes and properties
========================

Node ``preserved-memory``
-------------------------

KHO saves a special node named ``preserved-memory`` under the root node.
This node contains the metadata for KHO to preserve pages across kexec.

Property ``compatible``
-----------------------

The ``compatible`` property determines compatibility between the kernel
that created the KHO FDT and the kernel that attempts to load it.
If the kernel that loads the KHO FDT is not compatible with it, the entire
KHO process will be bypassed.

Examples
========

The following example demonstrates KHO FDT that preserves two memory
regions create with ``reserve_mem`` kernel command line parameter::

  /dts-v1/;

  / {
  	compatible = "kho-v1";

  	memblock {
  		compatible = "memblock-v1";

  		region1 {
  			compatible = "reserve-mem-v1";
  			start = <0xc07a 0x4000000>;
			size = <0x01 0x00>;
  		};

		region2 {
			compatible = "reserve-mem-v1";
			start = <0xc07b 0x4000000>;
			size = <0x8000 0x00>;
		};

  	};

	preserved-memory {
                metadata = <0x00 0x00>;
        };
  };
