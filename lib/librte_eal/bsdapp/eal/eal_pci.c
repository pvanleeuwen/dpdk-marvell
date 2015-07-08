/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2014 Intel Corporation. All rights reserved.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdarg.h>
#include <errno.h>
#include <dirent.h>
#include <limits.h>
#include <sys/queue.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/pciio.h>
#include <dev/pci/pcireg.h>

#include <rte_interrupts.h>
#include <rte_log.h>
#include <rte_pci.h>
#include <rte_common.h>
#include <rte_launch.h>
#include <rte_memory.h>
#include <rte_memzone.h>
#include <rte_eal.h>
#include <rte_eal_memconfig.h>
#include <rte_per_lcore.h>
#include <rte_lcore.h>
#include <rte_malloc.h>
#include <rte_string_fns.h>
#include <rte_debug.h>
#include <rte_devargs.h>

#include "rte_pci_dev_ids.h"
#include "eal_filesystem.h"
#include "eal_private.h"

/**
 * @file
 * PCI probing under linux
 *
 * This code is used to simulate a PCI probe by parsing information in
 * sysfs. Moreover, when a registered driver matches a device, the
 * kernel driver currently using it is unloaded and replaced by
 * igb_uio module, which is a very minimal userland driver for Intel
 * network card, only providing access to PCI BAR to applications, and
 * enabling bus master.
 */
static struct rte_tailq_elem rte_uio_tailq = {
	.name = "UIO_RESOURCE_LIST",
};
EAL_REGISTER_TAILQ(rte_uio_tailq)

/* unbind kernel driver for this device */
static int
pci_unbind_kernel_driver(struct rte_pci_device *dev __rte_unused)
{
	RTE_LOG(ERR, EAL, "RTE_PCI_DRV_FORCE_UNBIND flag is not implemented "
		"for BSD\n");
	return -ENOTSUP;
}

/* map a particular resource from a file */
static void *
pci_map_resource(void *requested_addr, int fd, off_t offset, size_t size,
		 int additional_flags)
{
	void *mapaddr;

	/* Map the PCI memory resource of device */
	mapaddr = mmap(requested_addr, size, PROT_READ | PROT_WRITE,
			MAP_SHARED | additional_flags, fd, offset);
	if (mapaddr == MAP_FAILED) {
		RTE_LOG(ERR, EAL,
			"%s(): cannot mmap(%d, %p, 0x%lx, 0x%lx): %s (%p)\n",
			__func__, fd, requested_addr,
			(unsigned long)size, (unsigned long)offset,
			strerror(errno), mapaddr);
	} else
		RTE_LOG(DEBUG, EAL, "  PCI memory mapped at %p\n", mapaddr);

	return mapaddr;
}

static int
pci_uio_map_secondary(struct rte_pci_device *dev)
{
	int i, fd;
	struct mapped_pci_resource *uio_res;
	struct mapped_pci_res_list *uio_res_list =
			RTE_TAILQ_CAST(rte_uio_tailq.head, mapped_pci_res_list);

	TAILQ_FOREACH(uio_res, uio_res_list, next) {

		/* skip this element if it doesn't match our PCI address */
		if (rte_eal_compare_pci_addr(&uio_res->pci_addr, &dev->addr))
			continue;

		for (i = 0; i != uio_res->nb_maps; i++) {
			/*
			 * open devname, to mmap it
			 */
			fd = open(uio_res->maps[i].path, O_RDWR);
			if (fd < 0) {
				RTE_LOG(ERR, EAL, "Cannot open %s: %s\n",
					uio_res->maps[i].path, strerror(errno));
				return -1;
			}

			void *mapaddr = pci_map_resource(uio_res->maps[i].addr,
					fd, (off_t)uio_res->maps[i].offset,
					(size_t)uio_res->maps[i].size, 0);
			/* fd is not needed in slave process, close it */
			close(fd);
			if (mapaddr != uio_res->maps[i].addr) {
				RTE_LOG(ERR, EAL,
					"Cannot mmap device resource file %s to address: %p\n",
					uio_res->maps[i].path,
					uio_res->maps[i].addr);
				return -1;
			}
		}
		return 0;
	}

	RTE_LOG(ERR, EAL, "Cannot find resource for device\n");
	return 1;
}

