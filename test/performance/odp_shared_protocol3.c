/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2024 Nokia
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <inttypes.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include <odp_api.h>
#include <odp/helper/odph_api.h>
#include "dma_ipc.h"

#define PROG_NAME "odp_shared_protocol"
#define MAX_OUT 15
#define RING_SIZE 128U
#define BUF_COUNT 32768U
#define BUF_SIZE 4096U
#define GIGAS 1000000000U
#define MEGAS 1000000U
#define KILOS 1000U
#define MAX_TRS 16U

typedef enum {
	PRS_OK,
	PRS_NOK,
	PRS_TERM,
	PRS_NOT_SUP
} parse_result_t;

typedef enum {
	SYNC,
	POLL
} trs_mode_t;

#define DEF_MODE SYNC

typedef struct {
	odp_instance_t inst;
	odp_instance_t peer_inst;
	odp_shm_t stash_shm;
	uint8_t *stash_data;
	odp_stash_t stash;
	dma_ipc_t *ipc;
	odp_bool_t is_latency;
	uint8_t mode;
} prog_config_t;

typedef struct {
	void *data[MAX_OUT];
	odp_bool_t is_running;
	dma_ipc_compl_t compl;
	dma_ipc_transfer_t trs;
} transfer_t;

typedef struct {
	transfer_t trss[MAX_TRS];
} transfers_t;

static prog_config_t prog_conf;
static transfers_t trss;
static odp_atomic_u32_t is_running;

static void terminate(int signal ODP_UNUSED)
{
	odp_atomic_store_u32(&is_running, 0U);
}

static odp_bool_t fill_buffer_stash(odp_stash_t stash, uint8_t *data)
{
	uintptr_t elem;

	for (uint32_t i = 0U; i < BUF_COUNT; ++i) {
		elem = (uintptr_t)&data[i * BUF_SIZE];

		if (odp_stash_put_ptr(stash, &elem, 1U) != 1U)
			return false;
	}

	return true;
}

static int alloc_bufs(void *pool, void *data[], int num, uint32_t data_len ODP_UNUSED)
{
	return odp_stash_get_ptr((odp_stash_t)pool, (uintptr_t *)data, num);
}

static void free_bufs(void *pool, void *data[], int num)
{
	(void)odp_stash_put_ptr((odp_stash_t)pool, (uintptr_t *)data, num);
}

static odp_bool_t setup_test(prog_config_t *config)
{
	odp_stash_param_t stash_param;
	dma_ipc_ops_t ops;
	dma_ipc_param_t ipc_param;

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

	odp_stash_param_init(&stash_param);
	stash_param.type = ODP_STASH_TYPE_FIFO;
	stash_param.put_mode = ODP_STASH_OP_LOCAL;
	stash_param.get_mode = ODP_STASH_OP_LOCAL;
	stash_param.num_obj = BUF_COUNT;
	stash_param.obj_size = sizeof(uintptr_t);
	config->stash = odp_stash_create(NULL, &stash_param);

	if (config->stash == ODP_STASH_INVALID) {
		ODPH_ERR("Error creating buffer stash\n");
		return false;
	}

	if (!fill_buffer_stash(config->stash, config->stash_data)) {
		ODPH_ERR("Error filling buffer stash\n");
		return false;
	}

	dma_ipc_param_init(&ipc_param);
	ops.alloc_fn = alloc_bufs;
	ops.free_fn = free_bufs;
	ops.pool = config->stash;
	ops.data_len = BUF_SIZE;
	ipc_param.ops = &ops;
	ipc_param.peer = config->peer_inst == 0 ? NULL : &config->peer_inst;
	config->ipc = dma_ipc_open(&ipc_param);

	if (config->ipc == NULL) {
		ODPH_ERR("Error opening IPC connection\n");
		return false;
	}

	return true;
}

static void init_config(prog_config_t *config)
{
	memset(config, 0, sizeof(*config));
	config->stash_shm = ODP_SHM_INVALID;
	config->stash = ODP_STASH_INVALID;
	config->mode = SYNC;

	for (uint32_t i = 0U; i < ODPH_ARRAY_SIZE(trss.trss); ++i)
		trss.trss[i].compl.mode = ODP_DMA_COMPL_POLL;
}

static void print_usage(void)
{
	printf("\n"
	       "IPC performance test\n"
	       "\n"
	       "Usage: " PROG_NAME " OPTIONS\n"
	       "\n"
	       "Optional OPTIONS:\n"
	       "\n"
	       "  -i, --peer_instance Peer instance.\n"
	       "  -m, --mode          Transfer mode. %u by default.\n"
	       "                          0: sync\n"
	       "                          1: poll\n"
	       "  -l, --latency       Calculate latency.\n"
	       "  -h, --help          This help.\n"
	       "\n", DEF_MODE);
}

