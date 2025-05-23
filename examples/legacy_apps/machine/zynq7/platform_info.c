/*
 * Copyright (c) 2014, Mentor Graphics Corporation. All rights reserved.
 * Copyright (c) 2017 - 2018 Xilinx, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**************************************************************************
 * FILE NAME
 *
 *       platform_info.c
 *
 * DESCRIPTION
 *
 *       This file define platform specific data and implements APIs to set
 *       platform specific information for OpenAMP.
 *
 **************************************************************************/

#include <openamp/remoteproc.h>
#include <openamp/rpmsg_virtio.h>
#include <metal/atomic.h>
#include <metal/device.h>
#include <metal/io.h>
#include <metal/irq.h>
#include <metal/sys.h>
#include "platform_info.h"
#include "rsc_table.h"
#include <errno.h>
#include <xparameters.h>

/* Another APU core ID. In this demo, the other APU core is 0. */
#define A9_CPU_ID	0UL

/* scugic device, used to raise soft irq */
#define SCUGIC_DEV_NAME	"scugic_dev"
#define SCUGIC_BUS_NAME	"generic"

/* scugic base address */
#define SCUGIC_PERIPH_BASE	0xF8F00000
#define SCUGIC_DIST_BASE	(SCUGIC_PERIPH_BASE + 0x00001000)

#define _rproc_wait() asm volatile("wfi")

/*
 * processor operations for hil_proc for A9. It defines
 * notification operation and remote processor management.
 */
extern const struct remoteproc_ops zynq_a9_proc_ops;
static metal_phys_addr_t scugic_phys_addr = SCUGIC_DIST_BASE;
struct metal_device scugic_device = {
	.name = SCUGIC_DEV_NAME,
	.bus = NULL,
	.num_regions = 1,
	.regions = {
		{
			.virt = (void *)SCUGIC_DIST_BASE,
			.physmap = &scugic_phys_addr,
			.size = 0x1000,
			.page_shift = -1UL,
			.page_mask = -1UL,
			.mem_flags = DEVICE_MEMORY,
			.ops = {NULL},
		},
	},
	.node = {NULL},
	.irq_num = 0,
	.irq_info = NULL,
};

/* Remoteproc private data */
static struct remoteproc_priv rproc_priv = {
	.gic_name = SCUGIC_DEV_NAME,
	.gic_bus_name = SCUGIC_BUS_NAME,
	.gic_dev = NULL,
	.gic_io = NULL,
	.irq_to_notify = SGI_TO_NOTIFY,
	.irq_notification = SGI_NOTIFICATION,
	/* toggle CPU ID, there is only two CPUs in PS */
	.cpu_id = (~XPAR_CPU_ID) & ZYNQ_CPU_ID_MASK,
};

/* Remoteproc instance */
static struct remoteproc rproc_inst;

/* External functions */
extern int init_system(void);
extern void cleanup_system(void);

static struct remoteproc *
platform_create_proc(int proc_index, int rsc_index)
{
	void *rsc_table;
	int rsc_size;
	int ret;
	metal_phys_addr_t pa;

	(void) proc_index;
	rsc_table = get_resource_table(rsc_index, &rsc_size);

	/* Register IPI device */
	(void)metal_register_generic_device(&scugic_device);
	/* Initialize remoteproc instance */
	if (!remoteproc_init(&rproc_inst, &zynq_a9_proc_ops, &rproc_priv))
		return NULL;

	/*
	 * Mmap shared memories
	 * Or shall we constraint that they will be set as carved out
	 * in the resource table?
	 */
	/* mmap resource table */
	pa = (metal_phys_addr_t)rsc_table;
	(void *)remoteproc_mmap(&rproc_inst, &pa,
				NULL, rsc_size,
				NORM_NONCACHE | STRONG_ORDERED,
				&rproc_inst.rsc_io);
	/* mmap shared memory */
	pa = SHARED_MEM_PA;
	(void *)remoteproc_mmap(&rproc_inst, &pa,
				NULL, SHARED_MEM_SIZE,
				NORM_NONCACHE | STRONG_ORDERED,
				NULL);

	/* parse resource table to remoteproc */
	ret = remoteproc_set_rsc_table(&rproc_inst, rsc_table, rsc_size);
	if (ret) {
		xil_printf("Failed to initialize remoteproc\r\n");
		remoteproc_remove(&rproc_inst);
		return NULL;
	}
	xil_printf("Initialize remoteproc successfully.\r\n");

	return &rproc_inst;
}

