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

/* queue is full if tail is just behind head. */
static uint8_t is_cq_full(NVMEState *n, uint16_t qid)
{
	uint16_t t;

	t = (n->cq[qid].tail + n->cq[qid].size + 1) % n->cq[qid].size;

	if (t == n->cq[qid].head) {
/*
		printf("%s(): CQ %d is full\n",  __func__, qid);
		printf("%s(): CQ %d size %d\n",  __func__, qid,
							 n->cq[qid].size);
		printf("%s(): CQ %d H %d  T %d\n",  __func__,
				 qid, n->cq[qid].head, n->cq[qid].tail);
*/
		return 1;
	}

	//printf("%s(): CQ %d is not full\n",  __func__, qid);

	return 0;
}

static void incr_sq_head(NVMEIOSQueue *q)
{
	q->head = (q->head + 1) % (q->size + 1);
}

static void incr_cq_tail(NVMEIOCQueue *q)
{
	q->tail = (q->tail + 1) % (q->size + 1);

	if (!q->tail)
		q->phase_tag = q->phase_tag ? 0 : 1;

//	printf("kw q: CQ H %d T %d, size %d, phase_tag %d\n",
//			 q->head, q->tail, q->size, q->phase_tag);
}

static uint8_t abort_command(NVMEState *n, uint16_t sq_id, NVMECmd *sqe)
{
	uint16_t i;

	for (i = 0; i < NVME_ABORT_COMMAND_LIMIT; i++) {
		if (n->sq[sq_id].abort_cmd_id[i] == sqe->cid) {
			n->sq[sq_id].abort_cmd_id[i] = NVME_EMPTY;
			n->abort--;
			return 1;
		}
	}

	return 0;
}

void process_sq_no_thread(NVMEState *n, uint16_t sq_id)
{
	target_phys_addr_t addr;
	uint16_t cq_id;
	NVMECmd sqe;
	NVMECQE cqe;
	uint32_t ret = NVME_SC_DATA_XFER_ERROR;
	NVMEStatusField *sf = (NVMEStatusField*) &cqe.status;

	cq_id = n->sq[sq_id].cq_id;
	if (is_cq_full(n, cq_id))
		return;

	memset(&cqe, 0, sizeof(cqe));
	addr = n->sq[sq_id].dma_addr + n->sq[sq_id].head * sizeof(sqe);

	nvme_dma_mem_read(addr, (uint8_t *)&sqe, sizeof(sqe));

	if (n->abort) {
		if (abort_command(n, sq_id, &sqe)) {
			incr_sq_head(&n->sq[sq_id]);
			return;
		}
	}

	if (sq_id == ASQ_ID)
		ret = nvme_admin_command(n, &sqe, &cqe);
	else
		ret = nvme_io_command(n, &sqe, &cqe);

	cqe.sq_id = sq_id;
	cqe.sq_head = n->sq[sq_id].head;
	cqe.command_id = sqe.cid;

	sf->p = n->cq[cq_id].phase_tag;
	sf->m = 0;
	sf->dnr = 0; /* TODO add support for dnr */
	/* write cqe to complition queue */

	addr = n->cq[cq_id].dma_addr + n->cq[cq_id].tail * sizeof(cqe);
	nvme_dma_mem_write(addr, (uint8_t *)&cqe, sizeof(cqe));

	incr_sq_head(&n->sq[sq_id]);
	incr_cq_tail(&n->cq[cq_id]);

	if (cq_id == ACQ_ID) {
		/*
		 3.1.9 says: "This queue is always associated
				 with interrupt vector 0"
		*/
//		printf("kw q: ADMIN CQ msix_notify(&pci, 0);\n");
		msix_notify(&(n->dev), 0);
	}

	if (n->cq[cq_id].irq_enabled) {
//		printf("kw q: msix_notify(&pci, %d);\n", n->cq[cq_id].vector);
		msix_notify(&(n->dev), n->cq[cq_id].vector);
	} else {
		printf("kw q: IRQ not enabled for CQ: %d;\n", cq_id);
	}

	return;
}

