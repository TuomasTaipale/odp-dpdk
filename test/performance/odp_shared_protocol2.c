/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2024 Nokia
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

#include <odp_api.h>
#include <odp/helper/odph_api.h>
#include "dma_ipc.h"

#define PROG_NAME "odp_shared_protocol"
#define MAX_OUT 15U
#define RING_SIZE 128U
#define BUF_COUNT 10240U
#define BUF_SIZE 4096U

struct ring_common {
	struct ODP_ALIGNED_CACHE {
		odp_atomic_u32_t w_head;
		odp_atomic_u32_t w_tail;
	};

	struct ODP_ALIGNED_CACHE {
		odp_atomic_u32_t r_head;
		odp_atomic_u32_t r_tail;
	};
};

typedef struct ODP_ALIGNED_CACHE {
	struct ring_common r;
	uint64_t data[];
} ring_u64_t;

static inline int cas_mo_u32(odp_atomic_u32_t *atom, uint32_t *old_val, uint32_t new_val,
			     int mo_success, int mo_failure)
{
	return __atomic_compare_exchange_n(&atom->v, old_val, new_val, 0 /* strong */, mo_success,
					   mo_failure);
}

static inline void ring_u64_init(ring_u64_t *ring)
{
	odp_atomic_init_u32(&ring->r.w_head, 0);
	odp_atomic_init_u32(&ring->r.w_tail, 0);
	odp_atomic_init_u32(&ring->r.r_head, 0);
	odp_atomic_init_u32(&ring->r.r_tail, 0);
}

static inline uint32_t ring_u64_deq(ring_u64_t *ring, uint32_t mask, uint64_t *data)
{
	uint32_t head, tail, new_head;

	/* Load/CAS acquire of r_head ensures that w_tail load happens after
	 * r_head load, and thus head value is always behind or equal to tail
	 * value. */
	head = odp_atomic_load_acq_u32(&ring->r.r_head);

	/* Move reader head. This thread owns data at the new head. */
	do {
		tail = odp_atomic_load_acq_u32(&ring->r.w_tail);

		if (head == tail)
			return 0;

		new_head = head + 1;

	} while (odp_unlikely(cas_mo_u32(&ring->r.r_head, &head, new_head,
					 __ATOMIC_ACQUIRE,
					 __ATOMIC_ACQUIRE) == 0));

	/* Read data. */
	*data = ring->data[new_head & mask];

	/* Wait until other readers have updated the tail */
	while (odp_unlikely(odp_atomic_load_u32(&ring->r.r_tail) != head))
		odp_cpu_pause();

	/* Update the tail. Writers acquire it. */
	odp_atomic_store_rel_u32(&ring->r.r_tail, new_head);

	return 1;
}

static inline uint32_t ring_u64_deq_multi(ring_u64_t *ring, uint32_t mask, uint64_t data[],
					  uint32_t num)
{
	uint32_t head, tail, new_head, i;

	/* Load/CAS acquire of r_head ensures that w_tail load happens after
	 * r_head load, and thus head value is always behind or equal to tail
	 * value. */
	head = odp_atomic_load_acq_u32(&ring->r.r_head);

	/* Move reader head. This thread owns data at the new head. */
	do {
		tail = odp_atomic_load_acq_u32(&ring->r.w_tail);

		/* Ring is empty */
		if (head == tail)
			return 0;

		/* Try to take all available */
		if ((tail - head) < num)
			num = tail - head;

		new_head = head + num;

	} while (odp_unlikely(cas_mo_u32(&ring->r.r_head, &head, new_head,
					 __ATOMIC_ACQUIRE,
					 __ATOMIC_ACQUIRE) == 0));

	/* Read data. */
	for (i = 0; i < num; i++)
		data[i] = ring->data[(head + 1 + i) & mask];

	/* Wait until other readers have updated the tail */
	while (odp_unlikely(odp_atomic_load_u32(&ring->r.r_tail) != head))
		odp_cpu_pause();

	/* Update the tail. Writers acquire it. */
	odp_atomic_store_rel_u32(&ring->r.r_tail, new_head);

	return num;
}

