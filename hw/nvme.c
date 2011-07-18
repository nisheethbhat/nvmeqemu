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

/* Mutex for exclusive access to NVMEState */
pthread_mutex_t nvme_mutex = PTHREAD_MUTEX_INITIALIZER;
/* Variables to set up Scheduling policies for NVME threads */
pthread_attr_t pthread_attr;
struct sched_param sch_param;
/* Maximum number of charachters on a line in any config file */
#define MAX_CHAR_PER_LINE 100

static void clear_nvme_device(NVMEState *n);
static void update_var(char* , FILERead *, int*, int *);
static void update_val(char* , char*);
static int read_config_file(FILE *, NVMEState *, uint8_t);
static void pci_space_init(PCIDevice *);
static void config_space_write(NVMEState *,  FILERead*, uint8_t,
    uint8_t);

#ifndef NVME_THREADED
static void process_reg_writel(NVMEState *n, target_phys_addr_t addr,
                                     uint32_t val)
{
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
    } else {
          /* Doorbell write */
    }
}

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
            LOG_NORM("Wrong CQ ID: %d\n", tmp);
            return 1;
        }

        n->cq[tmp].head = val & 0xffff;
    } else {
        /* SQ */
        tmp = (addr - NVME_SQ0TDBL) / 8;
        if (tmp > NVME_MAX_QID) {
            LOG_NORM("Wrong SQ ID: %d\n", tmp);
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
    #ifndef NVME_THREADED
        do {
            process_sq(n, tmp);
        } while (n->sq[tmp].head != n->sq[tmp].tail);
    #else
        /* Process single Message per thread    */
        process_sq(n, tmp);
    #endif
    }

    pthread_mutex_unlock(&nvme_mutex);
    LOG_DBG("Mutex Unlocked %s()", __func__);
    free(ntp);
    LOG_DBG("Came Outside Threaded %s()", __func__);
    return NULL;
}
#endif
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
    #ifdef NVME_THREADED
        pthread_t th;
        NVMEThread * nt = (NVMEThread *) malloc(1 * sizeof(NVMEThread));
    #else
        NVMEState *n = opaque;
    #endif
    LOG_NORM("%s(): addr = 0x%08x, val = 0x%08x\n", __func__,
        (unsigned)addr, val);

    /* Check if some registry was written */
    if (addr < NVME_SQ0TDBL) {
        #ifdef NVME_THREADED
            nt->n = opaque;
            nt->addr = addr;
            nt->val = val;
            nt->pt = nt;

            /* Same thread identifier th used for all the threads */
            if (pthread_create(&th, &pthread_attr, process_reg_writel_thread,
                (void *)nt->pt) != 0) {
                LOG_NORM("Thread Create failed in: %s()\n", __func__);
            }
        #else
            process_reg_writel(n, addr, val);
        #endif
        return;
    }

    /* Doorbell area is  between NVME_SQ0TDBL and NVME_CQMAXHDBL */
    if (addr >= NVME_SQ0TDBL && addr <= NVME_CQMAXHDBL) {
        #ifdef NVME_THREADED
            nt->n = opaque;
            nt->addr = addr;
            nt->val = val;
            nt->pt = nt;
            /* Same thread identifier th used for all the threads */
            if (pthread_create(&th, &pthread_attr, process_doorbell_thread,
                (void *)nt->pt) != 0) {
                LOG_NORM("Thread Create failed in: %s()\n", __func__);
            }
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
    /* Using the same Thread Identifier for different threads    */
    #ifdef NVME_THREADED
        pthread_t th;
        NVMEThread * nt = (NVMEThread *) malloc(1 * sizeof(NVMEThread));
    #else
        NVMEState *n = opaque;
    #endif


    /* Check if some registry was written */
    if (addr < NVME_SQ0TDBL) {
        #ifdef NVME_THREADED
            nt->n = opaque;
            nt->addr = addr;
            nt->val = val;
            nt->pt = nt;
            /* Same thread identifier th used for all the threads */
            if (pthread_create(&th, &pthread_attr, process_reg_writel_thread,
                (void *)nt->pt) != 0) {
                LOG_NORM("Thread Create failed in: %s()\n", __func__);
            }

        #else
            process_reg_writel(n, addr, val);
        #endif
        return;
    }

    /* Doorbell area is  between NVME_SQ0TDBL and NVME_CQMAXHDBL */
    if (addr >= NVME_SQ0TDBL && addr <= NVME_CQMAXHDBL) {
        #ifdef NVME_THREADED
            nt->n = opaque;
            nt->addr = addr;
            nt->val = val;
            nt->pt = nt;
            /* Same thread identifier th used for all the threads */
            if (pthread_create(&th, &pthread_attr, process_doorbell_thread,
                (void *)nt->pt) != 0) {
                LOG_NORM("Thread Create failed in: %s()\n", __func__);
            }

        #else
            process_doorbell(n, addr, val);
        #endif
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
    n =  ntp->n;
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

#ifdef NVME_THREADED
    uint32_t rd_val;
    pthread_t th;
    NVMEThread * nt = (NVMEThread *) malloc(1 * sizeof(NVMEThread));
    nt->n = opaque;
    nt->addr = addr;
    nt->val = 0;
    nt->pt = nt;
    /* Same thread identifier th used for all the threads */
    if (pthread_create(&th, &pthread_attr, process_reg_readl_thread,
        (void *)nt->pt) != 0) {
        LOG_NORM("Thread Create failed in: %s()\n", __func__);
    }
    pthread_join(th, NULL);
    rd_val = nt->val ;
    free(nt);
    return rd_val;

#else
    NVMEState *n = opaque;
    LOG_NORM("%s(): addr = 0x%08x\n", __func__, (unsigned)addr);

    switch (addr) {
    case NVME_CTST:
        LOG_NORM("%s(): Status required.\n", __func__);
        if ((n->cstatus.rdy) &&
            (n->sq[ASQ_ID].dma_addr && n->cq[ACQ_ID].dma_addr)) {
            LOG_NORM("%s(): ADM QUEUES ARE SET. Return 1.\n",
            __func__);
            return 1;
        } else {
            /*TODO Handle the case for read / writes when
             * controller ready bit is not set or dma addresses
             * are not set
             */
        }

        break;
    case NVME_CMD_SS:
        /* Offset of doorbell region */
        return NVME_SQ0TDBL;
        break;

    default:
        LOG_NORM("Register not supported. offset: 0x%x\n",
            (unsigned int) addr);
        break;
    }
    return 0;
#endif

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
    Function     :    config_space_write
    Description  :    Function for config space writes
    Return Type  :    void
    Arguments    :    NVMEState * : Pointer to NVME device state
                      FILERead *  : Pointer to the data extracted from
                                    <REG> </REG> from config file
                      uint8_t : Flag for type of config space
                              : 0 = PCI Config Space
                              : 1 = NVME Config Space
                      uint8_t : Dynamic offset ex:CAP pointer
*********************************************************************/
static void config_space_write(NVMEState *n, FILERead* data, uint8_t flag,
    uint8_t offset)
{
    /* Pointer to config  space */
    uint8_t *conf = NULL;
    /* Pointers for masks (PCI space) */
    uint8_t *wmask = NULL, *used = NULL, *cmask = NULL, *w1cmask = NULL;

    if (PCI_SPACE == flag) {
        conf = n->dev.config;
        wmask = n->dev.wmask;
        used = n->dev.used;
        cmask = n->dev.cmask;
        w1cmask = n->dev.w1cmask;
    } else {
        /* TODO
         * Add the start address of NVME address Space
         */
    }

    /* Write to config Space */

    /*
     * TODO
     * Remove the if statement to make the function more generalized
     * If condition addded since the number of mask pointers for PCI and
     * NVME space are different and for PCI space it is already defined in Qemu
     */
    if (flag == PCI_SPACE) {
        if (data->len == 1) {
            pci_set_byte(&conf[data->offset + offset], (uint8_t) data->val);
            pci_set_byte(&wmask[data->offset + offset],
                (uint8_t) data->rw_mask);
            pci_set_byte(&used[data->offset + offset], (uint8_t) 1);
            pci_set_byte(&cmask[data->offset + offset], (uint8_t) 1);
            pci_set_byte(&w1cmask[data->offset + offset],
                (uint8_t) data->rwc_mask);
        } else if (data->len == 2) {
            pci_set_word(&conf[data->offset + offset], (uint16_t) data->val);
            pci_set_word(&wmask[data->offset + offset],
                (uint16_t) data->rw_mask);
            pci_set_word(&used[data->offset + offset], (uint16_t) 1);
            pci_set_word(&cmask[data->offset + offset], (uint16_t) 1);
            pci_set_word(&w1cmask[data->offset + offset],
                (uint16_t) data->rwc_mask);
        } else if (data->len == 4) {
            pci_set_long(&conf[data->offset + offset], (uint32_t) data->val);
            pci_set_long(&wmask[data->offset + offset],
                (uint32_t) data->rw_mask);
            pci_set_long(&used[data->offset + offset], (uint32_t) 1);
            pci_set_long(&cmask[data->offset + offset], (uint32_t) 1);
            pci_set_long(&w1cmask[data->offset + offset],
                (uint32_t) data->rwc_mask);
        }
    } else {
        /* TODO
         * Add NVME Space Read/Writes
         */
    }


}
/*********************************************************************
    Function     :    read_config_file
    Description  :    Function for reading config files
    Return Type  :    int (0 : Success , 1 : Error)
    Arguments    :    FILE *    : Config File Pointer
                      NVMEState * : Pointer to NVME device state
                      uint8_t : Flag for type of config space
                              : 0 = PCI Config Space
                              : 1 = NVME Config Space
*********************************************************************/
static int read_config_file(FILE *config_file , NVMEState *n, uint8_t flag)
{
    /* Temp array used to store a line in the file */
    char var_line[MAX_CHAR_PER_LINE];
    /* Start of Reg and End of Reg indicator */
    int eor_flag = 0, sor_flag = 0;

    /* Offset used for reads and writes*/
    uint8_t offset = 0;
    /* Recent Capabilities pointer value*/
    uint8_t link = 0;

    /* Structure to retain values for <REG to </REG> */
    FILERead data;




    /* Read the configuration file line by line */

    if (fgets(var_line, MAX_CHAR_PER_LINE, config_file) == NULL) {
        LOG_NORM("Config File cannot be read\n");
        return 1;
    }
    do {
        update_var((char *)var_line, &data, &eor_flag, &sor_flag);
        if (sor_flag == 1) {
            do {
                /* Reading line after line between <REG and </REG> */
                if (fgets(var_line, MAX_CHAR_PER_LINE, config_file) == NULL) {
                    LOG_NORM("Wrong Format between <REG> and </REG>\n");
                    return 1;
                }
                update_var((char *)var_line, &data, &eor_flag, &sor_flag);
            } while (eor_flag != 1);

            LOG_DBG("Length Read : %u\n", data.len);
            LOG_DBG("Offset Read : %u\n", data.offset);
            LOG_DBG("Val Read : %u\n", data.val);
            LOG_DBG("RO Mask Read : %u\n", data.ro_mask);
            LOG_DBG("RW Mask Read : %u\n", data.rw_mask);
            LOG_DBG("RWC Mask Read : %u\n", data.rwc_mask);
            LOG_DBG("RWS Mask Read : %u\n", data.rws_mask);
            LOG_DBG("CFG NAME Read : %s\n", data.cfg_name);

            /* Logic for extracting dependancy between different capabilites
             * from file
             */
            if (!strcmp(data.cfg_name, "PCIHEADER")) {
                offset = 0;
                if (data.offset == 0x34) {
                    link = data.val;
                }
            } else if (!strcmp(data.cfg_name, "PMCAP")) {

                if (data.offset == 0) {
                    /* One time per CFG_NAME */
                    offset = link;
                }
                if (data.offset == 0 && data.len > 1) {
                    link = (uint8_t) data.val >> 8;
                } else if (data.offset == 1) {
                    link = (uint8_t) data.val ;
                }

            } else if (!strcmp(data.cfg_name, "MSICAP")) {
                if (data.offset == 0) {
                    /* One time per CFG_NAME */
                    offset = link;
                }
                if (data.offset == 0 && data.len > 1) {
                    link = (uint8_t) data.val >> 8;
                } else if (data.offset == 1) {
                    link = (uint8_t) data.val ;
                }
            } else if (!strcmp(data.cfg_name, "MSIXCAP")) {
                if (data.offset == 0) {
                    /* One time per CFG_NAME */
                    offset = link;
                    /*TODO
                    * Set QEMU_PCI_CAP_MSIX if present
                    */
                }
                if (data.offset == 0 && data.len > 1) {
                    link = (uint8_t) data.val >> 8;
                } else if (data.offset == 1) {
                    link = (uint8_t) data.val ;
                }
            } else if (!strcmp(data.cfg_name, "PXACP")) {
                if (data.offset == 0) {
                    /* One time per CFG_NAME */
                    offset = link;
                }
                if (data.offset == 0 && data.len > 1) {
                    link = (uint8_t) data.val >> 8;
                } else if (data.offset == 1) {
                    link = (uint8_t) data.val ;
                }
            } else if (!strcmp(data.cfg_name, "AERCAP")) {
                if (data.offset == 0) {
                    /* One time per CFG_NAME */
                    offset = link;
                }
                if (data.offset == 0 && data.len > 3) {
                    /* AERCAP pointer will take only 8 bits out of 12 bits */
                    /* TODO
                     * Extend it to 12 bits but as of now only
                     * uint8_t pointers are defined for PCI address space
                     * in Qemu
                     */
                    link = (uint8_t) data.val >> 20;
                } else if (data.offset == 2) {
                    link = (uint8_t) data.val >> 4 ;
                }
            }


            if ((data.offset + offset + data.len) > 0x1000) {
                LOG_ERR("Invlaid Offsets present in the Config file for \
                    PCI address space \n");
            } else {
                /* PCI / NVME space writes */
                config_space_write(n, &data, flag, offset);
            }

        }

        /* Reseting the data strucutre */
        data.len = 0;
        data.offset = 0;
        data.val = 0;
        data.ro_mask = 0;
        data.rw_mask = 0;
        data.rwc_mask = 0;
        data.rws_mask = 0;
        bzero(data.cfg_name, sizeof(data.cfg_name));
        /* Resetting the flags */
        sor_flag = 0;
        eor_flag = 0;
    } while (fgets(var_line, MAX_CHAR_PER_LINE, config_file) != NULL);

    return 0;
}

/*********************************************************************
    Function     :    update_var
    Description  :    Reads data variables (TAGS) from each line
    Return Type  :    void
    Arguments    :    char *: Single Line of config file
                      FILERead *: Pointing to store the values
                                  extracted
                      int * : Pointer to Start of Reg Indicator
                      int * : Pointer to End of Reg Indicator
*********************************************************************/

static void update_var(char *ptr , FILERead *data, int *eor, int *sor)
{
    /* Temp arrays for extracting TAGS and values respectivley */
    char tags[MAX_CHAR_PER_LINE], val[MAX_CHAR_PER_LINE];
    /* Offset used for parsing */
    int offset = 0;

    /* Reading of Data Variables from each line */
    do {
        if (*ptr != ' ' && *ptr != '\t' && *ptr != '\n' && *ptr != '\r') {
            tags[offset] = (char)*ptr;
            offset++;
        }
        ptr++;
    } while (*ptr != '=' && *ptr != '>' && (offset < MAX_CHAR_PER_LINE));

    tags[offset] = '\0';

    /* Compare the TAGS and extract the values */
    if (!strcmp(tags, "</REG")) {
        *eor = 1;

    } else if (!strcmp(tags, "<REG")) {
        *sor = 1;

    } else if (!strcmp(tags, "LENGTH")) {
        /* Pointer pre-incremented to get rid of "=" sign
         * while passing onto update_val function
         */
        update_val(++ptr, val);
        data->len = (uint32_t) strtol(val, NULL, 16) ;
    } else if (!strcmp(tags, "OFFSET")) {
        update_val(++ptr, val);
        data->offset = (uint32_t) strtol(val, NULL, 16) ;
    } else if (!strcmp(tags, "VALUE")) {
        update_val(++ptr, val);
        data->val = (uint32_t) strtol(val, NULL, 16) ;
    } else if (!strcmp(tags, "RO_MASK")) {
        update_val(++ptr, val);
        data->ro_mask = (uint32_t) strtol(val, NULL, 16) ;
    } else if (!strcmp(tags, "RW_MASK")) {
        update_val(++ptr, val);
        data->rw_mask = (uint32_t) strtol(val, NULL, 16) ;
    } else if (!strcmp(tags, "RWC_MASK")) {
        update_val(++ptr, val);
        data->rw_mask = (uint32_t) strtol(val, NULL, 16) ;
    } else if (!strcmp(tags, "RWS_MASK")) {
        update_val(++ptr, val);
        data->rw_mask = (uint32_t) strtol(val, NULL, 16) ;
    } else if (!strcmp(tags, "CFG_NAME")) {
        update_val(++ptr, val);
        strcpy(data->cfg_name, val);
    }

    return;
}


/*********************************************************************
    Function     :    update_val
    Description  :    Reads data values from each line
    Return Type  :    void
    Arguments    :    char * : Offset inside a single line in File
                      char * : Pointer to store the values extracted
*********************************************************************/
static void update_val(char *ptr, char *val)
{
    int offset = 0;

    /* Reading of Data Values from each line */
    do {
        if (*ptr != ' ' && *ptr != '\t' && *ptr != '\n' && *ptr != '\r') {
            val[offset] = (char)*ptr;
            offset++;
        }
        ptr++;
    } while (*ptr != '\0' && (offset < MAX_CHAR_PER_LINE));
    val[offset] = '\0';
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
    /* Pointer for Config Files */
    FILE *config_file ;
    /* Processor 0        */
    cpu_set_t  mask;

    pci_conf = n->dev.config;

    config_file = fopen(NVME_DEVICE_PCI_CONFIG_FILE, "r");
    if (config_file == NULL) {
        LOG_ERR("Could not open the config file\n");
        LOG_NORM("Defaulting the PCI space..\n");
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
    n->nvectors = NVME_MSIX_NVECTORS;
    n->bar0_size = NVME_REG_SIZE;
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
