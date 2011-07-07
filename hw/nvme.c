/*
 * INTEL CONFIDENTIAL
 * Copyright 2011 - 2011 Intel Corporation All Rights Reserved.
 * The source code contained or described herein and all documents related to
 * the source code ("Material") are owned by Intel Corporation or its
 * suppliers or licensors. Title to the Material remains with Intel Corporation
 * or its suppliers and licensors. The Material contains trade secrets and
 * proprietary and confidential information of Intel or its suppliers and
 * licensors. The Material is protected by worldwide copyright and trade secret
 * laws and treaty provisions. No part of the Material may be used, copied,
 * reproduced, modified, published, uploaded, posted, transmitted, distributed,
 * or disclosed in any way without Intel's prior express written permission.
 * No license under any patent, copyright, trade secret or other intellectual
 * property right is granted to or conferred upon you by disclosure or delivery
 * of the Materials, either expressly, by implication, inducement, estoppel or
 * otherwise. Any license under such intellectual property rights must be
 * express and approved by Intel in writing.
 */

#include "nvme.h"
#include "nvme_debug.h"

static const VMStateDescription vmstate_nvme = {
	.name = "nvme",
	.version_id = 1,
};

#define NVME_THREADED

/* Mutex for exclusive access to NVMEState	*/
pthread_mutex_t nvme_mutex = PTHREAD_MUTEX_INITIALIZER;

static void clear_nvme_device(NVMEState *n);

static void process_reg_writel(NVMEState *n, target_phys_addr_t addr,
								 uint32_t val)
{
	//printf("%s(): Called! \n",__func__);
	if (addr < NVME_SQ0TDBL) {
		switch (addr) {
		case NVME_INTMS:
			break;
		case NVME_INTMC:
			break;
		case NVME_CC:
			/* Check if admin queues are ready to use */
			/* Check enable bit CC.EN */
			/* TODO add support for other CC properties */
			if (val & 1) {
				if (n->cq[ACQ_ID].dma_addr &&
				    n->sq[ASQ_ID].dma_addr &&
				   (!nvme_open_storage_file(n))) {
					n->cstatus.rdy = 1;
					n->cq[ACQ_ID].phase_tag = 1;
				}
			} else {
				clear_nvme_device(n);
			}
			break;
		case NVME_AQA:
			n->sq[ASQ_ID].size = val & 0xfff;
			n->cq[ACQ_ID].size = (val >> 16) & 0xfff;
/*
			printf("--->ASQS: %hu, ACQS: %hu\n",
					n->sq[ASQ_ID].size, n->cq[ACQ_ID].size);
*/
			break;
		case NVME_ASQ:
			n->sq[ASQ_ID].dma_addr |= val;
			break;
		case (NVME_ASQ + 4):
			n->sq[ASQ_ID].dma_addr |=
					(uint64_t)((uint64_t)val << 32);
/*
			printf("--->ASQ: %lu\n",
				 (uint64_t)n->sq[ASQ_ID].dma_addr);
*/
			break;
		case NVME_ACQ:
			n->cq[ACQ_ID].dma_addr |= val;
			break;
		case (NVME_ACQ + 4):
			n->cq[ACQ_ID].dma_addr |=
					(uint64_t)((uint64_t)val <<32);
/*
			printf("--->ACQ: %lu\n",
				(uint64_t)n->cq[ACQ_ID].dma_addr);
*/
			break;
		default:
			/* ERROR */
			break;
		}
	} else {
		/* Doorbell write */
	}
}
#ifndef NVME_THREADED
static uint16_t process_doorbell(NVMEState *n, target_phys_addr_t addr,
								 uint32_t val)
{
	uint32_t tmp = 0;


	/*Check if it is CQ or SQ doorbell */

      tmp = (addr - NVME_SQ0TDBL) / sizeof(uint32_t);
	if (tmp % 2) {
	/* CQ */
		tmp = (addr - NVME_CQ0HDBL) / 8;
		if (tmp > NVME_MAX_QID) {
			printf("Wrong CQ ID: %d\n", tmp);

                        return 1;
		}

		n->cq[tmp].head = val & 0xffff;
	} else {
	/* SQ */
		tmp = (addr - NVME_SQ0TDBL) / 8;
		if (tmp > NVME_MAX_QID) {
			printf("Wrong SQ ID: %d\n", tmp);

                        return 1;
		}


		n->sq[tmp].tail = val & 0xffff;

		do {
				process_sq(n, tmp);
		} while (n->sq[tmp].head != n->sq[tmp].tail);

	}

        return 0;
}