static inline void ring_u64_enq(ring_u64_t *ring, uint32_t mask, uint64_t data)
{
	uint32_t old_head, new_head;
	uint32_t size = mask + 1;

	/* Reserve a slot in the ring for writing */
	old_head = odp_atomic_fetch_inc_u32(&ring->r.w_head);
	new_head = old_head + 1;

	/* Wait for the last reader to finish. This prevents overwrite when
	 * a reader has been left behind (e.g. due to an interrupt) and is
	 * still reading the same slot. */
	while (odp_unlikely(new_head - odp_atomic_load_acq_u32(&ring->r.r_tail)
			    >= size))
		odp_cpu_pause();

	/* Write data */
	ring->data[new_head & mask] = data;

	/* Wait until other writers have updated the tail */
	while (odp_unlikely(odp_atomic_load_u32(&ring->r.w_tail) != old_head))
		odp_cpu_pause();

	/* Release the new writer tail, readers acquire it. */
	odp_atomic_store_rel_u32(&ring->r.w_tail, new_head);
}

static inline void ring_u64_enq_multi(ring_u64_t *ring, uint32_t mask, uint64_t data[],
				      uint32_t num)
{
	uint32_t old_head, new_head, i;
	uint32_t size = mask + 1;

	/* Reserve a slot in the ring for writing */
	old_head = odp_atomic_fetch_add_u32(&ring->r.w_head, num);
	new_head = old_head + 1;

	/* Wait for the last reader to finish. This prevents overwrite when
	 * a reader has been left behind (e.g. due to an interrupt) and is
	 * still reading these slots. */
	while (odp_unlikely(new_head - odp_atomic_load_acq_u32(&ring->r.r_tail)
			    >= size))
		odp_cpu_pause();

	/* Write data */
	for (i = 0; i < num; i++)
		ring->data[(new_head + i) & mask] = data[i];

	/* Wait until other writers have updated the tail */
	while (odp_unlikely(odp_atomic_load_u32(&ring->r.w_tail) != old_head))
		odp_cpu_pause();

	/* Release the new writer tail, readers acquire it. */
	odp_atomic_store_rel_u32(&ring->r.w_tail, old_head + num);
}

typedef enum {
	PRS_OK,
	PRS_NOK,
	PRS_TERM,
	PRS_NOT_SUP
} parse_result_t;

typedef struct {
	odp_buffer_t outstanding[RING_SIZE];
	odp_instance_t inst;
	odp_instance_t peer_inst;
	odp_shm_t stash_shm;
	uint8_t *stash_data;
	odp_stash_t stash;
	odp_dma_t dma;
} prog_config_t;

typedef struct ODP_ALIGNED_CACHE stash_t {
	uint32_t ring_mask;
	ring_u64_t hdr;
	uint64_t data[RING_SIZE];
} stash_t;

typedef struct {
	stash_t tx_handle_channel1;
	stash_t tx_handle_channel2;
	stash_t rx_handle_channel1;
	stash_t rx_handle_channel2;
} shared_config_t;

static prog_config_t prog_conf;
static shared_config_t *shared_conf;

static odp_bool_t fill_buffer_stash(odp_stash_t stash, uint8_t *data)
{
	uintptr_t elem;

	for (uint32_t i = 0U; i < BUF_COUNT; ++i) {
		elem = (uintptr_t)&data[i * BUF_SIZE];

		if (odp_stash_put_ptr(stash, &elem, 1U) != 1U) {
			ODPH_ERR("Error filling buffer stash\n");
			return false;
		}
	}

	return true;
}

static odp_bool_t setup_test(prog_config_t *config)
{
	odp_dma_param_t dma_params;
	odp_stash_param_t stash_params;

	odp_dma_param_init(&dma_params);
	dma_params.compl_mode_mask = ODP_DMA_COMPL_SYNC;
	config->dma = odp_dma_create(NULL, &dma_params);

	if (config->dma == ODP_DMA_INVALID) {
		ODPH_ERR("Error creating DMA session\n");
		return false;
	}

	config->stash_shm = odp_shm_reserve(NULL, BUF_SIZE * BUF_COUNT, ODP_CACHE_LINE_SIZE, 0);

	if (config->stash_shm == ODP_SHM_INVALID) {
		ODPH_ERR("Error creating buffer SHM\n");
		return false;
	}

	config->stash_data = odp_shm_addr(config->stash_shm);

	if (config->stash_data == NULL) {
		ODPH_ERR("Error resolving buffer SHM address\n");
		return false;
	}

	odp_stash_param_init(&stash_params);
	stash_params.type = ODP_STASH_TYPE_FIFO;
	stash_params.put_mode = ODP_STASH_OP_LOCAL;
	stash_params.get_mode = ODP_STASH_OP_LOCAL;
	stash_params.num_obj = BUF_COUNT;
	stash_params.obj_size = sizeof(uintptr_t);
	config->stash = odp_stash_create(NULL, &stash_params);

	if (config->stash == ODP_STASH_INVALID) {
		ODPH_ERR("Error creating buffer stash\n");
		return false;
	}

	return fill_buffer_stash(config->stash, config->stash_data);
}

