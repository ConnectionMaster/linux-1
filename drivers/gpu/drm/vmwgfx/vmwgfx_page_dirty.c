// SPDX-License-Identifier: GPL-2.0 OR MIT
/**************************************************************************
 *
 * Copyright (c) 2019-2025 Broadcom. All Rights Reserved. The term
 * “Broadcom” refers to Broadcom Inc. and/or its subsidiaries.
 *
 **************************************************************************/
#include "vmwgfx_bo.h"
#include "vmwgfx_drv.h"

/*
 * Different methods for tracking dirty:
 * VMW_BO_DIRTY_PAGETABLE - Scan the pagetable for hardware dirty bits
 * VMW_BO_DIRTY_MKWRITE - Write-protect page table entries and record write-
 * accesses in the VM mkwrite() callback
 */
enum vmw_bo_dirty_method {
	VMW_BO_DIRTY_PAGETABLE,
	VMW_BO_DIRTY_MKWRITE,
};

/*
 * No dirtied pages at scan trigger a transition to the _MKWRITE method,
 * similarly a certain percentage of dirty pages trigger a transition to
 * the _PAGETABLE method. How many triggers should we wait for before
 * changing method?
 */
#define VMW_DIRTY_NUM_CHANGE_TRIGGERS 2

/* Percentage to trigger a transition to the _PAGETABLE method */
#define VMW_DIRTY_PERCENTAGE 10

/**
 * struct vmw_bo_dirty - Dirty information for buffer objects
 * @start: First currently dirty bit
 * @end: Last currently dirty bit + 1
 * @method: The currently used dirty method
 * @change_count: Number of consecutive method change triggers
 * @ref_count: Reference count for this structure
 * @bitmap_size: The size of the bitmap in bits. Typically equal to the
 * nuber of pages in the bo.
 * @bitmap: A bitmap where each bit represents a page. A set bit means a
 * dirty page.
 */
struct vmw_bo_dirty {
	unsigned long start;
	unsigned long end;
	enum vmw_bo_dirty_method method;
	unsigned int change_count;
	unsigned int ref_count;
	unsigned long bitmap_size;
	unsigned long bitmap[];
};

bool vmw_bo_is_dirty(struct vmw_bo *vbo)
{
	return vbo->dirty && (vbo->dirty->start < vbo->dirty->end);
}

/**
 * vmw_bo_dirty_scan_pagetable - Perform a pagetable scan for dirty bits
 * @vbo: The buffer object to scan
 *
 * Scans the pagetable for dirty bits. Clear those bits and modify the
 * dirty structure with the results. This function may change the
 * dirty-tracking method.
 */
static void vmw_bo_dirty_scan_pagetable(struct vmw_bo *vbo)
{
	struct vmw_bo_dirty *dirty = vbo->dirty;
	pgoff_t offset = drm_vma_node_start(&vbo->tbo.base.vma_node);
	struct address_space *mapping = vbo->tbo.bdev->dev_mapping;
	pgoff_t num_marked;

	num_marked = clean_record_shared_mapping_range
		(mapping,
		 offset, dirty->bitmap_size,
		 offset, &dirty->bitmap[0],
		 &dirty->start, &dirty->end);
	if (num_marked == 0)
		dirty->change_count++;
	else
		dirty->change_count = 0;

	if (dirty->change_count > VMW_DIRTY_NUM_CHANGE_TRIGGERS) {
		dirty->change_count = 0;
		dirty->method = VMW_BO_DIRTY_MKWRITE;
		wp_shared_mapping_range(mapping,
					offset, dirty->bitmap_size);
		clean_record_shared_mapping_range(mapping,
						  offset, dirty->bitmap_size,
						  offset, &dirty->bitmap[0],
						  &dirty->start, &dirty->end);
	}
}

/**
 * vmw_bo_dirty_scan_mkwrite - Reset the mkwrite dirty-tracking method
 * @vbo: The buffer object to scan
 *
 * Write-protect pages written to so that consecutive write accesses will
 * trigger a call to mkwrite.
 *
 * This function may change the dirty-tracking method.
 */