#else
/*********************************************************************
	Function 	:	process_doorbell_thread
	Description	:	Thread for processing Doorbell and SQ commands
	Return Type :	void*
	Arguments	: 	NVMEThread*
*********************************************************************/
void * process_doorbell_thread(NVMEThread* nt)
{
	NVMEState *n ;
	target_phys_addr_t addr;
	uint32_t val;
	uint32_t tmp = 0;
	//char msg[50];
	LOG_NORM("Came Inside Threaded Arch")
	pthread_detach(pthread_self());

	n = nt->n;
	addr = nt->addr;
	val = nt->val;

	/*Check if it is CQ or SQ doorbell */

    tmp = (addr - NVME_SQ0TDBL) / sizeof(uint32_t);

    pthread_mutex_lock(&nvme_mutex);
    if (tmp % 2) {
	/* CQ */
		tmp = (addr - NVME_CQ0HDBL) / 8;
		if (tmp > NVME_MAX_QID) {
			LOG_NORM("Wrong CQ ID: %d\n", tmp);
			pthread_mutex_unlock(&nvme_mutex);
			return (NULL);
		}

		n->cq[tmp].head = val & 0xffff;
	} else {
	/* SQ */
		tmp = (addr - NVME_SQ0TDBL) / 8;
		if (tmp > NVME_MAX_QID) {
			LOG_NORM("Wrong SQ ID: %d\n", tmp);
			pthread_mutex_unlock(&nvme_mutex);
			return (NULL);
		}


		n->sq[tmp].tail = val & 0xffff;

		do {
						process_sq(n, tmp);
		} while (n->sq[tmp].head != n->sq[tmp].tail);
	}

	pthread_mutex_unlock(&nvme_mutex);
	free(nt);
	LOG_NORM("Finished Threaded Arch")
	return(NULL);
}
#endif
/* Write 1 Byte at addr/register */
static void nvme_mmio_writeb(void *opaque, target_phys_addr_t addr,
								 uint32_t val)
{
	NVMEState *n = opaque;

	printf("%s(): addr = 0x%08x, val = 0x%08x\n",
					 __func__, (unsigned)addr, val);
	(void)n;
}

/* Write 2 Bytes at addr/register
 * 16 bit - can filter here doorbell write !!!
 */
static void nvme_mmio_writew(void *opaque, target_phys_addr_t addr,
								 uint32_t val)
{
	NVMEState *n = opaque;
	NVMEThread * nt = (NVMEThread *) malloc(1 * sizeof (NVMEThread));
	printf("%s(): addr = 0x%08x, val = 0x%08x\n", __func__,
		(unsigned)addr, val);

	/* Check if some registry was written */
	if (addr < NVME_SQ0TDBL) {
		process_reg_writel(n, addr, val);
		return;
	}

	/* Doorbell area is  between NVME_SQ0TDBL and NVME_CQMAXHDBL */
	if (addr >= NVME_SQ0TDBL && addr <= NVME_CQMAXHDBL){
		#ifdef NVME_THREADED
			nt->n = n;
			nt->addr = addr;
			nt->val = val;
			process_doorbell_thread(nt);
		#else
			process_doorbell(n, addr, val);
		#endif
	}
	return ;
}

/* Write 4 Bytes at addr/register */
static void nvme_mmio_writel(void *opaque, target_phys_addr_t addr,
								 uint32_t val)
{
	NVMEState *n = opaque;
	NVMEThread * nt = (NVMEThread *) malloc(1 * sizeof (NVMEThread));

/*
	printf("%s(): addr = 0x%08x, val = 0x%08x\n", __func__,
		(unsigned)addr, val);
*/
	/* Check if some registry was written */
	if (addr < NVME_SQ0TDBL) {
		process_reg_writel(n, addr, val);
		return;
	}

	/* Doorbell area is  between NVME_SQ0TDBL and NVME_CQMAXHDBL */
	if (addr >= NVME_SQ0TDBL && addr <= NVME_CQMAXHDBL){
		#ifdef NVME_THREADED
			nt->n = n;
			nt->addr = addr;
			nt->val = val;
			process_doorbell_thread(nt);
		#else
			process_doorbell(n, addr, val);
		#endif

	}
	return;
}
/* Read 1 Byte from addr/register */
static uint32_t nvme_mmio_readb(void *opaque, target_phys_addr_t addr)
{
	NVMEState *n = opaque;
	printf("%s(): addr = 0x%08x\n", __func__, (unsigned)addr);
	(void)n;
	return 0;
}