static void init_config(prog_config_t *config)
{
	memset(config, 0, sizeof(*config));
	config->stash_shm = ODP_SHM_INVALID;
	config->stash = ODP_STASH_INVALID;
	config->dma = ODP_DMA_INVALID;
}

static void print_usage(void)
{
	printf("\n"
	       "Shared memory protocol\n"
	       "\n"
	       "Usage: " PROG_NAME " OPTIONS\n"
	       "\n"
	       "Optional OPTIONS:\n"
	       "\n"
	       "  -i, --peer_instance Peer instance.\n"
	       "  -h, --help          This help.\n"
	       "\n");
}

static parse_result_t parse_options(int argc, char **argv, prog_config_t *config)
{
	int opt, long_index;

	static const struct option longopts[] = {
		{ "peer_instance", required_argument, NULL, 'i'},
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};

	static const char *shortopts = "i:h";

	init_config(config);

	while (1) {
		opt = getopt_long(argc, argv, shortopts, longopts, &long_index);

		if (opt == -1)
			break;

		switch (opt) {
		case 'i':
			config->peer_inst = atoll(optarg);
			break;
		case 'h':
			print_usage();
			return PRS_TERM;
		case '?':
		default:
			print_usage();
			return PRS_NOK;
		}
	}

	return PRS_OK;
}

static parse_result_t setup_program(int argc, char **argv, prog_config_t *config)
{
	return parse_options(argc, argv, config);
}

static void init_stash(stash_t *stash)
{
	ring_u64_init(&stash->hdr);
	stash->ring_mask = RING_SIZE - 1;

	for (uint32_t i = 0U; i < RING_SIZE; i++)
		stash->data[i] = 0U;
}

static void init_shared(shared_config_t *shared_conf)
{
	init_stash(&shared_conf->tx_handle_channel1);
	init_stash(&shared_conf->tx_handle_channel2);
	init_stash(&shared_conf->rx_handle_channel1);
	init_stash(&shared_conf->rx_handle_channel2);
}

static uint32_t ipc_recv(prog_config_t *prog_conf, shared_config_t *shared_conf,
			 void *data[], uint32_t num)
{
	stash_t *rx_handles = prog_conf->peer_inst == 0U ?
			       &shared_conf->rx_handle_channel2 : &shared_conf->rx_handle_channel1;
	uint32_t num_recv;
	int32_t ret;
	stash_t *tx_handles = prog_conf->peer_inst == 0U ?
			       &shared_conf->tx_handle_channel1 : &shared_conf->tx_handle_channel2;

	num_recv = ring_u64_deq_multi(&rx_handles->hdr, rx_handles->ring_mask,
				      (uint64_t *)(uintptr_t)data, num);

	if (num_recv > 0U) {
		uintptr_t tx_bufs[num_recv];

		ret = odp_stash_get_ptr(prog_conf->stash, tx_bufs, num_recv);

		if (ret < 0)
			ODPH_ERR("Failed to refill TX buffer ring\n");
		else
			ring_u64_enq_multi(&tx_handles->hdr, tx_handles->ring_mask, tx_bufs, ret);
	}

	return num_recv;
}

