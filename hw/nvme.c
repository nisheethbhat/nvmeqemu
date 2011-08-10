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
static void read_file(NVMEState *, uint8_t);


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
    uint32_t index;

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
        /* Process all the Queues */
        for (index = 0; index < NVME_MAX_QID; index++) {
            /* Processing all the messages for that particular queue */
            while (nvme_dev->sq[index].head != nvme_dev->sq[index].tail) {
                process_sq(nvme_dev, index);
            }
        }
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

    LOG_DBG("%s(): addr = 0x%08x, val = 0x%08x\n",
        __func__, (unsigned)addr, val);
    /* Check if NVME controller Capabilities was written */
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
                if (nvme_dev->cq[ACQ_ID].dma_addr &&
                    nvme_dev->sq[ASQ_ID].dma_addr &&
                    (!nvme_open_storage_file(nvme_dev))) {
                    nvme_dev->cstatus->rdy = 1;
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
            break;
        }
    } else if (addr >= NVME_SQ0TDBL && addr <= NVME_CQMAXHDBL) {
        /* TODO hardcoding doorbell register write masks since it's
         * never read from config file
         */

        /* Process the Doorbell Writes */
        process_doorbell(nvme_dev, addr, val);
    }
    return;
}

/*********************************************************************
    Function     :    nvme_cntrl_write_config
    Description  :    Function for NVME Controller space writes
                      (except doorbell reads)
    Return Type  :    void
    Arguments    :    NVMEState * : Pointer to NVME device State
                      target_phys_addr_t : address (offset address)
                      uint32_t : Value to write
                      uint8_t : Length to be read
*********************************************************************/
void nvme_cntrl_write_config(NVMEState *nvme_dev,
    target_phys_addr_t addr, uint32_t val, uint8_t len)
{
    uint8_t index;
    for (index = 0; index < len && addr + index < NVME_CNTRL_SIZE;
        val >>= 8, index++) {
        /* Settign up RW and RO mask and making reserved bits non writable */
        nvme_dev->cntrl_reg[addr + index] = (nvme_dev->cntrl_reg[addr + index]
            & (~(nvme_dev->rw_mask[addr + index])
                | ~(nvme_dev->used_mask[addr + index])))
                    | (val & nvme_dev->rw_mask[addr + index]);
        /* W1C: Write 1 to Clear */
        nvme_dev->cntrl_reg[addr + index] &=
            ~(val & nvme_dev->rwc_mask[addr + index]);
        /* W1S: Write 1 to Set */
        nvme_dev->cntrl_reg[addr + index] |=
            (val & nvme_dev->rws_mask[addr + index]);
    }
}

/*********************************************************************
    Function     :    nvme_cntrl_read_config
    Description  :    Function for NVME Controller space reads
                      (except doorbell reads)
    Return Type  :    uint32_t : Value read
    Arguments    :    NVMEState * : Pointer to NVME device State
                      target_phys_addr_t : address (offset address)
                      uint8_t : Length to be read
*********************************************************************/
uint32_t nvme_cntrl_read_config(NVMEState *nvme_dev,
    target_phys_addr_t addr, uint8_t len)
{
    uint32_t val;
    /* Prints the assertion and aborts */
    assert(len == 1 || len == 2 || len == 4);
    len = MIN(len, NVME_CNTRL_SIZE - addr);
    memcpy(&val, nvme_dev->cntrl_reg + addr, len);
    return le32_to_cpu(val);
}
/*********************************************************************
    Function     :    nvme_mmio_readb
    Description  :    Read 1 Bytes at addr/register
    Return Type  :    void
    Arguments    :    void * : Pointer to NVME device State
                      target_phys_addr_t : Address (offset address)
    Note:- Even though function is readb, return value is uint32_t
    coz, Qemu mapping code does the masking of repective bits
*********************************************************************/
static uint32_t nvme_mmio_readb(void *opaque, target_phys_addr_t addr)
{
    uint8_t rd_val;
    NVMEState *nvme_dev = (NVMEState *) opaque;
    LOG_DBG("%s(): addr = 0x%08x\n", __func__, (unsigned)addr);
    /* Check if NVME controller Capabilities was written */
    if (addr < NVME_SQ0TDBL) {
        rd_val = nvme_cntrl_read_config(nvme_dev, addr, (uint8_t)MASK(1, 0));
    } else if (addr >= NVME_SQ0TDBL && addr <= NVME_CQMAXHDBL) {
        LOG_NORM("Undefined operation of reading the doorbell registers");
        rd_val = 0;
    } else {
        LOG_ERR("Undefined address read");
        LOG_ERR("Qemu supports only 64 queues");
        rd_val = 0 ;
    }
    return rd_val;
}