static void vmw_bo_dirty_scan_mkwrite(struct vmw_bo *vbo)
{
	struct vmw_bo_dirty *dirty = vbo->dirty;
	unsigned long offset = drm_vma_node_start(&vbo->tbo.base.vma_node);
	struct address_space *mapping = vbo->tbo.bdev->dev_mapping;
	pgoff_t num_marked;

	if (dirty->end <= dirty->start)
		return;

	num_marked = wp_shared_mapping_range(vbo->tbo.bdev->dev_mapping,
					     dirty->start + offset,
					     dirty->end - dirty->start);

	if (100UL * num_marked / dirty->bitmap_size >
	    VMW_DIRTY_PERCENTAGE)
		dirty->change_count++;
	else
		dirty->change_count = 0;

	if (dirty->change_count > VMW_DIRTY_NUM_CHANGE_TRIGGERS) {
		pgoff_t start = 0;
		pgoff_t end = dirty->bitmap_size;

		dirty->method = VMW_BO_DIRTY_PAGETABLE;
		clean_record_shared_mapping_range(mapping, offset, end, offset,
						  &dirty->bitmap[0],
						  &start, &end);
		bitmap_clear(&dirty->bitmap[0], 0, dirty->bitmap_size);
		if (dirty->start < dirty->end)
			bitmap_set(&dirty->bitmap[0], dirty->start,
				   dirty->end - dirty->start);
		dirty->change_count = 0;
	}
}

/**
 * vmw_bo_dirty_scan - Scan for dirty pages and add them to the dirty
 * tracking structure
 * @vbo: The buffer object to scan
 *
 * This function may change the dirty tracking method.
 */
void vmw_bo_dirty_scan(struct vmw_bo *vbo)
{
	struct vmw_bo_dirty *dirty = vbo->dirty;

	if (dirty->method == VMW_BO_DIRTY_PAGETABLE)
		vmw_bo_dirty_scan_pagetable(vbo);
	else
		vmw_bo_dirty_scan_mkwrite(vbo);
}

/**
 * vmw_bo_dirty_pre_unmap - write-protect and pick up dirty pages before
 * an unmap_mapping_range operation.
 * @vbo: The buffer object,
 * @start: First page of the range within the buffer object.
 * @end: Last page of the range within the buffer object + 1.
 *
 * If we're using the _PAGETABLE scan method, we may leak dirty pages
 * when calling unmap_mapping_range(). This function makes sure we pick
 * up all dirty pages.
 */
static void vmw_bo_dirty_pre_unmap(struct vmw_bo *vbo,
				   pgoff_t start, pgoff_t end)
{
	struct vmw_bo_dirty *dirty = vbo->dirty;
	unsigned long offset = drm_vma_node_start(&vbo->tbo.base.vma_node);
	struct address_space *mapping = vbo->tbo.bdev->dev_mapping;

	if (dirty->method != VMW_BO_DIRTY_PAGETABLE || start >= end)
		return;

	wp_shared_mapping_range(mapping, start + offset, end - start);
	clean_record_shared_mapping_range(mapping, start + offset,
					  end - start, offset,
					  &dirty->bitmap[0], &dirty->start,
					  &dirty->end);
}

/**
 * vmw_bo_dirty_unmap - Clear all ptes pointing to a range within a bo
 * @vbo: The buffer object,
 * @start: First page of the range within the buffer object.
 * @end: Last page of the range within the buffer object + 1.
 *
 * This is similar to ttm_bo_unmap_virtual() except it takes a subrange.
 */
void vmw_bo_dirty_unmap(struct vmw_bo *vbo,
			pgoff_t start, pgoff_t end)
{
	unsigned long offset = drm_vma_node_start(&vbo->tbo.base.vma_node);
	struct address_space *mapping = vbo->tbo.bdev->dev_mapping;

	vmw_bo_dirty_pre_unmap(vbo, start, end);
	unmap_shared_mapping_range(mapping, (offset + start) << PAGE_SHIFT,
				   (loff_t) (end - start) << PAGE_SHIFT);
}

/**
 * vmw_bo_dirty_add - Add a dirty-tracking user to a buffer object
 * @vbo: The buffer object
 *
 * This function registers a dirty-tracking user to a buffer object.
 * A user can be for example a resource or a vma in a special user-space
 * mapping.
 *
 * Return: Zero on success, -ENOMEM on memory allocation failure.
 */