static void
pci_uio_free_resource(struct rte_pci_device *dev,
		struct mapped_pci_resource *uio_res)
{
	rte_free(uio_res);

	if (dev->intr_handle.fd) {
		close(dev->intr_handle.fd);
		dev->intr_handle.fd = -1;
		dev->intr_handle.type = RTE_INTR_HANDLE_UNKNOWN;
	}
}

static int
pci_uio_alloc_resource(struct rte_pci_device *dev,
		struct mapped_pci_resource **uio_res)
{
	char devname[PATH_MAX]; /* contains the /dev/uioX */
	struct rte_pci_addr *loc;

	loc = &dev->addr;

	snprintf(devname, sizeof(devname), "/dev/uio@pci:%u:%u:%u",
			dev->addr.bus, dev->addr.devid, dev->addr.function);

	if (access(devname, O_RDWR) < 0) {
		RTE_LOG(WARNING, EAL, "  "PCI_PRI_FMT" not managed by UIO driver, "
				"skipping\n", loc->domain, loc->bus, loc->devid, loc->function);
		return 1;
	}

	/* save fd if in primary process */
	dev->intr_handle.fd = open(devname, O_RDWR);
	if (dev->intr_handle.fd < 0) {
		RTE_LOG(ERR, EAL, "Cannot open %s: %s\n",
			devname, strerror(errno));
		goto error;
	}
	dev->intr_handle.type = RTE_INTR_HANDLE_UIO;

	/* allocate the mapping details for secondary processes*/
	*uio_res = rte_zmalloc("UIO_RES", sizeof(**uio_res), 0);
	if (*uio_res == NULL) {
		RTE_LOG(ERR, EAL,
			"%s(): cannot store uio mmap details\n", __func__);
		goto error;
	}

	snprintf((*uio_res)->path, sizeof((*uio_res)->path), "%s", devname);
	memcpy(&(*uio_res)->pci_addr, &dev->addr, sizeof((*uio_res)->pci_addr));

	return 0;

error:
	pci_uio_free_resource(dev, *uio_res);
	return -1;
}

static int
pci_uio_map_resource_by_index(struct rte_pci_device *dev, int res_idx,
		struct mapped_pci_resource *uio_res, int map_idx)
{
	int fd;
	char *devname;
	void *mapaddr;
	uint64_t offset;
	uint64_t pagesz;
	struct pci_map *maps;

	maps = uio_res->maps;
	devname = uio_res->path;
	pagesz = sysconf(_SC_PAGESIZE);

	/* allocate memory to keep path */
	maps[map_idx].path = rte_malloc(NULL, strlen(devname) + 1, 0);
	if (maps[map_idx].path == NULL) {
		RTE_LOG(ERR, EAL, "Cannot allocate memory for path: %s\n",
				strerror(errno));
		return -1;
	}

	/*
	 * open resource file, to mmap it
	 */
	fd = open(devname, O_RDWR);
	if (fd < 0) {
		RTE_LOG(ERR, EAL, "Cannot open %s: %s\n",
				devname, strerror(errno));
		goto error;
	}

	/* if matching map is found, then use it */
	offset = res_idx * pagesz;
	mapaddr = pci_map_resource(NULL, fd, (off_t)offset,
			(size_t)dev->mem_resource[res_idx].len, 0);
	close(fd);
	if (mapaddr == MAP_FAILED)
		goto error;

	maps[map_idx].phaddr = dev->mem_resource[res_idx].phys_addr;
	maps[map_idx].size = dev->mem_resource[res_idx].len;
	maps[map_idx].addr = mapaddr;
	maps[map_idx].offset = offset;
	strcpy(maps[map_idx].path, devname);
	dev->mem_resource[res_idx].addr = mapaddr;

	return 0;

error:
	rte_free(maps[map_idx].path);
	return -1;
}

