// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2026, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 *
 * Architecture specific CPU preservation support for x86.
 */
#include <linux/cc_platform.h>
#include <linux/cpu.h>
#include <linux/cpu_preserve.h>
#include <linux/cpufeature.h>
#include <linux/delay.h>
#include <linux/kexec_handover.h>
#include <linux/mm.h>
#include <linux/pgtable.h>
#include <linux/smp.h>

#include <asm/apic.h>
#include <asm/desc.h>
#include <asm/init.h>
#include <asm/realmode.h>
#include <asm/set_memory.h>
#include <asm/trapnr.h>

#include "cpu_preserve_internal.h"

static DEFINE_PER_CPU(struct cpupr_ser *, x86_cpupr_wake_ser);
static pgd_t x86_cpupr_wake_pgd_entry;
static u32 x86_cpupr_wake_start_ip;

static void *x86_cpupr_alloc_pgt_page(void *context)
{
	return (void *)get_zeroed_page(GFP_KERNEL);
}

static int x86_cpupr_get_wake_trampoline(pgd_t *pgd_entry, u32 *start_ip)
{
	static DEFINE_MUTEX(wake_lock);

	guard(mutex)(&wake_lock);
	if (!x86_cpupr_wake_start_ip) {
		static const u8 stub_code[] = {
			0xbc, 0x00, 0x20, 0x00, 0x00,             /* mov $0x2000, %esp */
			0x48, 0x8b, 0x05, 0x0c, 0x00, 0x00, 0x00, /* mov 12(%rip), %rax */
			0x0f, 0x22, 0xd8,                         /* mov %rax, %cr3 */
			0xff, 0x25, 0x0b, 0x00, 0x00, 0x00,       /* jmp *11(%rip) */
			0x90, 0x90, 0x90,                         /* nop; nop; nop */
		};
		struct {
			u8 code[24];
			u64 cr3;
			u64 rip;
		} __packed *stub;
		phys_addr_t stub_pa;
		pgd_t *wake_pgd;
		struct x86_mapping_info info = {
			.alloc_pgt_page = x86_cpupr_alloc_pgt_page,
			.page_flag = __PAGE_KERNEL_EXEC,
			.force_pte = true,
		};
		int i;

		wake_pgd = (pgd_t *)get_zeroed_page(GFP_KERNEL);
		if (!wake_pgd)
			return -ENOMEM;

		stub = (void *)get_zeroed_page(GFP_KERNEL);
		if (!stub) {
			free_page((unsigned long)wake_pgd);
			return -ENOMEM;
		}

		stub_pa = virt_to_phys(stub);
		info.offset = PAGE_SIZE - stub_pa;
		if (kernel_ident_mapping_init(&info, wake_pgd, stub_pa,
					      stub_pa + PAGE_SIZE)) {
			free_page((unsigned long)stub);
			free_page((unsigned long)wake_pgd);
			return -ENOMEM;
		}

		for (i = pgd_index(__PAGE_OFFSET); i < PTRS_PER_PGD; i++)
			wake_pgd[i] = init_top_pgt[i];

		memcpy(stub->code, stub_code, sizeof(stub_code));
		stub->cr3 = virt_to_phys(wake_pgd) | sme_me_mask;
		stub->rip = (u64)secondary_startup_64;
		cpupr_clean(stub);

		x86_cpupr_wake_pgd_entry = wake_pgd[0];
		smp_store_release(&x86_cpupr_wake_start_ip, PAGE_SIZE);
	}

	*pgd_entry = x86_cpupr_wake_pgd_entry;
	*start_ip = x86_cpupr_wake_start_ip;
	return 0;
}

/*
 * Initialize the preserved IDT with exception handlers.
 *
 * Vectors 0..31 are x86 architecture exceptions/traps.  They enter
 * x86_preserved_exc_handler_array[v] and x86_preserved_handle_exception():
 * - X86_TRAP_NMI (vector 2) is the kick sent by arch_cpupr_kick().  It
 *   wakes up a CPU that sleeps in x86_preserved_idle() and returns.
 * - _ASM_EXTABLE fixups in .preserved.ex_table are applied, and the CPU
 *   resumes after a recoverable machine check.  Any other exception is recorded
 *   and stops the CPU.
 * - #DF, NMI and #MC run on IST stacks, so they work even when the current
 *   stack does not.
 *
 * Vectors >= FIRST_EXTERNAL_VECTOR (32) are external interrupts, which are not
 * delivered: the runtime keeps interrupts disabled, and the local APIC stays
 * software-disabled.  Their gates are not present, so that one would raise #NP
 * and stop the CPU.
 */