int vmw_bo_dirty_add(struct vmw_bo *vbo)
{
	struct vmw_bo_dirty *dirty = vbo->dirty;
	pgoff_t num_pages = PFN_UP(vbo->tbo.resource->size);
	size_t size;
	int ret;

	if (dirty) {
		dirty->ref_count++;
		return 0;
	}

	size = sizeof(*dirty) + BITS_TO_LONGS(num_pages) * sizeof(long);
	dirty = kvzalloc(size, GFP_KERNEL);
	if (!dirty) {
		ret = -ENOMEM;
		goto out_no_dirty;
	}

	dirty->bitmap_size = num_pages;
	dirty->start = dirty->bitmap_size;
	dirty->end = 0;
	dirty->ref_count = 1;
	if (num_pages < PAGE_SIZE / sizeof(pte_t)) {
		dirty->method = VMW_BO_DIRTY_PAGETABLE;
	} else {
		struct address_space *mapping = vbo->tbo.bdev->dev_mapping;
		pgoff_t offset = drm_vma_node_start(&vbo->tbo.base.vma_node);

		dirty->method = VMW_BO_DIRTY_MKWRITE;

		/* Write-protect and then pick up already dirty bits */
		wp_shared_mapping_range(mapping, offset, num_pages);
		clean_record_shared_mapping_range(mapping, offset, num_pages,
						  offset,
						  &dirty->bitmap[0],
						  &dirty->start, &dirty->end);
	}

	vbo->dirty = dirty;

	return 0;

out_no_dirty:
	return ret;
}

/**
 * vmw_bo_dirty_release - Release a dirty-tracking user from a buffer object
 * @vbo: The buffer object
 *
 * This function releases a dirty-tracking user from a buffer object.
 * If the reference count reaches zero, then the dirty-tracking object is
 * freed and the pointer to it cleared.
 *
 * Return: Zero on success, -ENOMEM on memory allocation failure.
 */
void vmw_bo_dirty_release(struct vmw_bo *vbo)
{
	struct vmw_bo_dirty *dirty = vbo->dirty;

	if (dirty && --dirty->ref_count == 0) {
		kvfree(dirty);
		vbo->dirty = NULL;
	}
}

/**
 * vmw_bo_dirty_transfer_to_res - Pick up a resource's dirty region from
 * its backing mob.
 * @res: The resource
 *
 * This function will pick up all dirty ranges affecting the resource from
 * it's backup mob, and call vmw_resource_dirty_update() once for each
 * range. The transferred ranges will be cleared from the backing mob's
 * dirty tracking.
 */
void vmw_bo_dirty_transfer_to_res(struct vmw_resource *res)
{
	struct vmw_bo *vbo = res->guest_memory_bo;
	struct vmw_bo_dirty *dirty = vbo->dirty;
	pgoff_t start, cur, end;
	unsigned long res_start = res->guest_memory_offset;
	unsigned long res_end = res->guest_memory_offset + res->guest_memory_size;

	WARN_ON_ONCE(res_start & ~PAGE_MASK);
	res_start >>= PAGE_SHIFT;
	res_end = DIV_ROUND_UP(res_end, PAGE_SIZE);

	if (res_start >= dirty->end || res_end <= dirty->start)
		return;

	cur = max(res_start, dirty->start);
	res_end = max(res_end, dirty->end);
	while (cur < res_end) {
		unsigned long num;

		start = find_next_bit(&dirty->bitmap[0], res_end, cur);
		if (start >= res_end)
			break;

		end = find_next_zero_bit(&dirty->bitmap[0], res_end, start + 1);
		cur = end + 1;
		num = end - start;
		bitmap_clear(&dirty->bitmap[0], start, num);
		vmw_resource_dirty_update(res, start, end);
	}

	if (res_start <= dirty->start && res_end > dirty->start)
		dirty->start = res_end;
	if (res_start < dirty->end && res_end >= dirty->end)
		dirty->end = res_start;
}

void vmw_bo_dirty_clear(struct vmw_bo *vbo)
{
	struct vmw_bo_dirty *dirty = vbo->dirty;
	pgoff_t start, cur, end;
	unsigned long res_start = 0;
	unsigned long res_end = vbo->tbo.base.size;

	WARN_ON_ONCE(res_start & ~PAGE_MASK);
	res_start >>= PAGE_SHIFT;
	res_end = DIV_ROUND_UP(res_end, PAGE_SIZE);

	if (res_start >= dirty->end || res_end <= dirty->start)
		return;

	cur = max(res_start, dirty->start);
	res_end = max(res_end, dirty->end);
	while (cur < res_end) {
		unsigned long num;

		start = find_next_bit(&dirty->bitmap[0], res_end, cur);
		if (start >= res_end)
			break;

		end = find_next_zero_bit(&dirty->bitmap[0], res_end, start + 1);
		cur = end + 1;
		num = end - start;
		bitmap_clear(&dirty->bitmap[0], start, num);
	}

	if (res_start <= dirty->start && res_end > dirty->start)
		dirty->start = res_end;
	if (res_start < dirty->end && res_end >= dirty->end)
		dirty->end = res_start;
}