static uint32_t ipc_send(prog_config_t *prog_conf, shared_config_t *shared_conf,
			 void *data[], uint32_t num)
{
	stash_t *tx_handles = prog_conf->peer_inst == 0U ?
			       &shared_conf->tx_handle_channel2 : &shared_conf->tx_handle_channel1;
	uint64_t bufs[num];
	uint32_t num_recv;
	odp_dma_transfer_param_t trs_params;
	odp_dma_seg_t *seg;
	stash_t *rx_handles = prog_conf->peer_inst == 0U ?
			       &shared_conf->rx_handle_channel1 : &shared_conf->rx_handle_channel2;

	num_recv = ring_u64_deq_multi(&tx_handles->hdr, tx_handles->ring_mask, bufs, num);

	if (num_recv == 0U)
		return 0U;

	odp_dma_transfer_param_init(&trs_params);
	trs_params.num_src = num_recv;
	trs_params.num_dst = num_recv;

	odp_dma_seg_t src_segs[num_recv];
	odp_dma_seg_t dst_segs[num_recv];

	for (uint32_t i = 0U; i < num_recv; ++i) {
		seg = &src_segs[i];
		seg->addr = data[i];
		seg->len = BUF_SIZE;
		seg->offset = 0U;
		seg->all_hints = 0U;
		seg = &dst_segs[i];
		seg->addr = (void *)(uintptr_t)bufs[i];
		seg->len = BUF_SIZE;
		seg->offset = 0U;
		seg->all_hints = 0U;
	}

	trs_params.src_seg = src_segs;
	trs_params.dst_seg = dst_segs;

	if (odp_dma_transfer(prog_conf->dma, &trs_params, NULL) != 1) {
		ODPH_ERR("DMA transfer failed\n");
		return 0U;
	}

	odp_stash_put_ptr(prog_conf->stash, (uintptr_t *)data, num_recv);
	ring_u64_enq_multi(&rx_handles->hdr, rx_handles->ring_mask, bufs, num_recv);

	return num_recv;
}

/* In init phase both sides need to push some amount of buffers for writing. */
static void run_test(prog_config_t *prog_conf, shared_config_t *shared_conf)
{
	int32_t ret;
	stash_t *handles = prog_conf->peer_inst == 0U ?
			       &shared_conf->tx_handle_channel1 : &shared_conf->tx_handle_channel2;
	void *data[MAX_OUT];
	odp_bool_t is_started = false;
	odp_time_t start;
	uint32_t num_recv, num_sent, tot_recv = 0U;
#if 0
	uint8_t payload = 1, *pl_ptr;
#endif
	uint64_t tot_tm;

	printf("***** MY PID: %lu\n", prog_conf->inst);
	sleep(30U);
	printf("***** READY\n");

	ret = odp_stash_get_ptr(prog_conf->stash, (uintptr_t *)data, MAX_OUT);

	if (ret < (int)MAX_OUT)
		ODPH_ERR("Initial buffer allocation failure\n");

	ring_u64_enq_multi(&handles->hdr, handles->ring_mask, (uint64_t *)data, ret);

	while (tot_recv < 1000000U) {
		if (prog_conf->peer_inst == 0U) {
			num_recv = ipc_recv(prog_conf, shared_conf, data, MAX_OUT);
			tot_recv += num_recv;

			if (num_recv > 0) {
				if (!is_started) {
					is_started = true;
					start = odp_time_local_strict();
				}
#if 0
				printf("Received %u buffers:\n", num_recv);

				for (uint32_t i = 0U; i < num_recv; ++i) {
					printf("    buf[%u]: ", i);
					pl_ptr = data[i];

					for (uint32_t j = 0U; j < BUF_SIZE; ++j)
						printf("%u ", pl_ptr[j]);

					printf("\n");
				}

				printf("\n");
#endif
				num_sent = ipc_send(prog_conf, shared_conf, data, num_recv);

				if (num_sent < num_recv)
					odp_stash_put_ptr(prog_conf->stash,
							  (uintptr_t *)&data[num_sent],
							  num_recv - num_sent);
			}
		} else {
			ret = odp_stash_get_ptr(prog_conf->stash, (uintptr_t *)data, MAX_OUT);

			if (ret > 0) {
#if 0
				for (int i = 0; i < ret; ++i)
					memset(data[i], payload++, BUF_SIZE);

#endif
				num_sent = ipc_send(prog_conf, shared_conf, data, MAX_OUT);

				if (num_sent < MAX_OUT)
					odp_stash_put_ptr(prog_conf->stash,
							  (uintptr_t *)&data[num_sent],
							  MAX_OUT - num_sent);

				num_recv = ipc_recv(prog_conf, shared_conf, data, MAX_OUT);

				if (num_recv > 0)
					odp_stash_put_ptr(prog_conf->stash, (uintptr_t *)data,
							  num_recv);
			}
		}
	}

	tot_tm = odp_time_diff_ns(odp_time_local_strict(), start);

	if (prog_conf->peer_inst == 0U)
		printf("***** DONE:\n"
		       "    received bufs:   %u\n"
		       "    time (nsec):     %" PRIu64 "\n"
		       "    tput (B/sec):    %" PRIu64 "\n", tot_recv, tot_tm,
		       (uint64_t)(tot_recv * BUF_SIZE / ((double)tot_tm / ODP_TIME_SEC_IN_NS)));
}