static void init_preserved_idt(void)
{
	int v;

	for (v = 0; v < NUM_EXCEPTION_VECTORS; v++) {
		unsigned long handler = (unsigned long)x86_preserved_exc_handler_array[v];
		unsigned int ist = 0;

		if (v == X86_TRAP_DF)
			ist = X86_PRESERVED_IST_DF + 1;
		else if (v == X86_TRAP_NMI)
			ist = X86_PRESERVED_IST_NMI + 1;
		else if (v == X86_TRAP_MC)
			ist = X86_PRESERVED_IST_MC + 1;

		pack_gate(&x86_preserved_idt[v], GATE_INTERRUPT, handler, 0,
			  ist, __KERNEL_CS);
	}
	x86_preserved_idt_desc.size = sizeof(x86_preserved_idt) - 1;
	x86_preserved_idt_desc.address = (unsigned long)&x86_preserved_idt[0];
	cpupr_clean(&x86_preserved_idt);
	cpupr_clean(&x86_preserved_idt_desc);
}

void arch_cpupr_early_init(void)
{
	init_preserved_idt();
	x86_preserved_mwait = boot_cpu_has(X86_FEATURE_MWAIT) &&
			      !boot_cpu_has_bug(X86_BUG_MONITOR) &&
			      !boot_cpu_has_bug(X86_BUG_CLFLUSH_MONITOR);
	x86_preserved_sme_mask = sme_me_mask;
}

/*
 * The parked CPUs keep the APIC mode of the kernel that preserved them, and
 * their page tables its paging depth.
 */
struct cpupr_arch_mode_ser arch_cpupr_mode(void)
{
	return (struct cpupr_arch_mode_ser){
		.x2apic = !!x2apic_mode,
		.la57 = pgtable_l5_enabled(),
	};
}

static u32 x86_cpupr_apicid(int cpu)
{
	if ((unsigned int)cpu >= nr_cpu_ids)
		return BAD_APICID;

	return cpuid_to_apicid[cpu];
}

/*
 * Kick a preserved CPU with an NMI when HLT is used instead of MWAIT.  Use the
 * physical destination: this kernel sets up the logical destination of a CPU
 * only when it brings the CPU up.
 */
static void x86_cpupr_send_nmi(int cpu)
{
	unsigned long flags;
	u32 apicid;

	if (x86_preserved_mwait)
		return;

	apicid = x86_cpupr_apicid(cpu);
	if (apicid == BAD_APICID)
		return;

	local_irq_save(flags);
	apic_wait_icr_idle();
	apic_icr_write(APIC_DM_NMI | APIC_DEST_PHYSICAL, apicid);
	local_irq_restore(flags);
}

void arch_cpupr_kick(int cpu)
{
	if (!cpu_is_preserved(cpu))
		return;

	x86_cpupr_send_nmi(cpu);
}

u64 arch_cpupr_hwid(unsigned int cpu)
{
	return cpuid_to_apicid[cpu];
}

/*
 * Switch stack and enter park loop.
 */
void arch_cpupr_park_on_stack(int cpu, unsigned long stack_top)
{
	arch_cpupr_call_on_stack(cpu, stack_top, arch_cpupr_park_worker);
}

/**
 * arch_cpupr_as_map - Populate an isolated page table on x86
 * @ps: Session that owns the address space.
 * @pgd: Root page table of the address space.
 * @pa: Physical address of the range.
 * @va: Virtual address the range must appear at.
 * @size: Size of the range in bytes.
 * @prot: Protection to apply.
 *
 * Return: 0 on success, or a negative errno on failure.
 */
int arch_cpupr_as_map(struct cpupr_session *ps, void *pgd,
		      phys_addr_t pa, unsigned long va, size_t size,
		      pgprot_t prot)
{
	unsigned long offset = va & ~PAGE_MASK;
	size_t page_size = PAGE_ALIGN(offset + size);
	unsigned long page_va = va & PAGE_MASK;
	phys_addr_t page_pa = (pa & PAGE_MASK);
	struct x86_mapping_info info = {
		.alloc_pgt_page = cpupr_as_alloc_page,
		.context = ps,
		.page_flag = pgprot_val(prot),
		.offset = page_va - page_pa,
		.force_pte = true,
	};

	return kernel_ident_mapping_init(&info, pgd, page_pa,
					 page_pa + page_size);
}

/*
 * Only the isolated address spaces map the copy for execution, and only the
 * preserved CPUs write it: keep its direct map alias read-only.
 */
int arch_cpupr_setup_buffer(struct page *text_page,
			    unsigned int text_nr_pages,
			    struct page *data_page,
			    unsigned int data_nr_pages)
{
	unsigned long text = (unsigned long)page_address(text_page);
	unsigned long data = (unsigned long)page_address(data_page);
	int ret;

	/*
	 * The runtime requires native play_dead / APIC INIT-SIPI bringup and
	 * cannot handle confidential-guest #VC/#VE exceptions.
	 */
	if (cc_platform_has(CC_ATTR_GUEST_STATE_ENCRYPT) ||
	    cpu_feature_enabled(X86_FEATURE_TDX_GUEST) ||
	    smp_ops.play_dead != native_play_dead ||
	    apic->wakeup_secondary_cpu ||
	    apic->wakeup_secondary_cpu_64) {
		pr_warn("cpu_preserve: platform not supported\n");
		return -EOPNOTSUPP;
	}

	ret = set_memory_ro(text, text_nr_pages);
	if (!ret) {
		ret = set_memory_ro(data, data_nr_pages);
		if (!ret)
			return 0;
		set_memory_rw(data, data_nr_pages);
	}
	set_memory_rw(text, text_nr_pages);
	return ret;
}

