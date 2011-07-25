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
#include "range.h"

static const VMStateDescription vmstate_nvme = {
    .name = "nvme",
    .version_id = 1,
};

/* Mutex for exclusive access to NVMEState */
pthread_mutex_t nvme_mutex = PTHREAD_MUTEX_INITIALIZER;
/* Using the same Thread Identifier for different write threads */
pthread_t th;
/* Variables to set up Scheduling policies for NVME threads */
pthread_attr_t pthread_attr;
struct sched_param sch_param;

/* File Level scope functions */
static void clear_nvme_device(NVMEState *n);
static void pci_space_init(PCIDevice *);
static void nvme_pci_write_config(PCIDevice *, uint32_t, uint32_t, int);
static uint32_t nvme_pci_read_config(PCIDevice *, uint32_t, int);
static inline uint8_t range_covers_reg(uint64_t, uint64_t, uint64_t,
    uint64_t);



/*********************************************************************
    Function     :    process_reg_writel_thread
    Description  :    Thread for processing Register Reads and writes
    Return Type  :    void *
    Arguments    :    NVMEThread *
*********************************************************************/
void *process_reg_writel_thread(void *nt)
{
    NVMEState *n ;
    target_phys_addr_t addr;
    uint32_t val;

    pthread_detach(pthread_self());

    NVMEThread * ntp = (NVMEThread *) nt;
    /* Keeping Local copy of Addr and Value Sent    */
    addr = ntp->addr;
    val = ntp->val;

    if (addr < NVME_SQ0TDBL) {
        pthread_mutex_lock(&nvme_mutex);
        LOG_DBG("Executing Thread %s() with ID : %d with Addr: 0x%08x",
            __func__, (int)pthread_self(), (unsigned)addr);
        /* Re-Assigning the most updated state    */
        n = ntp->n;
        LOG_DBG("Mutex Locked %s()", __func__);
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
            LOG_NORM("--->ASQS: %hu, ACQS: %hu\n",
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
            LOG_NORM("--->ASQ: %lu\n",
                 (uint64_t)n->sq[ASQ_ID].dma_addr);
            */
            break;
        case NVME_ACQ:
            n->cq[ACQ_ID].dma_addr |= val;
            break;
        case (NVME_ACQ + 4):
            n->cq[ACQ_ID].dma_addr |=
                    (uint64_t)((uint64_t)val << 32);
            /*
            LOG_NORM("--->ACQ: %lu\n",
                (uint64_t)n->cq[ACQ_ID].dma_addr);
            */
            break;
        default:
            /* ERROR */
            break;
        }
        pthread_mutex_unlock(&nvme_mutex);
        LOG_DBG("Mutex Unlocked %s()", __func__);
    }
    free(ntp);
    LOG_DBG("Came Outside Threaded %s()", __func__);
    return NULL;
}

/*********************************************************************
    Function     :    process_doorbell_thread
    Description  :    Thread for processing Doorbell and SQ commands
    Return Type  :    void*
    Arguments    :    NVMEThread*
*********************************************************************/
void *process_doorbell_thread(void *nt)
{
    NVMEState *n ;
    target_phys_addr_t addr;
    uint32_t val;
    uint32_t tmp = 0;

    pthread_detach(pthread_self());

    NVMEThread * ntp = (NVMEThread *) nt;

    /* Keeping Local copy of Addr and Value Sent */
    addr = ntp->addr;
    val = ntp->val;


    pthread_mutex_lock(&nvme_mutex);
    LOG_DBG("Executing Thread %s() with ID : %d with Addr: 0x%08x",
        __func__, (int)pthread_self(), (unsigned)addr);
    /* Re-Assigning the most updated state */
    n =  ntp->n;

    /*Check if it is CQ or SQ doorbell */

    tmp = (addr - NVME_SQ0TDBL) / sizeof(uint32_t);


    LOG_DBG("Mutex Locked %s()", __func__);
    if (tmp % 2) {
        /* CQ */
        tmp = (addr - NVME_CQ0HDBL) / 8;
        if (tmp > NVME_MAX_QID) {
            LOG_NORM("Wrong CQ ID: %d\n", tmp);
            pthread_mutex_unlock(&nvme_mutex);
            free(ntp);
            return NULL;
        }

        n->cq[tmp].head = val & 0xffff;
    } else {
        /* SQ */
        tmp = (addr - NVME_SQ0TDBL) / 8;
        if (tmp > NVME_MAX_QID) {
            LOG_NORM("Wrong SQ ID: %d\n", tmp);
            pthread_mutex_unlock(&nvme_mutex);
            free(ntp);
            return NULL;
        }
        n->sq[tmp].tail = val & 0xffff;
        /* Process single Message per thread    */
        process_sq(n, tmp);
    }

    pthread_mutex_unlock(&nvme_mutex);
    LOG_DBG("Mutex Unlocked %s()", __func__);
    free(ntp);
    LOG_DBG("Came Outside Threaded %s()", __func__);
    return NULL;
}