static void drain_stash(odp_stash_t stash)
{
	uintptr_t elem;

	while (odp_stash_get_ptr(stash, &elem, 1U) == 1U);
}

static void teardown(prog_config_t *config)
{
	if (config->stash != ODP_STASH_INVALID) {
		drain_stash(config->stash);
		(void)odp_stash_destroy(config->stash);
	}

	if (config->stash_shm != ODP_SHM_INVALID)
		(void)odp_shm_free(config->stash_shm);

	if (config->dma != ODP_DMA_INVALID)
		(void)odp_dma_destroy(config->dma);
}

int main(int argc, char **argv)
{
	odph_helper_options_t odph_opts;
	odp_init_t init_param;
	odp_instance_t odp_instance;
	odp_shm_t shm_cfg = ODP_SHM_INVALID;
	int ret = EXIT_SUCCESS;
	parse_result_t parse_res;

	argc = odph_parse_options(argc, argv);

	if (odph_options(&odph_opts) == -1) {
		ODPH_ERR("Error while reading ODP helper options, exiting\n");
		exit(EXIT_FAILURE);
	}

	odp_init_param_init(&init_param);
	init_param.mem_model = odph_opts.mem_model;

	if (odp_init_global(&odp_instance, &init_param, NULL)) {
		ODPH_ERR("ODP global init failed, exiting\n");
		exit(EXIT_FAILURE);
	}

	if (odp_init_local(odp_instance, ODP_THREAD_CONTROL)) {
		ODPH_ERR("ODP local init failed, exiting\n");
		exit(EXIT_FAILURE);
	}

	parse_res = setup_program(argc, argv, &prog_conf);

	if (parse_res == PRS_NOK) {
		ret = EXIT_FAILURE;
		goto out;
	}

	if (parse_res == PRS_TERM) {
		ret = EXIT_SUCCESS;
		goto out;
	}

	if (!setup_test(&prog_conf)) {
		ret = EXIT_FAILURE;
		goto out;
	}

	if (prog_conf.peer_inst == 0) {
		printf("***** RESERVING\n");
		shm_cfg = odp_shm_reserve(PROG_NAME "_cfg", sizeof(*shared_conf),
					  ODP_CACHE_LINE_SIZE, ODP_SHM_EXPORT);

		if (shm_cfg == ODP_SHM_INVALID) {
			ODPH_ERR("Error reserving shared memory\n");
			ret = EXIT_FAILURE;
			goto out;
		}

		shared_conf = odp_shm_addr(shm_cfg);

		if (shared_conf == NULL) {
			ODPH_ERR("Error resolving shared memory address\n");
			ret = EXIT_FAILURE;
			goto out;
		}

		init_shared(shared_conf);
	} else {
		printf("***** IMPORTING\n");
		shm_cfg = odp_shm_import(PROG_NAME "_cfg", prog_conf.peer_inst, NULL);

		if (shm_cfg == ODP_SHM_INVALID) {
			ODPH_ERR("Error importing shared memory\n");
			ret = EXIT_FAILURE;
			goto out;
		}

		shared_conf = odp_shm_addr(shm_cfg);

		if (shared_conf == NULL) {
			ODPH_ERR("Error resolving imported shared memory address\n");
			ret = EXIT_FAILURE;
			goto out;
		}
	}

	prog_conf.inst = odp_instance;
	run_test(&prog_conf, shared_conf);

out:
	teardown(&prog_conf);

	if (shm_cfg != ODP_SHM_INVALID)
		(void)odp_shm_free(shm_cfg);

	if (odp_term_local()) {
		ODPH_ERR("ODP local terminate failed, exiting\n");
		exit(EXIT_FAILURE);
	}

	if (odp_term_global(odp_instance)) {
		ODPH_ERR("ODP global terminate failed, exiting\n");
		exit(EXIT_FAILURE);
	}

	return ret;
}
