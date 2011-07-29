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

/* File Level scope functions */
static void clear_nvme_device(NVMEState *n);
static void pci_space_init(PCIDevice *);
static void nvme_pci_write_config(PCIDevice *, uint32_t, uint32_t, int);
static uint32_t nvme_pci_read_config(PCIDevice *, uint32_t, int);
static inline uint8_t range_covers_reg(uint64_t, uint64_t, uint64_t,
    uint64_t);
static void process_doorbell(NVMEState *, target_phys_addr_t, uint32_t);

/*********************************************************************
    Function     :    process_doorbell
    Description  :    Processing Doorbell and SQ commands
    Return Type  :    void
    Arguments    :    NVMEState * : Pointer to NVME device State
                      target_phys_addr_t : Address (offset address)
                      uint32_t : Value to be written
*********************************************************************/
static void process_doorbell(NVMEState *nvme_dev, target_phys_addr_t addr,
    uint32_t val)
{
    /* Used to get the SQ/CQ number to be written to */
    uint32_t queue_id;

    LOG_DBG("%s(): addr = 0x%08x, val = 0x%08x\n",
        __func__, (unsigned)addr, val);


    /* Check if it is CQ or SQ doorbell */
    queue_id = (addr - NVME_SQ0TDBL) / sizeof(uint32_t);

    if (queue_id % 2) {
        /* CQ */
        queue_id = (addr - NVME_CQ0HDBL) / QUEUE_BASE_ADDRESS_WIDTH;
        if (queue_id > NVME_MAX_QID) {
            LOG_NORM("Wrong CQ ID: %d\n", queue_id);
            return;
        }

        nvme_dev->cq[queue_id].head = val & 0xffff;
    } else {
        /* SQ */
        queue_id = (addr - NVME_SQ0TDBL) / QUEUE_BASE_ADDRESS_WIDTH;
        if (queue_id > NVME_MAX_QID) {
            LOG_NORM("Wrong SQ ID: %d\n", queue_id);
            return;
        }
        nvme_dev->sq[queue_id].tail = val & 0xffff;
        /* Processing all the messages for that particular queue */
        do {
            process_sq(nvme_dev, queue_id);
        } while (nvme_dev->sq[queue_id].head != nvme_dev->sq[queue_id].tail);

    }
    return;
}

/*********************************************************************
    Function     :    nvme_mmio_writeb
    Description  :    Write 1 Byte at addr/register
    Return Type  :    void
    Arguments    :    void * : Pointer to NVME device State
                      target_phys_addr_t : Address (offset address)
                      uint32_t : Value to be written
*********************************************************************/
static void nvme_mmio_writeb(void *opaque, target_phys_addr_t addr,
    uint32_t val)
{
    NVMEState *n = opaque;

    LOG_DBG("%s(): addr = 0x%08x, val = 0x%08x\n",
        __func__, (unsigned)addr, val);
    (void)n;
}

/*********************************************************************
    Function     :    nvme_mmio_writew
    Description  :    Write 2 Bytes at addr/register
    Return Type  :    void
    Arguments    :    void * : Pointer to NVME device State
                      target_phys_addr_t : Address (offset address)
                      uint32_t : Value to be written
*********************************************************************/
static void nvme_mmio_writew(void *opaque, target_phys_addr_t addr,
    uint32_t val)
{
    NVMEState *n = opaque;

    LOG_DBG("%s(): addr = 0x%08x, val = 0x%08x\n",
        __func__, (unsigned)addr, val);
    (void)n;
}