/* Write 1 Byte at addr/register */
static void nvme_mmio_writeb(void *opaque, target_phys_addr_t addr,
                                 uint32_t val)
{
    NVMEState *n = opaque;

    LOG_NORM("%s(): addr = 0x%08x, val = 0x%08x\n",
        __func__, (unsigned)addr, val);
    (void)n;
}

/* Write 2 Bytes at addr/register
 * 16 bit - can filter here doorbell write !!!
 */
static void nvme_mmio_writew(void *opaque, target_phys_addr_t addr,
                                 uint32_t val)
{
    NVMEThread * nt = (NVMEThread *) malloc(1 * sizeof(NVMEThread));
    LOG_NORM("%s(): addr = 0x%08x, val = 0x%08x\n", __func__,
        (unsigned)addr, val);

    /* Check if some registry was written */
    if (addr < NVME_SQ0TDBL) {
        nt->n = opaque;
        nt->addr = addr;
        nt->val = val;
        nt->pt = nt;
        /* Same thread identifier th used for all the threads */
        /* TODO : Add length parameter inside NVMEThread to
         * specify read/write lengths
         */
        if (pthread_create(&th, &pthread_attr, process_reg_writel_thread,
            (void *)nt->pt) != 0) {
            LOG_NORM("Thread Create failed in: %s()\n", __func__);
        }
        return;
    }

    /* Doorbell area is  between NVME_SQ0TDBL and NVME_CQMAXHDBL */
    if (addr >= NVME_SQ0TDBL && addr <= NVME_CQMAXHDBL) {
        nt->n = opaque;
        nt->addr = addr;
        nt->val = val;
        nt->pt = nt;
        /* Same thread identifier th used for all the threads */
        if (pthread_create(&th, &pthread_attr, process_doorbell_thread,
            (void *)nt->pt) != 0) {
            LOG_NORM("Thread Create failed in: %s()\n", __func__);
        }
    }
    return ;
}

/* Write 4 Bytes at addr/register */
static void nvme_mmio_writel(void *opaque, target_phys_addr_t addr,
                                 uint32_t val)
{
    NVMEThread * nt = (NVMEThread *) malloc(1 * sizeof(NVMEThread));

    /* Check if some registry was written */
    if (addr < NVME_SQ0TDBL) {
        nt->n = opaque;
        nt->addr = addr;
        nt->val = val;
        nt->pt = nt;
        /* Same thread identifier th used for all the threads */
        if (pthread_create(&th, &pthread_attr, process_reg_writel_thread,
            (void *)nt->pt) != 0) {
            LOG_NORM("Thread Create failed in: %s()\n", __func__);
        }
        return;
    }

    /* Doorbell area is  between NVME_SQ0TDBL and NVME_CQMAXHDBL */
    if (addr >= NVME_SQ0TDBL && addr <= NVME_CQMAXHDBL) {
        nt->n = opaque;
        nt->addr = addr;
        nt->val = val;
        nt->pt = nt;
        /* Same thread identifier th used for all the threads */
        if (pthread_create(&th, &pthread_attr, process_doorbell_thread,
            (void *)nt->pt) != 0) {
            LOG_NORM("Thread Create failed in: %s()\n", __func__);
        }
    }
    return;
}

/*********************************************************************
    Function     :    process_reg_readl_thread
    Description  :    Thread for processing Register Reads
    Return Type  :    void*
    Arguments    :    NVMEThread*
*********************************************************************/
void *process_reg_readl_thread(void *nt)
{
    NVMEState *n ;
    target_phys_addr_t addr;
    NVMEThread * ntp = (NVMEThread *) nt;
    /* Keeping Local copy of Addr Sent    */
    addr = ntp->addr;

    pthread_mutex_lock(&nvme_mutex);
    LOG_DBG("Executing Thread %s() with ID : %d with Addr: 0x%08x", __func__,
        (int)pthread_self(), (unsigned)addr);

    /* Re-assigning the most updated state    */
    n = ntp->n;
    LOG_NORM("%s(): addr = 0x%08x\n", __func__, (unsigned)addr);

    switch (addr) {
    case NVME_CTST:
        LOG_NORM("%s(): Status required.\n", __func__);

        if ((n->cstatus.rdy) &&
            (n->sq[ASQ_ID].dma_addr && n->cq[ACQ_ID].dma_addr)) {
            LOG_NORM("%s(): ADM QUEUES ARE SET. Return 1.\n",
            __func__);
            ntp->val = 1;
        }

        break;
    case NVME_CMD_SS:
        /* Offset of doorbell region */
        ntp->val = NVME_SQ0TDBL;
        break;

    default:
        LOG_NORM("Register not supported. offset: 0x%x\n",
            (unsigned int) addr);
        break;
    }
    pthread_mutex_unlock(&nvme_mutex);
    return NULL;
}


