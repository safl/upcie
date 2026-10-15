// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Simon Andreas Frimann Lund <os@safl.dk>

/**
 * An admin command may take longer than CAP.TO
 * =============================================
 *
 * Opens a controller on a BAR0 in host memory with CAP.TO of 500 ms, submits an admin command
 * whose completion a thread posts after one second, and expects nvme_admin_sync_dmamem() to
 * collect it.
 *
 * No device is needed: the queues and the registers are host memory.
 */
#define _UPCIE_WITH_NVME
#include <upcie/upcie.h>
#include <pthread.h>

#define CAP_TO 1 ///< CAP.TO in units of 500 ms
#define CPL_DELAY_MS 1000
#define DEPTH 2
#define CID 7

struct poster {
	struct nvme_completion *cq;
	uint8_t phase;
};

static void *
post_late(void *arg)
{
	struct poster *p = arg;
	volatile struct nvme_completion *cqe = &p->cq[0];

	usleep(CPL_DELAY_MS * 1000);

	cqe->cid = CID;
	cqe->sqhd = 1;
	__atomic_store_n(&cqe->status, p->phase, __ATOMIC_RELEASE);

	return NULL;
}

int
main(void)
{
	struct nvme_controller ctrlr = {0};
	struct nvme_command cmd = {0};
	struct nvme_completion cpl = {0};
	struct poster poster = {0};
	uint32_t sqdb = 0, cqdb = 0;
	pthread_t thread;
	uint64_t cap;
	uint8_t *bar0;
	int err;

	bar0 = calloc(1, 0x1000);
	assert(bar0);
	// CSTS reads 0, so the reset finds the controller not ready at once
	*(volatile uint64_t *)bar0 = (uint64_t)CAP_TO << 24;

	err = nvme_controller_reset_via_bar0(&ctrlr, bar0, &cap);
	assert(!err);

	ctrlr.aq.sq = calloc(DEPTH, sizeof(struct nvme_command));
	ctrlr.aq.cq = calloc(DEPTH, sizeof(struct nvme_completion));
	assert(ctrlr.aq.sq && ctrlr.aq.cq);
	ctrlr.aq.sqdb = &sqdb;
	ctrlr.aq.cqdb = &cqdb;
	ctrlr.aq.depth = DEPTH;
	ctrlr.aq.tail_last_written = UINT16_MAX;
	ctrlr.aq.phase = 1;

	poster.cq = ctrlr.aq.cq;
	poster.phase = ctrlr.aq.phase;
	err = pthread_create(&thread, NULL, post_late, &poster);
	assert(!err);

	cmd.opc = 0x80; ///< Format NVM
	err = nvme_admin_sync_dmamem(&ctrlr, &cmd, CID, &cpl);

	pthread_join(thread, NULL);
	free(ctrlr.aq.sq);
	free(ctrlr.aq.cq);
	free(bar0);

	if (err) {
		printf("FAILED: an admin command completing after %d ms, past CAP.TO of %d ms, "
		       "returned err(%d)\n",
		       CPL_DELAY_MS, CAP_TO * 500, err);
		return 1;
	}

	printf("PASSED: an admin command completing past CAP.TO is collected\n");
	return 0;
}