static void process_sq(NVMEState *n, uint16_t sq_id)
{
	target_phys_addr_t addr;
	uint16_t cq_id;
	NVMECmd sqe;
	NVMECQE cqe;
	uint32_t ret = NVME_SC_DATA_XFER_ERROR;
	NVMEStatusField *sf = (NVMEStatusField*) &cqe.status;

	pthread_mutex_lock(&n->nvme_doorbell);
	printf("kw q: mutex_lock process_sq\n");
	cq_id = n->sq[sq_id].cq_id;
	if (is_cq_full(n, cq_id))
		return;

	memset(&cqe, 0, sizeof(cqe));
	addr = n->sq[sq_id].dma_addr + n->sq[sq_id].head * sizeof(sqe);

	nvme_dma_mem_read(addr, (uint8_t *)&sqe, sizeof(sqe));

	if (n->abort) {
		if (abort_command(n, sq_id, &sqe)) {
			incr_sq_head(&n->sq[sq_id]);
			pthread_mutex_unlock(&n->nvme_doorbell);
			return;
		}
	}

	if (sq_id == ASQ_ID)
		ret = nvme_admin_command(n, &sqe, &cqe);
	else
		ret = nvme_io_command(n, &sqe, &cqe);

	cqe.sq_id = sq_id;
	cqe.sq_head = n->sq[sq_id].head;
	cqe.command_id = sqe.cid;

	sf->p = n->cq[cq_id].phase_tag;
	sf->m = 0;
	sf->dnr = 0; /* TODO add support for dnr */
	/* write cqe to complition queue */

	addr = n->cq[cq_id].dma_addr + n->cq[cq_id].tail * sizeof(cqe);
	nvme_dma_mem_write(addr, (uint8_t *)&cqe, sizeof(cqe));

	incr_sq_head(&n->sq[sq_id]);
	incr_cq_tail(&n->cq[cq_id]);
	pthread_mutex_unlock(&n->nvme_doorbell);
	printf("kw q: mutex_unlock process_sq\n");

	if (cq_id == ACQ_ID) {
		/*
		 3.1.9 says: "This queue is always associated
				 with interrupt vector 0"
		*/
//		printf("kw q: ADMIN CQ msix_notify(&pci, 0);\n");
		msix_notify(&(n->dev), 0);
	}

	if (n->cq[cq_id].irq_enabled) {
//		printf("kw q: msix_notify(&pci, %d);\n", n->cq[cq_id].vector);
		msix_notify(&(n->dev), n->cq[cq_id].vector);
	} else {
		printf("kw q: IRQ not enabled for CQ: %d;\n", cq_id);
	}

	return;
}

static void process_debug_sq(NVMEState *n, uint16_t qid)
{
printf("-------------------------------------------------------------------\n");
	printf("process IO request SQ ID %d\n", qid);
	process_sq(n, qid);
printf("-------------------------------------------------------------------\n");
}

static void *nvme_io_thread(void *arg)
{
	NVMEState *n = (NVMEState *)arg;
	uint16_t i = 0;

	if(!n)
		pthread_exit(NULL);

	while (n->io_thread_state != TH_STOP) {
		for (i = 0; i < NVME_MAX_QID; i++)
			if (n->sq[i].tail != n->sq[i].head)
				process_debug_sq(n, i);

				//process_sq(n, i);

		pthread_yield();
	}

	n->io_thread_state = TH_EXIT;
	pthread_exit(NULL);
}

int nvme_init_io_thread(NVMEState *n)
{
	int ret = 0;

	ret = pthread_create(&(n->nvme_io_thread), NULL,
					 nvme_io_thread, (void *)n);

	if (ret)
		n->io_thread_state = TH_EXIT;
	else
		n->io_thread_state = TH_STARTED;

	return ret;
}