/* Read 1 Byte from addr/register */
static uint32_t nvme_mmio_readb(void *opaque, target_phys_addr_t addr)
{
    NVMEState *n = opaque;
    LOG_NORM("%s(): addr = 0x%08x\n", __func__, (unsigned)addr);
    (void)n;
    return 0;
}

/* Read 2 Bytes from addr/register */
static uint32_t nvme_mmio_readw(void *opaque, target_phys_addr_t addr)
{
    NVMEState *n = opaque;
    LOG_NORM("%s(): addr = 0x%08x\n", __func__, (unsigned)addr);
    (void)n;
    return 0;
}

/* Read 4 Bytes from addr/register */
static uint32_t nvme_mmio_readl(void *opaque, target_phys_addr_t addr)
{
    uint32_t rd_val;
    pthread_t th_read;
    NVMEThread * nt = (NVMEThread *) malloc(1 * sizeof(NVMEThread));
    nt->n = opaque;
    nt->addr = addr;
    nt->val = 0;
    nt->pt = nt;
    /* Local thread identifier used for all read threads */
    if (pthread_create(&th_read, &pthread_attr, process_reg_readl_thread,
        (void *)nt->pt) != 0) {
        LOG_NORM("Thread Create failed in: %s()\n", __func__);
    }
    pthread_join(th_read, NULL);
    rd_val = nt->val ;
    free(nt);
    return rd_val;
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

/*********************************************************************
    Function     :    range_covers_reg
    Description  :    Checks whether the given range covers a
                      particular register
    Return Type  :    uint8_t : 1 : covers , 0 : does not cover
    Arguments    :    uint64_t : Start addr to write
                      uint64_t : Length to be written
                      uint64_t : Register offset in address space
                      uint64_t : Size of register
*********************************************************************/
static inline uint8_t range_covers_reg(uint64_t addr, uint64_t len,
    uint64_t reg , uint64_t reg_size)
{
    return (uint8_t) ((addr <= reg) &&
        (range_get_last(reg, reg_size) <= range_get_last(addr, len)));
}

/*********************************************************************
    Function     :    pci_write_config
    Description  :    Thread for PCI space write
    Return Type  :    void *
    Arguments    :    PCIThread * : Pointer to PCIThread
*********************************************************************/
void *pci_write_config(void *pt)
{
    PCIThread * pci_th = (PCIThread *) pt;

    pthread_detach(pthread_self());

    /* Has to wait on the NVME mutex to maintain instruction ordering */
    pthread_mutex_lock(&nvme_mutex);
    LOG_DBG("Executing Thread %s() with ID : %d with Addr: 0x%08x",
        __func__, (int)pthread_self(), (unsigned int)pci_th->addr);

    /* Writing the PCI Config Space */
    pci_default_write_config(pci_th->p, pci_th->addr, pci_th->val, pci_th->len);
    LOG_DBG("RW Mask : 0x%08x", pci_th->p->wmask[pci_th->addr]);
    if (range_covers_reg(pci_th->addr, pci_th->len, PCI_ROM_ADDRESS,
        PCI_ROM_ADDRESS_LEN)) {
        /* Defaulting EROM value to 0x00 */
        pci_set_long(&pci_th->p->config[PCI_ROM_ADDRESS], (uint32_t) 0x00);
    } else if (range_covers_reg(pci_th->addr, pci_th->len, PCI_BIST,
        PCI_BIST_LEN)) {
        /* Defaulting BIST value to 0x00 */
        pci_set_byte(&pci_th->p->config[PCI_BIST], (uint8_t) 0x00);
    } else if (range_covers_reg(pci_th->addr, pci_th->len, PCI_BASE_ADDRESS_2,
        PCI_BASE_ADDRESS_2_LEN)) {
        /* Defaulting IDBAR (BAR2) value to 0x00 */
        pci_set_long(&pci_th->p->config[PCI_BASE_ADDRESS_2], (uint32_t) 0x00);
    }

    /* Logic for Resets and other functionality stuff will come here */

    pthread_mutex_unlock(&nvme_mutex);
    LOG_DBG("Mutex Unlocked %s()", __func__);
    free(pci_th);
    LOG_DBG("Came Outside Threaded %s()", __func__);
    return NULL;
}

/*********************************************************************
    Function     :    nvme_pci_write_config
    Description  :    Function for PCI config space writes
    Return Type  :    uint32_t : Value read
    Arguments    :    NVMEState * : Pointer to PCI device state
                      uint32_t : Address (offset address)
                      uint32_t : Value to be written
                      int : Length to be written
*********************************************************************/
static void nvme_pci_write_config(PCIDevice *d,
                                    uint32_t addr, uint32_t val, int len)
{
    PCIThread * pci_t = (PCIThread *) malloc(1 * sizeof(PCIThread));

    pci_t->p = d;
    pci_t->addr = addr;
    pci_t->val = val;
    pci_t->len = len;
    pci_t->pt = pci_t;

    /* Same thread identifier th used for all write threads */
    if (pthread_create(&th, &pthread_attr, pci_write_config,
        (void *)pci_t->pt) != 0) {
        LOG_NORM("Thread Create failed in: %s()\n", __func__);
    }
    return;
}

/*********************************************************************
    Function     :    nvme_pci_read_config
    Description  :    Function for PCI config space reads
    Return Type  :    uint32_t : Value read
    Arguments    :    PCIDevice * : Pointer to PCI device state
                      uint32_t : address (offset address)
                      int : Length to be read
*********************************************************************/
static uint32_t nvme_pci_read_config(PCIDevice *d, uint32_t addr, int len)
{
    uint32_t val;
    /* Has to wait on the NVME mutex to maintain instruction ordering */
    pthread_mutex_lock(&nvme_mutex);
    val = pci_default_read_config(d, addr, len);
    pthread_mutex_unlock(&nvme_mutex);
    return val;
}

static void nvme_mmio_map(PCIDevice *pci_dev, int reg_num, pcibus_t addr,
                            pcibus_t size, int type)
{
    NVMEState *n = DO_UPCAST(NVMEState, dev, pci_dev);

    if (reg_num) {
        LOG_NORM("Only bar0 is allowed! reg_num: %d\n", reg_num);
    }
    /* Is this hacking? */
    /* BAR 0 is shared: Registry, doorbells and MSI-X. Only
     * registry and doorbell part of BAR0 should be handled
     * by nvme mmio functions.
     * The MSI-X part of BAR0 should be mapped by MSI-X functions.
     * The msix_init function changes the bar size to add its
     * tables to it. */

    cpu_register_physical_memory(addr, n->bar0_size, n->mmio_index);
    n->bar0 = (void *) addr;

    /* Let the MSI-X part handle the MSI-X table.  */
    msix_mmio_map(pci_dev, reg_num, addr, size, type);
}

static void nvme_set_registry(NVMEState *n)
{
    if (!n) {
        return;
    }
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
    return;
}

static void clear_nvme_device(NVMEState *n)
{
    uint32_t i = 0;

    if (!n) {
        return;
    }
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


/*********************************************************************
    Function     :    pci_space_init
    Description  :    Hardcoded PCI space initialization
    Return Type  :    void
    Arguments    :    PCIDevice * : Pointer to the PCI device
*********************************************************************/
static void pci_space_init(PCIDevice *pci_dev)
{
    NVMEState *n = DO_UPCAST(NVMEState, dev, pci_dev);
    uint8_t *pci_conf = NULL;
    uint32_t ret;

    pci_conf = n->dev.config;

    pci_config_set_vendor_id(pci_conf, PCI_VENDOR_ID_INTEL);
    /* Device id is fake  */
    pci_config_set_device_id(pci_conf, NVME_DEV_ID);

    /* STORAGE EXPRESS is not yet a standard. */
    pci_config_set_class(pci_conf, PCI_CLASS_STORAGE_EXPRESS >> 8);

    pci_config_set_prog_interface(pci_conf,
        0xf & PCI_CLASS_STORAGE_EXPRESS);

    /* TODO: What with the rest of PCI fields? Capabilities? */

    /*other notation:  pci_config[OFFSET] = 0xff; */

    LOG_NORM("%s(): Setting PCI Interrupt PIN A\n", __func__);
    pci_conf[PCI_INTERRUPT_PIN] = 1;

    n->nvectors = NVME_MSIX_NVECTORS;
    n->bar0_size = NVME_REG_SIZE;
    ret = msix_init((struct PCIDevice *)&n->dev,
        n->nvectors, 0, n->bar0_size);
    if (ret) {
        LOG_NORM("%s(): PCI MSI-X Failed\n", __func__);

    } else {
        LOG_NORM("%s(): PCI MSI-X Initialized\n", __func__);
    }
}

/*********************************************************************
    Function     :    pci_nvme_init
    Description  :    NVME initialization
    Return Type  :    int
    Arguments    :    PCIDevice * : Pointer to the PCI device
    TODO: Make any initialization here or when
         controller receives 'enable' bit?
*********************************************************************/
static int pci_nvme_init(PCIDevice *pci_dev)
{
    NVMEState *n = DO_UPCAST(NVMEState, dev, pci_dev);
    uint8_t *pci_conf = NULL;
    uint32_t ret;
    /* Pointer for Config file and temp file */
    FILE *config_file;
    /* Processor 1 */
    cpu_set_t  mask;

    /* Array to store the PATH to config files
     * NVME_DEVICE_PCI_CONFIG_FILE and
     * NVME_DEVICE_NVME_CONFIG_FILE
     */
    char file_path[MAX_CHAR_PER_LINE];

    pci_conf = n->dev.config;
    n->nvectors = NVME_MSIX_NVECTORS;
    n->bar0_size = NVME_REG_SIZE;

    /* Get the Config File Path in the system */
    read_file_path(file_path, PCI_SPACE);

    config_file = fopen((char *)file_path, "r");
    if (config_file == NULL) {
        LOG_ERR("Could not open the config file");
        LOG_NORM("Defaulting the PCI space..");
        pci_space_init(pci_dev);
    } else {
        /* Reads PCI config File */
        if (read_config_file(config_file, n, PCI_SPACE)) {
            fclose(config_file);
            LOG_ERR("Error Reading the Config File\n");
            LOG_NORM("Defaulting the PCI space..\n");
            pci_space_init(pci_dev);
        } else {
            /* Close the File */
            fclose(config_file);
        }
    }
    LOG_NORM("%s(): Reg0 size %u, nvectors: %hu\n", __func__,
        n->bar0_size, n->nvectors);

    /* NVMe is Little Endian. */
    n->mmio_index = cpu_register_io_memory(nvme_mmio_read, nvme_mmio_write,
        n,  DEVICE_LITTLE_ENDIAN);

    /* Register BAR 0 (and bar 1 as it is 64bit). */
    pci_register_bar((struct PCIDevice *)&n->dev,
        0, ((n->dev.cap_present & QEMU_PCI_CAP_MSIX) ?
        n->dev.msix_bar_size : n->bar0_size),
        (PCI_BASE_ADDRESS_SPACE_MEMORY |
        PCI_BASE_ADDRESS_MEM_TYPE_64),
        nvme_mmio_map);

    for (ret = 0; ret < n->nvectors; ret++) {
        msix_vector_use(&n->dev, ret);
    }
    nvme_set_registry(n);

    for (ret = 0; ret < NVME_MAX_QID; ret++) {
        memset(&(n->sq[ret]), 0, sizeof(NVMEIOSQueue));
        memset(&(n->cq[ret]), 0, sizeof(NVMEIOCQueue));
    }

    n->fd = -1;
    n->mapping_addr = NULL;

    /* Setting up thread attributes */

    /* NOTE: Policy SCHED_OTHER is selected but still run to completion is
     * guaranteed based on mutexes which synchronize between the threads
     */

    /* SCHED_OTHER is default but it's explicitly set here to isolate
     * NVME module from the affects of future code changes to other
     * modules in Qemu
     */

    /* TODO :
     * If the responsiveness of the NVME module goes down scheduling
     * policy will have to be changed to SCHED_FIFO
     */
    pthread_attr_init(&pthread_attr);
    pthread_attr_setinheritsched(&pthread_attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&pthread_attr, SCHED_OTHER);
    CPU_ZERO(&mask);
    CPU_SET(1, &mask);
    if (pthread_attr_setaffinity_np(&pthread_attr, sizeof(cpu_set_t),
        &mask) < 0) {
        perror("pthread_setaffinity_np");
    }
    sch_param.sched_priority = sched_get_priority_max(SCHED_OTHER);
    pthread_attr_setschedparam(&pthread_attr, &sch_param);
    sched_setscheduler(getpid(), SCHED_OTHER, &sch_param);

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
    .is_express = 1,
    .config_write = nvme_pci_write_config,
    .config_read = nvme_pci_read_config,
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