/* map the PCI resource of a PCI device in virtual memory */
static int
pci_uio_map_resource(struct rte_pci_device *dev)
{
	int i, map_idx = 0, ret;
	uint64_t phaddr;
	struct mapped_pci_resource *uio_res = NULL;
	struct mapped_pci_res_list *uio_res_list =
		RTE_TAILQ_CAST(rte_uio_tailq.head, mapped_pci_res_list);

	dev->intr_handle.fd = -1;
	dev->intr_handle.type = RTE_INTR_HANDLE_UNKNOWN;

	/* secondary processes - use already recorded details */
	if (rte_eal_process_type() != RTE_PROC_PRIMARY)
		return pci_uio_map_secondary(dev);

	/* allocate uio resource */
	ret = pci_uio_alloc_resource(dev, &uio_res);
	if (ret)
		return ret;

	/* Map all BARs */
	for (i = 0; i != PCI_MAX_RESOURCE; i++) {
		/* skip empty BAR */
		if ((phaddr = dev->mem_resource[i].phys_addr) == 0)
			continue;

		ret = pci_uio_map_resource_by_index(dev, i,
				uio_res, map_idx);
		if (ret)
			goto error;

		map_idx++;
	}

	uio_res->nb_maps = map_idx;

	TAILQ_INSERT_TAIL(uio_res_list, uio_res, next);

	return 0;
error:
	for (i = 0; i < map_idx; i++)
		rte_free(uio_res->maps[i].path);
	pci_uio_free_resource(dev, uio_res);
	return -1;
}

/* Scan one pci sysfs entry, and fill the devices list from it. */
static int
pci_scan_one(int dev_pci_fd, struct pci_conf *conf)
{
	struct rte_pci_device *dev;
	struct pci_bar_io bar;
	unsigned i, max;

	dev = malloc(sizeof(*dev));
	if (dev == NULL) {
		return -1;
	}

	memset(dev, 0, sizeof(*dev));
	dev->addr.domain = conf->pc_sel.pc_domain;
	dev->addr.bus = conf->pc_sel.pc_bus;
	dev->addr.devid = conf->pc_sel.pc_dev;
	dev->addr.function = conf->pc_sel.pc_func;

	/* get vendor id */
	dev->id.vendor_id = conf->pc_vendor;

	/* get device id */
	dev->id.device_id = conf->pc_device;

	/* get subsystem_vendor id */
	dev->id.subsystem_vendor_id = conf->pc_subvendor;

	/* get subsystem_device id */
	dev->id.subsystem_device_id = conf->pc_subdevice;

	/* TODO: get max_vfs */
	dev->max_vfs = 0;

	/* FreeBSD has no NUMA support (yet) */
	dev->numa_node = 0;

	/* parse resources */
	switch (conf->pc_hdr & PCIM_HDRTYPE) {
	case PCIM_HDRTYPE_NORMAL:
		max = PCIR_MAX_BAR_0;
		break;
	case PCIM_HDRTYPE_BRIDGE:
		max = PCIR_MAX_BAR_1;
		break;
	case PCIM_HDRTYPE_CARDBUS:
		max = PCIR_MAX_BAR_2;
		break;
	default:
		goto skipdev;
	}

	for (i = 0; i <= max; i++) {
		bar.pbi_sel = conf->pc_sel;
		bar.pbi_reg = PCIR_BAR(i);
		if (ioctl(dev_pci_fd, PCIOCGETBAR, &bar) < 0)
			continue;

		dev->mem_resource[i].len = bar.pbi_length;
		if (PCI_BAR_IO(bar.pbi_base)) {
			dev->mem_resource[i].addr = (void *)(bar.pbi_base & ~((uint64_t)0xf));
			continue;
		}
		dev->mem_resource[i].phys_addr = bar.pbi_base & ~((uint64_t)0xf);
	}

	/* device is valid, add in list (sorted) */
	if (TAILQ_EMPTY(&pci_device_list)) {
		TAILQ_INSERT_TAIL(&pci_device_list, dev, next);
	}
	else {
		struct rte_pci_device *dev2 = NULL;
		int ret;

		TAILQ_FOREACH(dev2, &pci_device_list, next) {
			ret = rte_eal_compare_pci_addr(&dev->addr, &dev2->addr);
			if (ret > 0)
				continue;
			else if (ret < 0) {
				TAILQ_INSERT_BEFORE(dev2, dev, next);
				return 0;
			} else { /* already registered */
				dev2->kdrv = dev->kdrv;
				dev2->max_vfs = dev->max_vfs;
				memmove(dev2->mem_resource,
					dev->mem_resource,
					sizeof(dev->mem_resource));
				free(dev);
				return 0;
			}
		}
		TAILQ_INSERT_TAIL(&pci_device_list, dev, next);
	}

	return 0;

skipdev:
	free(dev);
	return 0;
}

