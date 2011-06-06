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
#define NVME_STORAGE_FILE_NAME "250.img"
#define NVME_STORAGE_FILE_SIZE	(250 * 1024 * 1024)
#define PAGE_SIZE 4096

void nvme_dma_mem_read(target_phys_addr_t addr, uint8_t *buf, int len)
{
	/* XXX cpu_physical_memory_rw fails if used in threads */
	cpu_physical_memory_rw(addr, buf, len, 0);

}

void nvme_dma_mem_write(target_phys_addr_t addr, uint8_t *buf, int len)
{
	/* XXX cpu_physical_memory_rw fails if used in threads */
	cpu_physical_memory_rw(addr, (uint8_t *)buf, len, 1);
}

static uint8_t do_rw_prp(FILE *file, uint64_t mem_addr, uint64_t data_size,
			 uint64_t file_offset, uint8_t rw)
{
	uint8_t buf[NVME_BUF_SIZE];
	uint64_t m_offset;
	uint64_t f_offset;
	uint64_t total = 0;
	uint64_t len = 0;
	uint32_t res = 0;

	total = data_size;
	f_offset = file_offset;
	m_offset = 0;

	while (total != 0) {
		if (total >= NVME_BUF_SIZE)
			len = NVME_BUF_SIZE;
		else
			len = total;

		switch (rw) {
		case NVME_CMD_READ:
			res = fseek(file, f_offset, SEEK_SET);
			if (res)
				return FAIL;

			res = fread(buf, 1, len, file);
			if (res != len)
				return FAIL;

			nvme_dma_mem_write(mem_addr + m_offset, buf, len);
			break;

		case NVME_CMD_WRITE:
			nvme_dma_mem_read(mem_addr + m_offset, buf, len);

			res = fseek(file, f_offset, SEEK_SET);
			if (res)
				return FAIL;

			res = fwrite(buf, 1, len, file);
			if (res != len)
				return FAIL;

			break;
		default:
			printf("Error- wrong opcode: %d\n", rw);
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

	res = do_rw_prp(n->file, cmd->prp1, len, offset, cmd->opcode);
	if (res == FAIL)
		return FAIL;

	total = total - PAGE_SIZE;
	offset = offset + PAGE_SIZE;

	//printf("sizeof(prp_list) %d\n", sizeof(prp_list));
	memset(prp_list, 0, sizeof(prp_list));
	nvme_dma_mem_read(cmd->prp2, (uint8_t *)prp_list, sizeof(prp_list));

	i = 0;
	while (total != 0) {
		if (i == 511) {
			nvme_dma_mem_read(prp_list[511], (uint8_t *)prp_list,
							 sizeof(prp_list));
			i = 0;
		}

		if (total >= PAGE_SIZE)
			len = PAGE_SIZE;
		else
			len = total;

		res = do_rw_prp(n->file, prp_list[i], len, offset, cmd->opcode);
		if (res == FAIL)
			break;

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
		fflush(n->file);
		fsync(fileno(n->file));
		return NVME_SC_SUCCESS;
	}

	if ((sqe->opcode != NVME_CMD_READ) &&
	    (sqe->opcode != NVME_CMD_WRITE)) {
		printf("Wrong IO opcode:\t\t0x%02x\n", sqe->opcode);
		return res;
	}

	if (!e->prp2) {
		res = do_rw_prp(n->file, e->prp1,
			 ((e->nlb + 1) * NVME_BLOCK_SIZE),
			 (e->slba * NVME_BLOCK_SIZE), e->opcode);
	} else if ((e->nlb + 1) <= 2 * (PAGE_SIZE/NVME_BLOCK_SIZE)) {
		res = do_rw_prp(n->file, e->prp1, PAGE_SIZE,
				e->slba * NVME_BLOCK_SIZE, e->opcode);

		if (res == FAIL)
			return FAIL;

		res = do_rw_prp(n->file, e->prp2,
				(e->nlb + 1) * NVME_BLOCK_SIZE - PAGE_SIZE,
				e->slba * NVME_BLOCK_SIZE + PAGE_SIZE,
				e->opcode);

		if (res == FAIL)
			return FAIL;
	} else {
		res = do_rw_prp_list(n, sqe);
	}

	fflush(n->file);
	fsync(fileno(n->file));
	return res;
}

static int name_create_storage_file(NVMEState *n)
{
	n->file = fopen(NVME_STORAGE_FILE_NAME, "w");
	fclose(n->file);
	n->file = NULL;

	if (truncate(NVME_STORAGE_FILE_NAME, NVME_STORAGE_FILE_SIZE)) {
		printf("Can't truncate file \"%s\"\n", NVME_STORAGE_FILE_NAME);
		return FAIL;
	}

	n->file = fopen(NVME_STORAGE_FILE_NAME, "r+");

	if (!n->file) {
		printf("Can't create file \"%s\"\n", NVME_STORAGE_FILE_NAME);
		return FAIL;
	}

	return 0;
}

int nvme_close_storage_file(NVMEState *n)
{
	if (n->file) {
		fflush(n->file);
		fsync(fileno(n->file));
		fclose(n->file);
		n->file = NULL;
	}
	return 0;
}

int nvme_open_storage_file(NVMEState *n)
{
	struct stat st;

	n->file = fopen(NVME_STORAGE_FILE_NAME, "r+");

	if (!n->file)
		return name_create_storage_file(n);

	if (stat(NVME_STORAGE_FILE_NAME, &st) != 0) {
		fclose(n->file);
		return FAIL;
	}

	if (st.st_size == NVME_STORAGE_FILE_SIZE)
		return 0;

	fclose(n->file);
	n->file = NULL;
	if (truncate(NVME_STORAGE_FILE_NAME, NVME_STORAGE_FILE_SIZE)) {
		printf("Can't truncate file \"%s\"\n", NVME_STORAGE_FILE_NAME);
		return FAIL;
	}

	n->file = fopen(NVME_STORAGE_FILE_NAME, "r+");
	if (!n->file)
		return FAIL;

	return 0;
}
