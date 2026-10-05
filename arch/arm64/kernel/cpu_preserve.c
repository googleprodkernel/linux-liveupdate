// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2026, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 *
 * Architecture specific CPU preservation support for ARM64.
 */
#include <linux/arm-smccc.h>
#include <linux/cacheflush.h>
#include <linux/cpu_preserve.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/irqchip/arm-gic-v3.h>
#include <linux/kho/abi/cpu.h>
#include <linux/mm.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/pgtable.h>
#include <linux/psci.h>
#include <uapi/linux/psci.h>

#include <asm/barrier.h>
#include <asm/cpu_ops.h>
#include <asm/cpufeature.h>
#include <asm/sysreg.h>
#include <asm/tlbflush.h>
#include <asm/trans_pgd.h>
#include <asm/virt.h>

#include "cpu_preserve_internal.h"

static void arm64_add_gicr_region(phys_addr_t pa, size_t size, u64 stride)
{
	void __iomem *va;
	size_t map_size;
	int i;

	for (i = 0; i < cpupr_gic.nr_regions; i++)
		if (cpupr_gic.regions[i].pa == pa)
			return;

	if (cpupr_gic.nr_regions >= ARRAY_SIZE(cpupr_gic.regions))
		return;

	map_size = max_t(size_t, size, nr_cpu_ids * (stride ? : SZ_128K));
	va = ioremap(pa, map_size);
	if (!va)
		return;

	i = cpupr_gic.nr_regions++;
	cpupr_gic.regions[i].pa = pa;
	cpupr_gic.regions[i].va = va;
	cpupr_gic.regions[i].size = map_size;
	cpupr_gic.regions[i].stride = stride;
}

static void arm64_discover_gicr_res(struct resource *res)
{
	for (; res; res = res->sibling) {
		if (res->name && !strcmp(res->name, "GICR"))
			arm64_add_gicr_region(res->start, resource_size(res), 0);
		if (res->child)
			arm64_discover_gicr_res(res->child);
	}
}

static void arm64_cpupr_gic_init(void)
{
	struct device_node *node;

	if (!cpus_have_cap(ARM64_HAS_GICV3_CPUIF) ||
	    cpupr_gic.nr_regions > 0)
		return;

	node = of_find_compatible_node(NULL, NULL, "arm,gic-v3");
	if (node) {
		u32 nr_redist_regions = 1;
		u64 stride = 0;
		int i;

		of_property_read_u32(node, "#redistributor-regions",
				     &nr_redist_regions);
		of_property_read_u64(node, "redistributor-stride", &stride);
		for (i = 0; i < nr_redist_regions; i++) {
			struct resource res;

			if (of_address_to_resource(node, 1 + i, &res) == 0)
				arm64_add_gicr_region(res.start,
						      resource_size(&res),
						      stride);
		}
		of_node_put(node);
	}

	if (cpupr_gic.nr_regions == 0)
		arm64_discover_gicr_res(&iomem_resource);
}

/**
 * arch_cpupr_as_map - Populate an isolated page table on arm64
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
	struct trans_pgd_info info = {
		.trans_alloc_page = cpupr_as_alloc_page,
		.trans_alloc_arg = ps,
	};
	unsigned long offset = va & ~PAGE_MASK;
	size_t page_size = PAGE_ALIGN(offset + size);
	unsigned long page_va = va & PAGE_MASK;
	phys_addr_t page_pa = (pa & PAGE_MASK);
	int ret;

	ret = trans_pgd_map_range(&info, pgd, page_pa,
				  page_va, page_size, prot);
	if (!ret)
		dsb(ishst);
	return ret;
}

/**
 * arch_cpupr_setup_buffer - Prepare the copy of the preserved runtime
 * @text_page: Runtime-allocated physical page backing preserved text.
 * @text_nr_pages: Number of pages in text buffer.
 * @data_page: Runtime-allocated physical page backing preserved data.
 * @data_nr_pages: Number of pages in data buffer.
 *
 * Validates platform support for CPU preservation, cleans the D-cache on the
 * linear map alias of the allocated runtime copy, and invalidates the I-cache.
 *
 * Return: 0 on success, or a negative errno on failure.
 */
int arch_cpupr_setup_buffer(struct page *text_page,
			    unsigned int text_nr_pages,
			    struct page *data_page,
			    unsigned int data_nr_pages)
{
	int cpu;

	if (!cpus_have_final_cap(ARM64_HAS_GICV3_CPUIF) ||
	    cpupr_gic.nr_regions <= 0 ||
	    arm64_psci_conduit == SMCCC_CONDUIT_NONE || !psci_ops.cpu_off)
		return -EOPNOTSUPP;

	for_each_present_cpu(cpu) {
		if (!gicv3_get_rdist_for_mpidr(cpu_logical_map(cpu)))
			return -EOPNOTSUPP;
	}

	cpupr_clean_sz(page_address(text_page),
		       text_nr_pages * PAGE_SIZE);
	cpupr_clean_sz(page_address(data_page),
		       data_nr_pages * PAGE_SIZE);
	icache_inval_all_pou();

	return 0;
}

void arch_cpupr_early_init(void)
{
	arm64_cpupr_gic_init();

	if (psci_ops.cpu_off && psci_ops.get_version &&
	    psci_ops.get_version() >= PSCI_VERSION(0, 2))
		arm64_psci_conduit = psci_get_conduit();
}

/*
 * Signal or wake up a preserved physical CPU via SEV and GICv3 SGI.
 */
void arch_cpupr_kick(int cpu)
{
	if (!cpu_is_preserved(cpu))
		return;

	dsb(ishst);
	sev();
	if (cpus_have_cap(ARM64_HAS_GICV3_CPUIF))
		gicv3_cpupr_kick_mpidr(cpu_logical_map(cpu));
	isb();
}

u64 arch_cpupr_hwid(unsigned int cpu)
{
	return cpu_logical_map(cpu);
}

struct cpupr_arch_mode_ser arch_cpupr_mode(void)
{
	return (struct cpupr_arch_mode_ser){
		.page_shift = PAGE_SHIFT,
		.va_bits = VA_BITS,
		.pgtable_levels = CONFIG_PGTABLE_LEVELS,
		.lpa2 = lpa2_is_enabled(),
		.vhe = is_kernel_in_hyp_mode(),
		.be = IS_ENABLED(CONFIG_CPU_BIG_ENDIAN),
	};
}

void arch_cpupr_park_on_stack(int cpu, unsigned long stack_top)
{
	__arch_cpupr_park_on_stack(cpu, stack_top);
}

void arch_cpupr_wait_dead(int cpu, struct cpupr_ser *ser, bool bringup)
{
	const struct cpu_operations *ops = get_cpu_ops(cpu);

	if (ops && ops->cpu_kill)
		ops->cpu_kill(cpu);
}