/**
 * vmw_bo_dirty_clear_res - Clear a resource's dirty region from
 * its backing mob.
 * @res: The resource
 *
 * This function will clear all dirty ranges affecting the resource from
 * it's backup mob's dirty tracking.
 */
void vmw_bo_dirty_clear_res(struct vmw_resource *res)
{
	unsigned long res_start = res->guest_memory_offset;
	unsigned long res_end = res->guest_memory_offset + res->guest_memory_size;
	struct vmw_bo *vbo = res->guest_memory_bo;
	struct vmw_bo_dirty *dirty = vbo->dirty;

	res_start >>= PAGE_SHIFT;
	res_end = DIV_ROUND_UP(res_end, PAGE_SIZE);

	if (res_start >= dirty->end || res_end <= dirty->start)
		return;

	res_start = max(res_start, dirty->start);
	res_end = min(res_end, dirty->end);
	bitmap_clear(&dirty->bitmap[0], res_start, res_end - res_start);

	if (res_start <= dirty->start && res_end > dirty->start)
		dirty->start = res_end;
	if (res_start < dirty->end && res_end >= dirty->end)
		dirty->end = res_start;
}

vm_fault_t vmw_bo_vm_mkwrite(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
	struct ttm_buffer_object *bo = (struct ttm_buffer_object *)
	    vma->vm_private_data;
	vm_fault_t ret;
	unsigned long page_offset;
	unsigned int save_flags;
	struct vmw_bo *vbo = to_vmw_bo(&bo->base);

	/*
	 * mkwrite() doesn't handle the VM_FAULT_RETRY return value correctly.
	 * So make sure the TTM helpers are aware.
	 */
	save_flags = vmf->flags;
	vmf->flags &= ~FAULT_FLAG_ALLOW_RETRY;
	ret = ttm_bo_vm_reserve(bo, vmf);
	vmf->flags = save_flags;
	if (ret)
		return ret;

	page_offset = vmf->pgoff - drm_vma_node_start(&bo->base.vma_node);
	if (unlikely(page_offset >= PFN_UP(bo->resource->size))) {
		ret = VM_FAULT_SIGBUS;
		goto out_unlock;
	}

	if (vbo->dirty && vbo->dirty->method == VMW_BO_DIRTY_MKWRITE &&
	    !test_bit(page_offset, &vbo->dirty->bitmap[0])) {
		struct vmw_bo_dirty *dirty = vbo->dirty;

		__set_bit(page_offset, &dirty->bitmap[0]);
		dirty->start = min(dirty->start, page_offset);
		dirty->end = max(dirty->end, page_offset + 1);
	}

out_unlock:
	dma_resv_unlock(bo->base.resv);
	return ret;
}

vm_fault_t vmw_bo_vm_fault(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
	struct ttm_buffer_object *bo = (struct ttm_buffer_object *)
	    vma->vm_private_data;
	struct vmw_bo *vbo = to_vmw_bo(&bo->base);
	pgoff_t num_prefault;
	pgprot_t prot;
	vm_fault_t ret;

	ret = ttm_bo_vm_reserve(bo, vmf);
	if (ret)
		return ret;

	num_prefault = (vma->vm_flags & VM_RAND_READ) ? 1 :
		TTM_BO_VM_NUM_PREFAULT;

	if (vbo->dirty) {
		pgoff_t allowed_prefault;
		unsigned long page_offset;

		page_offset = vmf->pgoff -
			drm_vma_node_start(&bo->base.vma_node);
		if (page_offset >= PFN_UP(bo->resource->size) ||
		    vmw_resources_clean(vbo, page_offset,
					page_offset + PAGE_SIZE,
					&allowed_prefault)) {
			ret = VM_FAULT_SIGBUS;
			goto out_unlock;
		}

		num_prefault = min(num_prefault, allowed_prefault);
	}

	/*
	 * If we don't track dirty using the MKWRITE method, make sure
	 * sure the page protection is write-enabled so we don't get
	 * a lot of unnecessary write faults.
	 */
	if (vbo->dirty && vbo->dirty->method == VMW_BO_DIRTY_MKWRITE)
		prot = vm_get_page_prot(vma->vm_flags & ~VM_SHARED);
	else
		prot = vm_get_page_prot(vma->vm_flags);

	ret = ttm_bo_vm_fault_reserved(vmf, prot, num_prefault);
	if (ret == VM_FAULT_RETRY && !(vmf->flags & FAULT_FLAG_RETRY_NOWAIT))
		return ret;

out_unlock:
	dma_resv_unlock(bo->base.resv);

	return ret;
}