int platform_init(int argc, char *argv[], void **platform)
{
	unsigned long proc_id = 0;
	unsigned long rsc_id = 0;
	struct remoteproc *rproc;

	if (!platform) {
		xil_printf("Failed to initialize platform,"
			   "NULL pointer to store platform data.\r\n");
		return -EINVAL;
	}
	/* Initialize HW system components */
	init_system();

	if (argc >= 2) {
		proc_id = strtoul(argv[1], NULL, 0);
	}

	if (argc >= 3) {
		rsc_id = strtoul(argv[2], NULL, 0);
	}

	rproc = platform_create_proc(proc_id, rsc_id);
	if (!rproc) {
		xil_printf("Failed to create remoteproc device.\r\n");
		return -EINVAL;
	}
	*platform = rproc;
	return 0;
}

/* RPMsg virtio shared buffer pool */
static struct rpmsg_virtio_shm_pool shpool;

struct  rpmsg_device *
platform_create_rpmsg_vdev(void *platform, unsigned int vdev_index,
			   unsigned int role,
			   void (*rst_cb)(struct virtio_device *vdev),
			   rpmsg_ns_bind_cb ns_bind_cb)
{
	struct remoteproc *rproc = platform;
	struct rpmsg_virtio_device *rpmsg_vdev;
	struct virtio_device *vdev;
	void *shbuf;
	struct metal_io_region *shbuf_io;
	int ret;

	rpmsg_vdev = metal_allocate_memory(sizeof(*rpmsg_vdev));
	if (!rpmsg_vdev)
		return NULL;
	shbuf_io = remoteproc_get_io_with_pa(rproc, SHARED_MEM_PA);
	if (!shbuf_io)
		goto err1;
	shbuf = metal_io_phys_to_virt(shbuf_io,
				      SHARED_MEM_PA + SHARED_BUF_OFFSET);

	xil_printf("creating remoteproc virtio\r\n");
	/* TODO: can we have a wrapper for the following two functions? */
	vdev = remoteproc_create_virtio(rproc, vdev_index, role, rst_cb);
	if (!vdev) {
		xil_printf("failed remoteproc_create_virtio\r\n");
		goto err1;
	}

	xil_printf("initializing rpmsg vdev\r\n");
	if (role == VIRTIO_DEV_DRIVER) {
		/* Only RPMsg virtio driver needs to initialize the shared buffers pool */
		rpmsg_virtio_init_shm_pool(&shpool, shbuf,
					   (SHARED_MEM_SIZE -
					    SHARED_BUF_OFFSET));

		/* RPMsg virtio device can set shared buffers pool argument to NULL */
		ret =  rpmsg_init_vdev(rpmsg_vdev, vdev, ns_bind_cb,
				       shbuf_io, &shpool);
	} else {
		ret =  rpmsg_init_vdev(rpmsg_vdev, vdev, ns_bind_cb,
				       shbuf_io, NULL);
	}
	if (ret) {
		xil_printf("failed rpmsg_init_vdev\r\n");
		goto err2;
	}
	xil_printf("initializing rpmsg vdev\r\n");
	return rpmsg_virtio_get_rpmsg_device(rpmsg_vdev);
err2:
	remoteproc_remove_virtio(rproc, vdev);
err1:
	metal_free_memory(rpmsg_vdev);
	return NULL;
}

int platform_poll(void *priv)
{
	struct remoteproc *rproc = priv;
	struct remoteproc_priv *prproc;
	unsigned int flags;

	prproc = rproc->priv;
	while(1) {
		flags = metal_irq_save_disable();
		if (!(atomic_flag_test_and_set(&prproc->nokick))) {
			metal_irq_restore_enable(flags);
			remoteproc_get_notification(rproc, RSC_NOTIFY_ID_ANY);
			break;
		}
		_rproc_wait();
		metal_irq_restore_enable(flags);
	}
	return 0;
}

void platform_release_rpmsg_vdev(struct rpmsg_device *rpdev, void *platform)
{
	struct rpmsg_virtio_device *rpvdev;
	struct virtio_device *vdev;
	struct remoteproc *rproc;

	rpvdev = metal_container_of(rpdev, struct rpmsg_virtio_device, rdev);
	rproc = platform;
	vdev = pvdev->vdev;

	rpmsg_deinit_vdev(rpvdev);
	remoteproc_remove_virtio(rproc, vdev);
	metal_free_memory(rpvdev);

}

void platform_cleanup(void *platform)
{
	struct remoteproc *rproc = platform;

	if (rproc)
		remoteproc_remove(rproc);
	cleanup_system();
}