/* Read 2 Bytes from addr/register */
static uint32_t nvme_mmio_readw(void *opaque, target_phys_addr_t addr)
{
	NVMEState *n = opaque;
	printf("%s(): addr = 0x%08x\n", __func__, (unsigned)addr);
	(void)n;
	return 0;
}

/* Read 4 Bytes from addr/register */
static uint32_t nvme_mmio_readl(void *opaque, target_phys_addr_t addr)
{
	NVMEState *n = opaque;
	printf("%s(): addr = 0x%08x\n", __func__, (unsigned)addr);

	switch (addr) {
	case NVME_CTST:
		printf("%s(): Status required.\n", __func__);
		if ((n->cstatus.rdy) &&
		   (n->sq[ASQ_ID].dma_addr && n->cq[ACQ_ID].dma_addr)) {
			printf("%s(): ADM QUEUES ARE SET. Return 1.\n",
								 __func__);
			return 1;
		}
		break;
	case NVME_CMD_SS:
		/* Offset of doorbell region */
		return NVME_SQ0TDBL;
		break;

	default:
		printf("Register not supported. offset: 0x%x\n",
							(unsigned int) addr);
		break;
	}

	return 0;
}

static CPUWriteMemoryFunc * const nvme_mmio_write[] = {
	nvme_mmio_writeb,
	nvme_mmio_writew,
	nvme_mmio_writel,
};

static CPUReadMemoryFunc * const nvme_mmio_read[] = {
	nvme_mmio_readb,
	nvme_mmio_readw,
	nvme_mmio_readl,
};

static void nvme_mmio_map(PCIDevice *pci_dev, int reg_num, pcibus_t addr,
							pcibus_t size, int type)
{
	NVMEState *n = DO_UPCAST(NVMEState, dev, pci_dev);

	if (reg_num) {
		printf("Only bar0 is allowed! reg_num: %d\n",reg_num);
	}
	/* Is this hacking? */
	/* BAR 0 is shared: Registry, doorbells and MSI-X. Only
	 * registry and doorbell part of BAR0 should be handled
	 * by nvme mmio functions.
	 * The MSI-X part of BAR0 should be mapped by MSI-X functions.
	 * The msix_init function changes the bar size to add its
	 * tables to it. */
	cpu_register_physical_memory(addr, n->bar0_size, n->mmio_index);
	n->bar0 = (void*)addr;

	/* Let the MSI-X part handle the MSI-X table.  */
	msix_mmio_map(pci_dev, reg_num, addr, size, type);
}

static void nvme_set_registry(NVMEState *n)
{
	if (!n)
		return;

	n->ctrlcap.mpsmax = 0; /* 4kB*/
	n->ctrlcap.mpsmin = 0; /* 4kB*/
	n->ctrlcap.css = 1; /* set bit 37*/
	n->ctrlcap.to = 0xf; /* maximum possible timeout. */
	n->ctrlcap.ams = 0; /* TBD */
	n->ctrlcap.cqr = 1; /* Controller Requires contiguous memory. */
	/* each command is 64 bytes. That gives 4kB */
	n->ctrlcap.mqes = NVME_MAX_QUEUE_SIZE - 1;

	n->cconf.mps = 0;

	n->ctrlv.mjr = 1;
	n->ctrlv.mnr = 0;

	n->cstatus.rdy = 0;
	n->cstatus.cfs = 0;
	n->cstatus.shst = 0;


	n->feature.number_of_queues = ((NVME_MAX_QID - 1) << 16)
							 | (NVME_MAX_QID - 1);
//	printf("n->feature.number_of_queues %x\n", n->feature.number_of_queues);

	return;
}

static void clear_nvme_device(NVMEState *n)
{
	uint32_t i = 0;

	if(!n)
		return;

	nvme_close_storage_file(n);
	nvme_set_registry(n);

	for (i = 0; i < NVME_MAX_QID; i++) {
		memset(&(n->sq[i]), 0, sizeof(NVMEIOSQueue));
		memset(&(n->cq[i]), 0, sizeof(NVMEIOCQueue));
	}
}

