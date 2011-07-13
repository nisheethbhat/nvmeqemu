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
#include <sys/mman.h>

#define NVME_STORAGE_FILE_NAME "nvme_store.img"
#define PAGE_SIZE 4096


void nvme_dma_mem_read(target_phys_addr_t addr, uint8_t *buf, int len)
{
    cpu_physical_memory_rw(addr, buf, len, 0);
}

void nvme_dma_mem_write(target_phys_addr_t addr, uint8_t *buf, int len)
{
    cpu_physical_memory_rw(addr, buf, len, 1);
}

static uint8_t do_rw_prp(NVMEState *n, uint64_t mem_addr, uint64_t data_size,
             uint64_t file_offset, uint8_t rw)
{
    uint8_t *mapping_addr = n->mapping_addr;
    uint64_t m_offset = 0;
    uint64_t f_offset = file_offset;
    uint64_t total = data_size;
    uint64_t len = 0;

    while (total != 0) {
        if (total >= NVME_BUF_SIZE) {
            len = NVME_BUF_SIZE;
        } else {
            len = total;
        }
        switch (rw) {
        case NVME_CMD_READ:
            nvme_dma_mem_write(mem_addr + m_offset,
                mapping_addr + f_offset, len);
            break;
        case NVME_CMD_WRITE:
            nvme_dma_mem_read(mem_addr + m_offset,
                mapping_addr + f_offset, len);
            break;
        default:
            LOG_NORM("Error- wrong opcode: %d\n", rw);
            break;
        }

        m_offset = m_offset + len;
        f_offset = f_offset + len;
        total = total - len;
    };

    return NVME_SC_SUCCESS;
}

static uint8_t do_rw_prp_list(NVMEState *n, NVMECmd *command)
{
    uint64_t total = 0;
    uint64_t len = 0;
    uint64_t offset = 0;
    uint64_t prp_list[512];
    uint16_t i = 0;

    uint8_t res = FAIL;
    struct NVME_rw *cmd = (struct NVME_rw *)command;

    /*check if prp2 contains pointer to list or pointer to memory*/
    /*assume page size 4096 */
    /*TODO find from which NVME register PAGE_SIZE size should be read*/

    total = (cmd->nlb + 1) * NVME_BLOCK_SIZE;

    len = PAGE_SIZE;
    offset = cmd->slba * NVME_BLOCK_SIZE;

    res = do_rw_prp(n, cmd->prp1, len, offset, cmd->opcode);
    if (res == FAIL) {
        return FAIL;
    }
    total = total - PAGE_SIZE;
    offset = offset + PAGE_SIZE;

    /* LOG_NORM("sizeof(prp_list) %d\n", sizeof(prp_list)); */
    memset(prp_list, 0, sizeof(prp_list));
    nvme_dma_mem_read(cmd->prp2, (uint8_t *)prp_list, sizeof(prp_list));

    i = 0;
    while (total != 0) {
        if (i == 511) {
            nvme_dma_mem_read(prp_list[511], (uint8_t *)prp_list,
                sizeof(prp_list));
            i = 0;
        }

        if (total >= PAGE_SIZE) {
            len = PAGE_SIZE;
        } else {
            len = total;
        }
        res = do_rw_prp(n, prp_list[i], len, offset, cmd->opcode);
        if (res == FAIL) {
            break;
        }
        total = total - len;
        offset = offset + len;
        i++;
    }

    return res;
}

uint8_t nvme_io_command(NVMEState *n, NVMECmd *sqe, NVMECQE *cqe)
{
    struct NVME_rw *e = (struct NVME_rw *)sqe;
    uint8_t res = FAIL;

    if (sqe->opcode == NVME_CMD_FLUSH) {
        return NVME_SC_SUCCESS;
    }

    if ((sqe->opcode != NVME_CMD_READ) &&
        (sqe->opcode != NVME_CMD_WRITE)) {
        LOG_NORM("Wrong IO opcode:\t\t0x%02x\n", sqe->opcode);
        return res;
    }

    if (!e->prp2) {
        res = do_rw_prp(n, e->prp1,
            ((e->nlb + 1) * NVME_BLOCK_SIZE),
            (e->slba * NVME_BLOCK_SIZE), e->opcode);
    } else if ((e->nlb + 1) <= 2 * (PAGE_SIZE/NVME_BLOCK_SIZE)) {
        res = do_rw_prp(n, e->prp1, PAGE_SIZE,
            e->slba * NVME_BLOCK_SIZE, e->opcode);

        if (res == FAIL) {
            return FAIL;
        }
        res = do_rw_prp(n, e->prp2,
            (e->nlb + 1) * NVME_BLOCK_SIZE - PAGE_SIZE,
            e->slba * NVME_BLOCK_SIZE + PAGE_SIZE,
            e->opcode);

        if (res == FAIL) {
            return FAIL;
        }
    } else {
        res = do_rw_prp_list(n, sqe);
    }
    return res;
}

static int nvme_create_storage_file(NVMEState *n)
{
    n->fd = open(NVME_STORAGE_FILE_NAME, O_RDWR | O_CREAT
        | O_TRUNC, S_IRUSR | S_IWUSR);
    posix_fallocate(n->fd, 0, NVME_STORAGE_FILE_SIZE);
    LOG_NORM("Backing store created with fd %d\n", n->fd);
    close(n->fd);
    n->fd = -1;
    return 0;
}

int nvme_close_storage_file(NVMEState *n)
{
    if (n->fd != -1) {
        if (n->mapping_addr) {
            munmap(n->mapping_addr, n->mapping_size);
            n->mapping_addr = NULL;
            n->mapping_size = 0;
        }
        close(n->fd);
        n->fd = -1;
    }
    return 0;
}

int nvme_open_storage_file(NVMEState *n)
{
    struct stat st;
    uint8_t *mapping_addr;

    if (n->fd != -1) {
        return FAIL;
    }

    if (stat(NVME_STORAGE_FILE_NAME, &st) != 0 ||
        st.st_size != NVME_STORAGE_FILE_SIZE) {
        nvme_create_storage_file(n);
    }

    n->fd = open(NVME_STORAGE_FILE_NAME, O_RDWR);
    if (n->fd == -1) {
        return FAIL;
    }
    mapping_addr = mmap(NULL, NVME_STORAGE_FILE_SIZE, PROT_READ | PROT_WRITE,
        MAP_SHARED, n->fd, 0);

    if (mapping_addr == NULL) {
        close(n->fd);
        return FAIL;
    }

    n->mapping_size = NVME_STORAGE_FILE_SIZE;
    n->mapping_addr = mapping_addr;

    LOG_NORM("Backing store mapped to %p\n", n->mapping_addr);
    return 0;
}