void arch_cpupr_wait_dead(int cpu, struct cpupr_ser *ser, bool bringup)
{
	struct cpupr_stack_context *sctx = cpupr_get_sctx(cpu);
	unsigned long flags;
	u32 apicid;

	if (sctx) {
		const struct x86_preserved_fault *f = &sctx->fault;

		if (READ_ONCE(f->count))
			pr_err("cpu_preserve: CPU %d unhandled fault vec=%lu err=0x%lx ip=0x%lx cr2=0x%lx cr3=0x%lx\n",
			       cpu, f->vector, f->error_code, f->ip, f->cr2, f->cr3);
		if (READ_ONCE(f->nr_mce))
			pr_warn("cpu_preserve: CPU %d recovered from %lu machine checks\n",
				cpu, f->nr_mce);
	}

	if (bringup) {
		phys_addr_t pgd_pa = 0;
		pgd_t wake_pgd_entry;
		u32 wake_start_ip;

		if (ser && READ_ONCE(ser->state) == CPUPR_DEAD &&
		    !x86_cpupr_get_wake_trampoline(&wake_pgd_entry, &wake_start_ip)) {
			if (kho_is_ro()) {
				static phys_addr_t last_ro_as_pa;
				static phys_addr_t last_ro_pgd_pa;

				if (ser->as.phys == READ_ONCE(last_ro_as_pa) &&
				    READ_ONCE(last_ro_pgd_pa)) {
					pgd_pa = READ_ONCE(last_ro_pgd_pa);
				} else {
					struct cpupr_as_ser *as;

					as = kho_ro_map_folio(ser->as.phys, PAGE_KERNEL_RO);
					if (as)
						pgd_pa = as->pgd_pa;
					if (pgd_pa) {
						pgd_t *pgd = kho_ro_map_folio(pgd_pa, PAGE_KERNEL);

						if (pgd) {
							WRITE_ONCE(pgd[0], wake_pgd_entry);
							cpupr_clean(&pgd[0]);
							WRITE_ONCE(last_ro_pgd_pa, pgd_pa);
							WRITE_ONCE(last_ro_as_pa, ser->as.phys);
						} else {
							pgd_pa = 0;
						}
					}
				}
			} else {
				struct cpupr_as_ser *as = KHOSER_LOAD_PTR(ser->as);

				if (as && as->pgd_pa) {
					pgd_t *pgd = phys_to_virt(as->pgd_pa);

					WRITE_ONCE(pgd[0], wake_pgd_entry);
					cpupr_clean(&pgd[0]);
					pgd_pa = as->pgd_pa;
				}
			}
		}
		if (pgd_pa) {
			per_cpu(x86_cpupr_wake_ser, cpu) = ser;
			return;
		}
	}

	per_cpu(x86_cpupr_wake_ser, cpu) = NULL;

	apicid = x86_cpupr_apicid(cpu);
	if (apicid == BAD_APICID)
		return;

	/*
	 * Put the stopped CPU into wait-for-SIPI so it no longer references
	 * the session page tables, stack, or runtime text if it stays offline.
	 */
	local_irq_save(flags);
	apic_wait_icr_idle();
	apic_icr_write(APIC_INT_LEVELTRIG | APIC_INT_ASSERT | APIC_DM_INIT,
		       apicid);
	safe_apic_wait_icr_idle();
	apic_icr_write(APIC_INT_LEVELTRIG | APIC_DM_INIT, apicid);
	safe_apic_wait_icr_idle();
	local_irq_restore(flags);
}

int arch_cpupr_wakeup_ap(unsigned int cpu)
{
	pgd_t wake_pgd_entry;
	struct cpupr_ser *ser;
	u32 wake_start_ip;
	unsigned int i;

	if (cpu >= nr_cpu_ids)
		return -EINVAL;

	ser = xchg(&per_cpu(x86_cpupr_wake_ser, cpu), NULL);
	if (!ser)
		return -ENODEV;

	if (x86_cpupr_get_wake_trampoline(&wake_pgd_entry, &wake_start_ip))
		return -ENOMEM;

	smp_store_release(&ser->state, wake_start_ip);
	cpupr_clean(ser);
	x86_cpupr_send_nmi((int)cpu);

	if (cpuhp_in_batch())
		return 0;

	for (i = 0; i < 1000000; i++) {
		cpupr_clean(ser);
		if (smp_load_acquire(&ser->state) == CPUPR_DEAD)
			return 0;
		udelay(5);
	}

	cpupr_clean(ser);
	if (smp_load_acquire(&ser->state) == CPUPR_DEAD)
		return 0;

	return -ETIMEDOUT;
}
