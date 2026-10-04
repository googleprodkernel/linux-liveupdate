/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Linker script used for partial linking of preserved-runtime object files.
 * Moves all code and data sections into .preserved.* sections before symbol
 * prefixing via objcopy --prefix-symbols=__preserved_.
 *
 * Sections such as .altinstructions pass through to vmlinux so that boot-time
 * patching applies before cpupr_init_runtime_buffer_locked() copies the
 * runtime sections.
 */
SECTIONS {
	/* Must come before .preserved.data so .data..percpu* is not matched by .data.* */
	.bad : {
		*(__jump_table .static_call_sites __bug_table __kvm_ex_table)
		*(__tracepoints* __ksymtab* .export_symbol)
		*(.data..percpu* .init.* .exit.*)
	}
	ASSERT(SIZEOF(.bad) == 0, "Forbidden section in preserved runtime object")

	.preserved.text : {
		*(.text .text.* .noinstr.text)
	}

	/* Mapped read-only in the isolated address space */
	.preserved.rodata : {
		*(.rodata .rodata.* .data..ro_after_init)
	}

	.preserved.data : {
		*(.data .data.*)
	}

	.preserved.ex_table : ALIGN(4) {
		*(__ex_table)
	}

	.preserved.bss : {
		*(.bss .bss.* COMMON)
	}

	/DISCARD/ : {
		*(__patchable_function_entries)
		*(.note.gnu.property)
		*(.llvm_addrsig)
	}
}