/*********************************************************************
    Function     :    nvme_mmio_writel
    Description  :    Write 4 Bytes at addr/register
    Return Type  :    void
    Arguments    :    void * : Pointer to NVME device State
                      target_phys_addr_t : Address (offset address)
                      uint32_t : Value to be written
*********************************************************************/
static void nvme_mmio_writel(void *opaque, target_phys_addr_t addr,
    uint32_t val)
{
    NVMEState *nvme_dev = (NVMEState *) opaque;
    /* Strucutre Used for Worker Threads */

    LOG_DBG("%s(): addr = 0x%08x, val = 0x%08x\n",
        __func__, (unsigned)addr, val);
    /* Check if NVME controller Capabilities was written */
    if (addr < NVME_SQ0TDBL) {
        LOG_DBG("%s(): addr = 0x%08x\n", __func__, (unsigned)addr);
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
                if (nvme_dev->cq[ACQ_ID].dma_addr &&
                    nvme_dev->sq[ASQ_ID].dma_addr &&
                    (!nvme_open_storage_file(nvme_dev))) {
                    nvme_dev->cstatus.rdy = 1;
                    nvme_dev->cq[ACQ_ID].phase_tag = 1;
                }
            } else {
                clear_nvme_device(nvme_dev);
            }
            break;
        case NVME_AQA:
            nvme_dev->sq[ASQ_ID].size = val & 0xfff;
            nvme_dev->cq[ACQ_ID].size = (val >> 16) & 0xfff;
            break;
        case NVME_ASQ:
            nvme_dev->sq[ASQ_ID].dma_addr |= val;
            break;
        case (NVME_ASQ + 4):
            nvme_dev->sq[ASQ_ID].dma_addr |=
                    (uint64_t)((uint64_t)val << 32);
            break;
        case NVME_ACQ:
            nvme_dev->cq[ACQ_ID].dma_addr |= val;
            break;
        case (NVME_ACQ + 4):
            nvme_dev->cq[ACQ_ID].dma_addr |=
                    (uint64_t)((uint64_t)val << 32);
            break;
        default:
            /* ERROR */
            break;
        }
    } else if (addr >= NVME_SQ0TDBL && addr <= NVME_CQMAXHDBL) {
        /* Process the Doorbell Writes */
        process_doorbell(nvme_dev, addr, val);
    }
    return;
}

/*********************************************************************
    Function     :    nvme_mmio_readb
    Description  :    Read 1 Bytes at addr/register
    Return Type  :    void
    Arguments    :    void * : Pointer to NVME device State
                      target_phys_addr_t : Address (offset address)
*********************************************************************/
static uint32_t nvme_mmio_readb(void *opaque, target_phys_addr_t addr)
{
    NVMEState *n = opaque;
    LOG_DBG("%s(): addr = 0x%08x\n", __func__, (unsigned)addr);
    (void)n;
    return 0;
}

/*********************************************************************
    Function     :    nvme_mmio_readw
    Description  :    Read 2 Bytes at addr/register
    Return Type  :    void
    Arguments    :    void * : Pointer to NVME device State
                      target_phys_addr_t : Address (offset address)
*********************************************************************/
static uint32_t nvme_mmio_readw(void *opaque, target_phys_addr_t addr)
{
    NVMEState *n = opaque;
    LOG_DBG("%s(): addr = 0x%08x\n", __func__, (unsigned)addr);
    (void)n;
    return 0;
}