/*
 * Scan the content of the PCI bus, and add the devices in the devices
 * list. Call pci_scan_one() for each pci entry found.
 */
static int
pci_scan(void)
{
	int fd;
	unsigned dev_count = 0;
	struct pci_conf matches[16];
	struct pci_conf_io conf_io = {
			.pat_buf_len = 0,
			.num_patterns = 0,
			.patterns = NULL,
			.match_buf_len = sizeof(matches),
			.matches = &matches[0],
	};

	fd = open("/dev/pci", O_RDONLY);
	if (fd < 0) {
		RTE_LOG(ERR, EAL, "%s(): error opening /dev/pci\n", __func__);
		goto error;
	}

	do {
		unsigned i;
		if (ioctl(fd, PCIOCGETCONF, &conf_io) < 0) {
			RTE_LOG(ERR, EAL, "%s(): error with ioctl on /dev/pci: %s\n",
					__func__, strerror(errno));
			goto error;
		}

		for (i = 0; i < conf_io.num_matches; i++)
			if (pci_scan_one(fd, &matches[i]) < 0)
				goto error;

		dev_count += conf_io.num_matches;
	} while(conf_io.status == PCI_GETCONF_MORE_DEVS);

	close(fd);

	RTE_LOG(ERR, EAL, "PCI scan found %u devices\n", dev_count);
	return 0;

error:
	if (fd >= 0)
		close(fd);
	return -1;
}

/*
 * If vendor/device ID match, call the devinit() function of the
 * driver.
 */
int
rte_eal_pci_probe_one_driver(struct rte_pci_driver *dr, struct rte_pci_device *dev)
{
	const struct rte_pci_id *id_table;
	int ret;

	for (id_table = dr->id_table ; id_table->vendor_id != 0; id_table++) {

		/* check if device's identifiers match the driver's ones */
		if (id_table->vendor_id != dev->id.vendor_id &&
				id_table->vendor_id != PCI_ANY_ID)
			continue;
		if (id_table->device_id != dev->id.device_id &&
				id_table->device_id != PCI_ANY_ID)
			continue;
		if (id_table->subsystem_vendor_id != dev->id.subsystem_vendor_id &&
				id_table->subsystem_vendor_id != PCI_ANY_ID)
			continue;
		if (id_table->subsystem_device_id != dev->id.subsystem_device_id &&
				id_table->subsystem_device_id != PCI_ANY_ID)
			continue;

		struct rte_pci_addr *loc = &dev->addr;

		RTE_LOG(DEBUG, EAL, "PCI device "PCI_PRI_FMT" on NUMA socket %i\n",
				loc->domain, loc->bus, loc->devid, loc->function,
				dev->numa_node);

		RTE_LOG(DEBUG, EAL, "  probe driver: %x:%x %s\n", dev->id.vendor_id,
				dev->id.device_id, dr->name);

		/* no initialization when blacklisted, return without error */
		if (dev->devargs != NULL &&
			dev->devargs->type == RTE_DEVTYPE_BLACKLISTED_PCI) {

			RTE_LOG(DEBUG, EAL, "  Device is blacklisted, not initializing\n");
			return 0;
		}

		if (dr->drv_flags & RTE_PCI_DRV_NEED_MAPPING) {
			/* map resources for devices that use igb_uio */
			ret = pci_uio_map_resource(dev);
			if (ret != 0)
				return ret;
		} else if (dr->drv_flags & RTE_PCI_DRV_FORCE_UNBIND &&
		           rte_eal_process_type() == RTE_PROC_PRIMARY) {
			/* unbind current driver */
			if (pci_unbind_kernel_driver(dev) < 0)
				return -1;
		}

		/* reference driver structure */
		dev->driver = dr;

		/* call the driver devinit() function */
		return dr->devinit(dr, dev);
	}
	/* return positive value if driver is not found */
	return 1;
}

/* Init the PCI EAL subsystem */
int
rte_eal_pci_init(void)
{
	TAILQ_INIT(&pci_driver_list);
	TAILQ_INIT(&pci_device_list);

	/* for debug purposes, PCI can be disabled */
	if (internal_config.no_pci)
		return 0;

	if (pci_scan() < 0) {
		RTE_LOG(ERR, EAL, "%s(): Cannot scan PCI bus\n", __func__);
		return -1;
	}
	return 0;
}