/* FIXME */
static void do_nvme_reset(NVMEState *n)
{
	(void)n;
}

/* Qemu Device Reset. */
static void qdev_nvme_reset(DeviceState *dev)
{
	NVMEState *n = DO_UPCAST(NVMEState, dev.qdev, dev);
	do_nvme_reset(n);
}

/* Initialization routine
 *
 * FIXME: Make any initialization here or when
 *        controller receives 'enable' bit?
 * */
static int pci_nvme_init(PCIDevice *pci_dev)
{
	NVMEState *n = DO_UPCAST(NVMEState, dev, pci_dev);
	uint8_t *pci_conf = NULL;
	uint32_t ret;

        pci_conf = n->dev.config;


	pci_config_set_vendor_id(pci_conf, PCI_VENDOR_ID_INTEL);
	/* Device id is fake  */
	pci_config_set_device_id(pci_conf, NVME_DEV_ID);

	/* FIXME: Class is 24 bits, but this function takes 16 bits.*/
	/* STORAGE EXPRESS is not yet a standard. */
	pci_config_set_class(pci_conf, PCI_CLASS_STORAGE_EXPRESS >> 8);
	/* FIXME: is it ok? */
	pci_config_set_prog_interface(pci_conf,
				0xf & PCI_CLASS_STORAGE_EXPRESS);

	/* FIXME: What with the rest of PCI fields? Capabilities? */

	/*other notation:  pci_config[OFFSET] = 0xff; */

	/* FIXME: Is it OK? Interrupt PIN A */
	printf("%s(): Setting PCI Interrupt PIN A\n",__func__);
	pci_conf[PCI_INTERRUPT_PIN] = 1;

	n->nvectors = NVME_MSIX_NVECTORS;
	n->bar0_size = NVME_REG_SIZE;
	printf("%s(): Reg0 size %u, nvectors: %hu\n",__func__,
						n->bar0_size, n->nvectors);
	ret = msix_init((struct PCIDevice*)&n->dev,
						 n->nvectors, 0, n->bar0_size);
	if (ret) {
		printf("%s(): PCI MSI-X Failed\n",__func__);
		//???
		return -1;
	}
	printf("%s(): PCI MSI-X Initialized\n",__func__);
	/* NVMe is Little Endian. */
	n->mmio_index = cpu_register_io_memory(nvme_mmio_read, nvme_mmio_write,
						n, DEVICE_LITTLE_ENDIAN);

	/* Register BAR 0 (and bar 1 as it is 64bit). */
	pci_register_bar((struct PCIDevice *)&n->dev,
			0, msix_bar_size((struct PCIDevice*)&n->dev),
			(PCI_BASE_ADDRESS_SPACE_MEMORY |
			PCI_BASE_ADDRESS_MEM_TYPE_64),
			nvme_mmio_map);

	for (ret = 0; ret < n->nvectors; ret++)
		msix_vector_use(&n->dev, ret);

	nvme_set_registry(n);

	for (ret = 0; ret < NVME_MAX_QID; ret++) {
		memset(&(n->sq[ret]), 0, sizeof(NVMEIOSQueue));
		memset(&(n->cq[ret]), 0, sizeof(NVMEIOCQueue));
	}

        n->fd = -1;
        n->mapping_addr = NULL;
	return 0;
}

static int pci_nvme_uninit(PCIDevice *pci_dev)
{
	NVMEState *n = DO_UPCAST(NVMEState, dev, pci_dev);

        nvme_close_storage_file(n);

	return 0;
}

/* FIXME: Update this field later.*/
static PCIDeviceInfo nvme_info = {
	.qdev.name = "nvme",
	.qdev.desc = "Non-Volatile Memory Express",
	.qdev.size = sizeof(NVMEState),
	.qdev.vmsd = &vmstate_nvme,
	.qdev.reset = qdev_nvme_reset,
	.init = pci_nvme_init,
	.exit = pci_nvme_uninit,
	.qdev.props = (Property[]) {
		DEFINE_PROP_END_OF_LIST(),
	}
};

/* Framework */
static void nvme_register_devices(void)
{
	pci_qdev_register(&nvme_info);
}

device_init(nvme_register_devices);