/*********************************************************************
    Function     :    nvme_mmio_readl
    Description  :    Read 4 Bytes at addr/register
    Return Type  :    void
    Arguments    :    void * : Pointer to NVME device State
                      target_phys_addr_t : Address (offset address)
*********************************************************************/
static uint32_t nvme_mmio_readl(void *opaque, target_phys_addr_t addr)
{
    uint32_t rd_val = 0;
    NVMEState *nvme_dev = (NVMEState *) opaque;

    LOG_DBG("%s(): addr = 0x%08x\n", __func__, (unsigned)addr);
    switch (addr) {
    case NVME_CTST:
        LOG_NORM("%s(): Status required.\n", __func__);
        if ((nvme_dev->cstatus.rdy) &&
            (nvme_dev->sq[ASQ_ID].dma_addr && nvme_dev->cq[ACQ_ID].dma_addr)) {
            LOG_NORM("%s(): ADM QUEUES ARE SET. Return 1.\n",
            __func__);
            rd_val = 1;
        }
        break;
    case NVME_CMD_SS:
        /* Offset of doorbell region */
        rd_val = NVME_SQ0TDBL;
        break;

    default:
        LOG_NORM("Register not supported. offset: 0x%x\n",
            (unsigned int) addr);
        break;
    }
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
    Function     :    nvme_pci_write_config
    Description  :    Function for PCI config space writes
    Return Type  :    uint32_t : Value read
    Arguments    :    NVMEState * : Pointer to PCI device state
                      uint32_t : Address (offset address)
                      uint32_t : Value to be written
                      int : Length to be written
*********************************************************************/
static void nvme_pci_write_config(PCIDevice *pci_dev,
                                    uint32_t addr, uint32_t val, int len)
{
    LOG_DBG("%s(): addr = 0x%08x\n", __func__, (unsigned)addr);
    /* Writing the PCI Config Space */
    pci_default_write_config(pci_dev, addr, val, len);
    LOG_DBG("RW Mask : 0x%08x", pci_dev->wmask[addr]);
    if (range_covers_reg(addr, len, PCI_ROM_ADDRESS, PCI_ROM_ADDRESS_LEN)) {
        /* Defaulting EROM value to 0x00 */
        pci_set_long(&pci_dev->config[PCI_ROM_ADDRESS], (uint32_t) 0x00);
    } else if (range_covers_reg(addr, len, PCI_BIST, PCI_BIST_LEN)) {
        /* Defaulting BIST value to 0x00 */
        pci_set_byte(&pci_dev->config[PCI_BIST], (uint8_t) 0x00);
    } else if (range_covers_reg(addr, len, PCI_BASE_ADDRESS_2,
        PCI_BASE_ADDRESS_2_LEN)) {
        /* Defaulting IDBAR (BAR2) value to 0x00 */
        pci_set_long(&pci_dev->config[PCI_BASE_ADDRESS_2], (uint32_t) 0x00);
    }
    /* Logic for Resets and other functionality stuff will come here */
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
static uint32_t nvme_pci_read_config(PCIDevice *pci_dev, uint32_t addr, int len)
{
    uint32_t val = 0;

    LOG_DBG("%s(): addr = 0x%08x\n", __func__, (unsigned)addr);
    val = pci_default_read_config(pci_dev, addr, len);
    return val;
}

/*********************************************************************
    Function     :    nvme_mmio_map
    Description  :    Function for registering NVME controller space
    Return Type  :    void
    Arguments    :    PCIDevice * : Pointer to PCI device state
                      int : To specify the BAR's from BAR0-BAR5
                      pcibus_t : Addr to be registered
                      pcibus_t : size to be registered
                      int : Used for similarity bewtween msix map
*********************************************************************/
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

/*********************************************************************
    Function     :    nvme_set_registry
    Description  :    Default initialization of NVME Registery
    Return Type  :    void
    Arguments    :    NVMEState * : Pointer to NVME device state
*********************************************************************/
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

/*********************************************************************
    Function     :    clear_nvme_device
    Description  :    To reset/clear Nvme Device
    Return Type  :    void
    Arguments    :    NVMEState * : Pointer to NVME device state
*********************************************************************/
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

/*********************************************************************
    Function     :    do_nvme_reset
    Description  :    TODO: Not yet implemented
    Return Type  :    void
    Arguments    :    NVMEState * : Pointer to NVME device state
*********************************************************************/
static void do_nvme_reset(NVMEState *n)
{
    (void)n;
}

/*********************************************************************
    Function     :    qdev_nvme_reset
    Description  :    Handler for NVME Reset
    Return Type  :    void
    Arguments    :    DeviceState * : Pointer to NVME device state
*********************************************************************/
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

    return 0;
}

/*********************************************************************
    Function     :    pci_nvme_uninit
    Description  :    To unregister the NVME device from Qemu
    Return Type  :    void
    Arguments    :    PCIDevice * : Pointer to the PCI device
*********************************************************************/
static int pci_nvme_uninit(PCIDevice *pci_dev)
{
    NVMEState *n = DO_UPCAST(NVMEState, dev, pci_dev);
    nvme_close_storage_file(n);
    return 0;
}

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

/*********************************************************************
    Function     :    nvme_register_devices
    Description  :    Registering the NVME Device with Qemu
    Return Type  :    void
    Arguments    :    void
*********************************************************************/
static void nvme_register_devices(void)
{
    pci_qdev_register(&nvme_info);
}

device_init(nvme_register_devices);
