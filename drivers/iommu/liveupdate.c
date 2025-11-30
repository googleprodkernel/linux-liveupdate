// SPDX-License-Identifier: GPL-2.0-only

/*
 * Copyright (C) 2025, Google LLC
 * Author: Samiullah Khawaja <skhawaja@google.com>
 */

#define pr_fmt(fmt)    "iommu: liveupdate: " fmt

#include <linux/kexec_handover.h>
#include <linux/liveupdate.h>
#include <linux/iommu-lu.h>
#include <linux/iommu.h>
#include <linux/errno.h>

static void iommu_liveupdate_free_objs(u64 next, bool incoming)
{
	struct iommu_objs_ser *objs;

	while (next) {
		objs = __va(next);
		next = objs->next_objs;

		if (!incoming)
			kho_unpreserve_free(objs);
		else
			folio_put(virt_to_folio(objs));
	}
}

static void iommu_liveupdate_flb_free(struct iommu_lu_flb_obj *obj)
{
	if (obj->iommu_domains)
		iommu_liveupdate_free_objs(obj->ser->iommu_domains_phys, false);

	if (obj->devices)
		iommu_liveupdate_free_objs(obj->ser->devices_phys, false);

	if (obj->iommus)
		iommu_liveupdate_free_objs(obj->ser->iommus_phys, false);

	kho_unpreserve_free(obj->ser);
}

static int iommu_liveupdate_flb_preserve(struct liveupdate_flb_op_args *argp)
{
	struct iommu_lu_flb_obj *obj;
	struct iommu_lu_flb_ser *ser;
	void *mem;

	obj = kzalloc(sizeof(*obj), GFP_KERNEL);
	if (!obj)
		return -ENOMEM;

	mutex_init(&obj->lock);
	mem = kho_alloc_preserve(sizeof(*ser));
	if (IS_ERR(mem))
		goto err_free;

	ser = mem;
	obj->ser = ser;

	mem = kho_alloc_preserve(PAGE_SIZE);
	if (IS_ERR(mem))
		goto err_free;

	obj->iommu_domains = mem;
	ser->iommu_domains_phys = virt_to_phys(obj->iommu_domains);

	mem = kho_alloc_preserve(PAGE_SIZE);
	if (IS_ERR(mem))
		goto err_free;

	obj->devices = mem;
	ser->devices_phys = virt_to_phys(obj->devices);

	mem = kho_alloc_preserve(PAGE_SIZE);
	if (IS_ERR(mem))
		goto err_free;

	obj->iommus = mem;
	ser->iommus_phys = virt_to_phys(obj->iommus);

	argp->obj = obj;
	argp->data = virt_to_phys(ser);
	return 0;

err_free:
	iommu_liveupdate_flb_free(obj);
	return PTR_ERR(mem);
}

static void iommu_liveupdate_flb_unpreserve(struct liveupdate_flb_op_args *argp)
{
	iommu_liveupdate_flb_free(argp->obj);
}

static void iommu_liveupdate_flb_finish(struct liveupdate_flb_op_args *argp)
{
}

static int iommu_liveupdate_flb_retrieve(struct liveupdate_flb_op_args *argp)
{
	return -EOPNOTSUPP;
}

static struct liveupdate_flb_ops iommu_flb_ops = {
	.preserve = iommu_liveupdate_flb_preserve,
	.unpreserve = iommu_liveupdate_flb_unpreserve,
	.finish = iommu_liveupdate_flb_finish,
	.retrieve = iommu_liveupdate_flb_retrieve,
};

static struct liveupdate_flb iommu_flb = {
	.compatible = IOMMU_LUO_FLB_COMPATIBLE,
	.ops = &iommu_flb_ops,
};

int iommu_liveupdate_register_flb(struct liveupdate_file_handler *handler)
{
	return liveupdate_register_flb(handler, &iommu_flb);
}
EXPORT_SYMBOL(iommu_liveupdate_register_flb);

int iommu_liveupdate_unregister_flb(struct liveupdate_file_handler *handler)
{
	return liveupdate_unregister_flb(handler, &iommu_flb);
}
EXPORT_SYMBOL(iommu_liveupdate_unregister_flb);

static int reserve_obj_ser(struct iommu_objs_ser **objs_ptr, u64 max_objs)
{
	struct iommu_objs_ser *next_objs, *objs = *objs_ptr;
	int idx;

	if (objs->nr_objs == max_objs) {
		next_objs = kho_alloc_preserve(PAGE_SIZE);
		if (!next_objs)
			return -ENOMEM;

		objs->next_objs = virt_to_phys(next_objs);
		objs = next_objs;
		*objs_ptr = objs;
		objs->nr_objs = 0;
	}

	idx = objs->nr_objs++;
	return idx;
}

int iommu_domain_preserve(struct iommu_domain *domain, struct iommu_domain_ser **ser)
{
	struct iommu_domain_ser *domain_ser;
	struct iommu_lu_flb_obj *flb_obj;
	int idx, ret;

	if (!domain->ops->preserve)
		return -EOPNOTSUPP;

	ret = liveupdate_flb_get_outgoing(&iommu_flb, (void **)&flb_obj);
	if (ret)
		return ret;

	guard(mutex)(&flb_obj->lock);
	idx = reserve_obj_ser((struct iommu_objs_ser **)&flb_obj->iommu_domains,
			      MAX_IOMMU_DOMAIN_SERS);
	if (idx < 0)
		return idx;

	domain_ser = &flb_obj->iommu_domains->iommu_domains[idx];
	idx = flb_obj->ser->nr_domains++;
	domain_ser->obj.idx = idx;
	domain_ser->obj.ref_count = 1;

	ret = domain->ops->preserve(domain, domain_ser);
	if (ret) {
		domain_ser->obj.deleted = true;
		return ret;
	}

	domain->preserved_state = domain_ser;
	*ser = domain_ser;
	return 0;
}
EXPORT_SYMBOL_GPL(iommu_domain_preserve);

int iommu_domain_unpreserve(struct iommu_domain *domain)
{
	struct iommu_domain_ser *domain_ser;
	struct iommu_lu_flb_obj *flb_obj;
	int ret;

	if (!domain->ops->unpreserve)
		return -EOPNOTSUPP;

	ret = liveupdate_flb_get_outgoing(&iommu_flb, (void **)&flb_obj);
	if (ret)
		return ret;

	guard(mutex)(&flb_obj->lock);
	domain_ser = domain->preserved_state;
	if (domain_ser->attach_count)
		ret = -EBUSY;

	domain->ops->unpreserve(domain, domain_ser);
	domain_ser->obj.deleted = true;
	domain->preserved_state = NULL;

	return 0;
}
EXPORT_SYMBOL_GPL(iommu_domain_unpreserve);