static parse_result_t check_options(prog_config_t *config)
{
	if (config->mode != SYNC && config->mode != POLL) {
		ODPH_ERR("Invalid transfer mode: %u\n", config->mode);
		return PRS_NOK;
	}

	return PRS_OK;
}

static parse_result_t parse_options(int argc, char **argv, prog_config_t *config)
{
	int opt, long_index;

	static const struct option longopts[] = {
		{ "peer_instance", required_argument, NULL, 'i'},
		{ "mode", required_argument, NULL, 'm'},
		{ "latency", no_argument, NULL, 'l'},
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};

	static const char *shortopts = "i:m:lh";

	init_config(config);

	while (1) {
		opt = getopt_long(argc, argv, shortopts, longopts, &long_index);

		if (opt == -1)
			break;

		switch (opt) {
		case 'i':
			config->peer_inst = atoll(optarg);
			break;
		case 'm':
			config->mode = atoi(optarg);
			break;
		case 'l':
			config->is_latency = true;
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

	return check_options(config);
}

static parse_result_t setup_program(int argc, char **argv, prog_config_t *config)
{
	struct sigaction action = { .sa_handler = terminate };

	if (sigemptyset(&action.sa_mask) == -1 || sigaddset(&action.sa_mask, SIGINT) == -1 ||
	    sigaddset(&action.sa_mask, SIGTERM) == -1 ||
	    sigaddset(&action.sa_mask, SIGHUP) == -1 || sigaction(SIGINT, &action, NULL) == -1 ||
	    sigaction(SIGTERM, &action, NULL) == -1 || sigaction(SIGHUP, &action, NULL) == -1) {
		ODPH_ERR("Error installing signal handler\n");
		return PRS_NOK;
	}

	return parse_options(argc, argv, config);
}

static void print_humanised(uint64_t value, const char *type)
{
	if (value > GIGAS)
		printf("%.2f G%s\n", (double)value / GIGAS, type);
	else if (value > MEGAS)
		printf("%.2f M%s\n", (double)value / MEGAS, type);
	else if (value > KILOS)
		printf("%.2f k%s\n", (double)value / KILOS, type);
	else
		printf("%" PRIu64 " %s\n", value, type);
}

static void run_test(prog_config_t *config)
{
	const trs_mode_t mode = config->mode;
	const odp_bool_t is_master = config->peer_inst == 0U, is_latency = config->is_latency;
	dma_ipc_t *ipc = config->ipc;
	void *data[MAX_OUT * MAX_TRS];
	int num_recv, num_sent, num_tot_sent, ret, num_to_send;
	odp_bool_t is_started = false;
	uint64_t num_tot = 0U, tot_tm = 0U, tot_lat = 0U;
	struct timespec ts1, ts2;
	odp_time_t start;
	transfer_t *trs;

	printf("starting, waiting for configuration, instance: %" PRIu64 ", peer: %" PRIu64 "\n",
	       config->inst, config->peer_inst);
	sleep(30U);
	printf("ready\n");

	while (odp_atomic_load_u32(&is_running)) {
		odp_event_t ev;

		ev = odp_schedule(NULL, ODP_SCHED_NO_WAIT);

		if (ev != ODP_EVENT_INVALID)
			printf("got event: %" PRIu64 "\n", odp_event_to_u64(ev));

		if (mode == SYNC) {
			if (is_master) {
				num_recv = dma_ipc_recv(ipc, data, MAX_OUT);

				if (num_recv > 0) {
					num_tot += num_recv;

					if (!is_started) {
						is_started = true;
						start = odp_time_local_strict();
					}

					if (is_latency) {
						for (int i = 0; i < num_recv; ++i) {
							memcpy(&ts1, data[i], sizeof(ts1));
							(void)clock_gettime(CLOCK_MONOTONIC, &ts2);
							tot_lat += ((ts2.tv_sec * ODP_TIME_SEC_IN_NS + ts2.tv_nsec) - (ts1.tv_sec * ODP_TIME_SEC_IN_NS + ts1.tv_nsec));
						}
					}

					num_sent = dma_ipc_send(ipc, data, num_recv);

					if (num_sent < num_recv)
						odp_stash_put_ptr(config->stash,
								(uintptr_t *)&data[num_sent],
								num_recv - num_sent);
				}
			} else {
				ret = odp_stash_get_ptr(config->stash, (uintptr_t *)data, MAX_OUT);

				if (ret > 0) {
					if (is_latency) {
						for (int i = 0; i < ret; ++i) {
							(void)clock_gettime(CLOCK_MONOTONIC, &ts1);
							memcpy(data[i], &ts1, sizeof(ts1));
						}
					}

					num_sent = dma_ipc_send(ipc, data, ret);

					if (num_sent < ret)
						odp_stash_put_ptr(config->stash,
								(uintptr_t *)&data[num_sent],
								ret - num_sent);

					num_recv = dma_ipc_recv(ipc, data, num_sent);

					if (num_recv > 0)
						odp_stash_put_ptr(config->stash, (uintptr_t *)data,
								num_recv);
				}
			}
		} else {
			if (is_master) {
				num_recv = dma_ipc_recv(ipc, data, MAX_OUT * MAX_TRS);

				if (num_recv > 0) {
					num_tot_sent = 0;
					num_tot += num_recv;

					if (!is_started) {
						is_started = true;
						start = odp_time_local_strict();
					}

					for (uint32_t i = 0U;
					     i < ODPH_ARRAY_SIZE(trss.trss) &&
					     num_tot_sent < num_recv; ++i) {
						trs = &trss.trss[i];

						if (trs->is_running) {
							if (dma_ipc_done(ipc, trs->trs) == 1) {
								trs->is_running = false;
							}

							continue;
						}

						num_to_send = ODPH_MIN(MAX_OUT, num_recv - num_tot_sent);
						num_sent = dma_ipc_send_start(ipc, &data[num_tot_sent], num_to_send,
									      &trs->compl, &trs->trs);
						num_sent = num_sent > 0 ? num_sent : 0;
						num_tot_sent += num_sent;

						if (num_sent < num_to_send)
							odp_stash_put_ptr(config->stash,
									  (uintptr_t *)&data[num_tot_sent],
									  num_to_send - num_sent);

						if (num_sent > 0)
							trs->is_running = true;
					}

					if (num_tot_sent < num_recv)
						odp_stash_put_ptr(config->stash,
								  (uintptr_t *)&data[num_tot_sent],
								  num_recv - num_tot_sent);
				}
			} else {
				for (uint32_t i = 0U; i < ODPH_ARRAY_SIZE(trss.trss); ++i) {
					trs = &trss.trss[i];

					if (trs->is_running) {
						if (dma_ipc_done(ipc, trs->trs) == 1) {
							trs->is_running = false;
						}

						continue;
					}

					ret = odp_stash_get_ptr(config->stash,
								(uintptr_t *)trs->data,
								MAX_OUT);

					if (ret <= 0)
						continue;

					num_sent = dma_ipc_send_start(ipc, trs->data, ret,
								      &trs->compl, &trs->trs);

					if (num_sent < ret)
						odp_stash_put_ptr(config->stash,
								  (uintptr_t *)&trs->data[num_sent],
								  ret - num_sent);

					if (num_sent > 0)
						trs->is_running = true;
				}

				num_recv = dma_ipc_recv(ipc, data, MAX_OUT * MAX_TRS);

				if (num_recv > 0)
					odp_stash_put_ptr(config->stash, (uintptr_t *)data,
							  num_recv);
			}
		}
	}

	if (is_master) {
		if (is_started)
			tot_tm = odp_time_diff_ns(odp_time_local_strict(), start);

		printf("\n======================\n\n"
		       "IPC performance test done\n\n"
		       "    buf size:      %u B\n"
		       "    received bufs: %" PRIu64 "\n"
		       "    time:          %" PRIu64 " nsec\n"
		       "    tput:          ", BUF_SIZE, num_tot, tot_tm);
		print_humanised(num_tot * BUF_SIZE / ((double)tot_tm / ODP_TIME_SEC_IN_NS), "B/s");

		if (is_latency)
			printf("    latency/buf:   %" PRIu64 " nsec\n", tot_lat / num_tot);

		printf("\n======================\n");
	}
}

static void drain_stash(odp_stash_t stash)
{
	uintptr_t elem;

	while (odp_stash_get_ptr(stash, &elem, 1U) == 1U);
}

static void teardown(prog_config_t *config)
{
	if (config->ipc != NULL)
		dma_ipc_close(config->ipc);

	if (config->stash != ODP_STASH_INVALID) {
		drain_stash(config->stash);
		(void)odp_stash_destroy(config->stash);
	}

	if (config->stash_shm != ODP_SHM_INVALID)
		(void)odp_shm_free(config->stash_shm);
}

int main(int argc, char **argv)
{
	odph_helper_options_t odph_opts;
	odp_init_t init_param;
	odp_instance_t odp_instance;
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

	if (odp_schedule_config(NULL) < 0) {
		ODPH_ERR("Error configuring scheduler\n");
		goto out;
	}

	odp_atomic_init_u32(&is_running, 1U);
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

	prog_conf.inst = odp_instance;
	run_test(&prog_conf);

out:
	teardown(&prog_conf);

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