/*********************************************************************
    Function     :    nvme_mmio_readw
    Description  :    Read 2 Bytes at addr/register
    Return Type  :    void
    Arguments    :    void * : Pointer to NVME device State
                      target_phys_addr_t : Address (offset address)
    Note:- Even though function is readw, return value is uint32_t
    coz, Qemu mapping code does the masking of repective bits
*********************************************************************/
static uint32_t nvme_mmio_readw(void *opaque, target_phys_addr_t addr)
{
    uint32_t rd_val;
    NVMEState *nvme_dev = (NVMEState *) opaque;
    LOG_DBG("%s(): addr = 0x%08x\n", __func__, (unsigned)addr);

    /* Check if NVME controller Capabilities was written */
    if (addr < NVME_SQ0TDBL) {
        rd_val = nvme_cntrl_read_config(nvme_dev, addr, (uint8_t)MASK(1, 1));
    } else if (addr >= NVME_SQ0TDBL && addr <= NVME_CQMAXHDBL) {
        LOG_NORM("Undefined operation of reading the doorbell registers");
        rd_val = 0;
    } else {
        LOG_ERR("Undefined address read");
        LOG_ERR("Qemu supports only 64 queues");
        rd_val = 0 ;
    }
    return rd_val;
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

    /* Check if NVME controller Capabilities was written */
    if (addr < NVME_SQ0TDBL) {
        rd_val = nvme_cntrl_read_config(nvme_dev, addr, (uint8_t)MASK(1, 2));
    } else if (addr >= NVME_SQ0TDBL && addr <= NVME_CQMAXHDBL) {
        LOG_NORM("Undefined operation of reading the doorbell registers");
        rd_val = 0;
    } else {
        LOG_ERR("Undefined address read");
        LOG_ERR("Qemu supports only 64 queues");
        rd_val = 0 ;
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

    /* Writing the PCI Config Space */
    pci_default_write_config(pci_dev, addr, val, len);
    /*LOG_DBG("RW Mask : 0x%08x", pci_dev->wmask[addr]); */
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
    uint32_t val;
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
    /* This is the default initialization sequence when
     * config file is not found */
    uint32_t ind, index;
    uint32_t val, rw_mask, rws_mask, rwc_mask;
    for (ind = 0; ind < sizeof(nvme_reg)/sizeof(nvme_reg[0]); ind++) {
        rw_mask = nvme_reg[ind].rw_mask;
        rwc_mask = nvme_reg[ind].rwc_mask;
        rws_mask = nvme_reg[ind].rws_mask;

        val = nvme_reg[ind].reset;
        for (index = 0; index < nvme_reg[ind].len; val >>= 8, rw_mask >>= 8,
            rwc_mask >>= 8, rws_mask >>= 8, index++) {
            n->cntrl_reg[nvme_reg[ind].offset + index] = val;
            n->rw_mask[nvme_reg[ind].offset + index] = rw_mask;
            n->rws_mask[nvme_reg[ind].offset + index] = rws_mask;
            n->rwc_mask[nvme_reg[ind].offset + index] = rwc_mask;
            n->used_mask[nvme_reg[ind].offset + index] = (uint8_t)MASK(8, 0);
        }
    }
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
    Note:- RO/RW/RWC masks not supported for default PCI space
    initialization
*********************************************************************/
static void pci_space_init(PCIDevice *pci_dev)
{
    NVMEState *n = DO_UPCAST(NVMEState, dev, pci_dev);
    uint8_t *pci_conf = NULL;

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
}

/*********************************************************************
    Function     :    read_file
    Description  :    Reading the config files accompanied with error
                      handling
    Return Type  :    void
    Arguments    :    NVMEState * : Pointer to the NVMEState device
                      uint8_t : Space to Read
                                NVME_SPACE and PCI_SPACE
*********************************************************************/
static void read_file(NVMEState *n, uint8_t space)
{
    /* Pointer for Config file and temp file */
    FILE *config_file;
    /* Array to store the PATH to config files
     * NVME_DEVICE_PCI_CONFIG_FILE and
     * NVME_DEVICE_NVME_CONFIG_FILE
     */
    char file_path[MAX_CHAR_PER_LINE];

    /* Get the Config File Path in the system */
    read_file_path(file_path, space);

    config_file = fopen((char *)file_path, "r");
    if (config_file == NULL) {
        LOG_ERR("Could not open the config file");
        if (space == NVME_SPACE) {
            LOG_NORM("Defaulting the NVME space..");
            nvme_set_registry(n);
        } else if (space == PCI_SPACE) {
            LOG_NORM("Defaulting the PCI space..");
            pci_space_init(&n->dev);
        }
    } else {
        /* Reads config File */
        if (read_config_file(config_file, n, space)) {
            fclose(config_file);
            LOG_ERR("Error Reading the Config File");
            if (space == NVME_SPACE) {
                LOG_NORM("Defaulting the NVME space..");
                nvme_set_registry(n);
            } else if (space == PCI_SPACE) {
                LOG_NORM("Defaulting the PCI space..");
                pci_space_init(&n->dev);
            }
        } else {
            /* Close the File */
            fclose(config_file);
        }
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

    pci_conf = n->dev.config;
    n->nvectors = NVME_MSIX_NVECTORS;
    n->bar0_size = NVME_REG_SIZE;

    /* Reading the PCI space from the file */
    read_file(n, PCI_SPACE);

    ret = msix_init((struct PCIDevice *)&n->dev,
         n->nvectors, 0, n->bar0_size);
    if (ret) {
        LOG_NORM("%s(): PCI MSI-X Failed\n", __func__);
    } else {
        LOG_NORM("%s(): PCI MSI-X Initialized\n", __func__);
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

    /* Allocating space for NVME regspace & masks except the doorbells */
    n->cntrl_reg = qemu_mallocz(NVME_CNTRL_SIZE);
    n->rw_mask = qemu_mallocz(NVME_CNTRL_SIZE);
    n->rwc_mask = qemu_mallocz(NVME_CNTRL_SIZE);
    n->rws_mask = qemu_mallocz(NVME_CNTRL_SIZE);
    n->used_mask = qemu_mallocz(NVME_CNTRL_SIZE);
    /* Setting up the pointers in NVME address Space
     * TODO
     * These pointers have been defined since
     * present code uses the older defined strucutres
     * which has been replaced by pointers.
     * Once each and every reference is replaced by
     * offset from cntrl_reg, remove these pointers
     * becasue bit field structures are not portable
     * especially when the memory locations of the bit fields
     * have importance
     */
    n->ctrlcap = (NVMECtrlCap *) (n->cntrl_reg + NVME_CAP);
    n->ctrlv = (NVMEVersion *) (n->cntrl_reg + NVME_VER);
    n->cconf = (NVMECtrlConf *) (n->cntrl_reg + NVME_CC);
    n->cstatus = (NVMECtrlStatus *) (n->cntrl_reg + NVME_CTST);
    n->admqattrs = (NVMEAQA *) (n->cntrl_reg + NVME_AQA);

    /* Update NVME space registery from config file */
    read_file(n, NVME_SPACE);

    /* Defaulting the number of Queues */
    n->feature.number_of_queues = ((NVME_MAX_QID - 1) << 16)
        | (NVME_MAX_QID - 1);

    for (ret = 0; ret < n->nvectors; ret++) {
        msix_vector_use(&n->dev, ret);
    }


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
    /* Freeing space allocated for NVME regspace masks except the doorbells */
    qemu_free(n->cntrl_reg);
    qemu_free(n->rw_mask);
    qemu_free(n->rwc_mask);
    qemu_free(n->rws_mask);
    qemu_free(n->used_mask);
    LOG_NORM("Freed NVME device memory");
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
