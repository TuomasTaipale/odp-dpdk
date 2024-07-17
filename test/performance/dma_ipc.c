#include <odp_api.h>
#include <odp/helper/odph_api.h>

#include <inttypes.h>

#include "dma_ipc.h"

/*************************************************************************************************/

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
} ring_t;

static inline int cas_mo_u32(odp_atomic_u32_t *atom, uint32_t *old_val, uint32_t new_val,
			     int mo_success, int mo_failure)
{
	return __atomic_compare_exchange_n(&atom->v, old_val, new_val, 0 /* strong */, mo_success,
					   mo_failure);
}

static inline void ring_init(ring_t *ring)
{
	odp_atomic_init_u32(&ring->r.w_head, 0);
	odp_atomic_init_u32(&ring->r.w_tail, 0);
	odp_atomic_init_u32(&ring->r.r_head, 0);
	odp_atomic_init_u32(&ring->r.r_tail, 0);
}

static inline uint32_t ring_deq(ring_t *ring, uint32_t mask, uint64_t data[], uint32_t num)
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

	} while (odp_unlikely(cas_mo_u32(&ring->r.r_head, &head, new_head, __ATOMIC_ACQUIRE,
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

static inline void ring_enq(ring_t *ring, uint32_t mask, uint64_t data[], uint32_t num)
{
	uint32_t old_head, new_head, i;
	uint32_t size = mask + 1;

	/* Reserve a slot in the ring for writing */
	old_head = odp_atomic_fetch_add_u32(&ring->r.w_head, num);
	new_head = old_head + 1;

	/* Wait for the last reader to finish. This prevents overwrite when
	 * a reader has been left behind (e.g. due to an interrupt) and is
	 * still reading these slots. */
	while (odp_unlikely(new_head - odp_atomic_load_acq_u32(&ring->r.r_tail) >= size))
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

/*************************************************************************************************/

#define CHANNEL_SIZE 512U
#define SHM_NAME "dma_ipc_shared"

typedef struct ODP_ALIGNED_CACHE {
	uint32_t mask;
	ring_t hdr;
	uint64_t data[CHANNEL_SIZE];
} channel_t;

typedef struct {
	channel_t tx_pri;
	channel_t tx_sec;
	channel_t rx_pri;
	channel_t rx_sec;
	odp_shm_t shm;
} shared_t;

typedef enum {
	PRIMARY,
	SECONDARY
} role_t;

typedef struct {
	odp_dma_transfer_id_t transfer_id;
	void *usr;

	struct {
		uint32_t num;
		uint64_t data[];
	} pending;
} transfer_t;

typedef struct ODP_ALIGNED_CACHE {
	odp_dma_t dma;
	shared_t *shared;
	dma_ipc_ops_t ops;
	role_t role;
	odp_shm_t shm;
	odp_stash_t inflight;
	transfer_t transfers[];
} ipc_t;

void dma_ipc_param_init(dma_ipc_param_t *param)
{
	memset(param, 0, sizeof(*param));
}

static void init_channel(channel_t *channel)
{
	ring_init(&channel->hdr);
	channel->mask = CHANNEL_SIZE - 1;

	for (uint32_t i = 0U; i < CHANNEL_SIZE; i++)
		channel->data[i] = 0U;
}

static void init_shared(shared_t *shared)
{
	init_channel(&shared->tx_pri);
	init_channel(&shared->tx_sec);
	init_channel(&shared->rx_pri);
	init_channel(&shared->rx_sec);
}

static odp_bool_t reserve_shared(ipc_t *ipc)
{
	odp_shm_t shm;
	shared_t *shared;

	shm = odp_shm_reserve(SHM_NAME, sizeof(*shared), ODP_CACHE_LINE_SIZE,
			      ODP_SHM_EXPORT);

	if (shm == ODP_SHM_INVALID) {
		ODPH_ERR("Error allocating SHM for shared state\n");
		return false;
	}

	shared = (shared_t *)odp_shm_addr(shm);

	if (shared == NULL) {
		ODPH_ERR("Error resolving address for shared state\n");
		(void)odp_shm_free(shm);
		return false;
	}

	init_shared(shared);
	ipc->shared = shared;
	ipc->shared->shm = shm;

	return true;
}

static odp_bool_t import_shared(ipc_t *ipc, odp_instance_t peer)
{
	odp_shm_t shm;
	shared_t *shared;

	shm = odp_shm_import(SHM_NAME, peer, NULL);

	if (shm == ODP_SHM_INVALID) {
		ODPH_ERR("Error importing shared state SHM\n");
		return false;
	}

	shared = (shared_t *)odp_shm_addr(shm);

	if (shared == NULL) {
		ODPH_ERR("Error resolving address for shared state while importing\n");
		(void)odp_shm_free(shm);
		return false;
	}

	ipc->shared = shared;
	ipc->shared->shm = shm;

	return true;
}

static inline odp_bool_t fill_tx_buffers(dma_ipc_ops_t *ops, channel_t *channel, uint32_t num)
{
	void *bufs[num];
	int ret;

	ret = ops->alloc_fn(ops->pool, bufs, num, ops->data_len);

	if (odp_unlikely(ret < 0))
		return false;

	ring_enq(&channel->hdr, channel->mask, (uint64_t *)(uintptr_t)bufs, ret);

	return true;
}

static void destroy_inflight_stash(odp_stash_t stash)
{
	uintptr_t elem;

	while (odp_stash_get_ptr(stash, &elem, 1) == 1);

	odp_stash_destroy(stash);
}

static odp_bool_t create_inflight_stash(ipc_t *ipc, uint32_t num_infl)
{
	odp_stash_param_t stash_param;
	odp_stash_t stash;
	uintptr_t elem;

	odp_stash_param_init(&stash_param);
	stash_param.type = ODP_STASH_TYPE_FIFO;
	stash_param.put_mode = ODP_STASH_OP_LOCAL;
	stash_param.get_mode = ODP_STASH_OP_LOCAL;
	stash_param.num_obj = num_infl;
	stash_param.obj_size = sizeof(uintptr_t);
	stash = odp_stash_create(NULL, &stash_param);

	if (stash == ODP_STASH_INVALID) {
		ODPH_ERR("Error creating inflight stash\n");
		return false;
	}

	for (uint32_t i = 0U; i < num_infl; ++i) {
		elem = (uintptr_t)&ipc->transfers[i];

		if (odp_stash_put_ptr(stash, &elem, 1) != 1) {
			ODPH_ERR("Error filling inflight stash\n");
			destroy_inflight_stash(stash);
			return false;
		}
	}

	ipc->inflight = stash;

	return true;
}

static odp_bool_t create_dma_session(ipc_t *ipc)
{
	odp_dma_t dma;
	odp_dma_param_t dma_param;

	odp_dma_param_init(&dma_param);
	dma_param.compl_mode_mask = ODP_DMA_COMPL_SYNC | ODP_DMA_COMPL_POLL;
	dma = odp_dma_create("dma_ipc_dma", &dma_param);

	if (dma == ODP_DMA_INVALID) {
		ODPH_ERR("Error creating DMA session\n");
		return false;
	}

	ipc->dma = dma;

	return true;
}

dma_ipc_t * dma_ipc_open(const dma_ipc_param_t *param)
{
	odp_dma_capability_t dma_capa;
	uint32_t max_transfers, max_segs;
	odp_shm_t shm;
	ipc_t *ipc;
	const role_t role = param->peer ? SECONDARY : PRIMARY;
	odp_bool_t ret;

	if (param->ops == NULL) {
		ODPH_ERR("Bad ops handle\n");
		return NULL;
	}

	if (odp_dma_capability(&dma_capa) < 0) {
		ODPH_ERR("Error querying DMA capabilities\n");
		return NULL;
	}

	max_transfers = dma_capa.max_transfers;
	max_segs = dma_capa.max_dst_segs;
	shm = odp_shm_reserve("dma_ipc_handle", sizeof(*ipc) + max_transfers * (sizeof(transfer_t)
			                        + sizeof(uint64_t) * max_segs),
			      ODP_CACHE_LINE_SIZE, 0U);

	if (shm == ODP_SHM_INVALID) {
		ODPH_ERR("Error allocating SHM for handle\n");
		return NULL;
	}

	ipc = (ipc_t *)odp_shm_addr(shm);

	if (ipc == NULL) {
		ODPH_ERR("Error resolving address for handle\n");
		(void)odp_shm_free(shm);
		return NULL;
	}

	memset(ipc, 0, sizeof(*ipc));

	ret = role == PRIMARY ? reserve_shared(ipc) : import_shared(ipc, *param->peer);

	if (!ret) {
		(void)odp_shm_free(shm);
		return NULL;
	}

	if (!fill_tx_buffers(param->ops,
			     role == PRIMARY ? &ipc->shared->tx_pri : &ipc->shared->tx_sec,
			     max_segs)) {
		ODPH_ERR("Error filling TX buffer channel\n");
		(void)odp_shm_free(ipc->shared->shm);
		(void)odp_shm_free(shm);
		return NULL;
	}

	if (!create_inflight_stash(ipc, max_transfers)) {
		(void)odp_shm_free(ipc->shared->shm);
		(void)odp_shm_free(shm);
		return NULL;
	}

	if (!create_dma_session(ipc)) {
		destroy_inflight_stash(ipc->inflight);
		(void)odp_shm_free(ipc->shared->shm);
		(void)odp_shm_free(shm);
		return NULL;
	}

	ipc->ops = *param->ops;
	ipc->role = role;
	ipc->shm = shm;

	return (dma_ipc_t *)ipc;
}

void dma_ipc_close(dma_ipc_t *dma_ipc)
{
	ipc_t *ipc = (ipc_t *)dma_ipc;
	(void)odp_dma_destroy(ipc->dma);
	destroy_inflight_stash(ipc->inflight);
	(void)odp_shm_free(ipc->shared->shm);
	(void)odp_shm_free(ipc->shm);
}

int dma_ipc_recv(dma_ipc_t *dma_ipc, void *data[], uint32_t num)
{
	ipc_t *ipc = (ipc_t *)dma_ipc;
	const role_t role = ipc->role;
	channel_t *rx = role == PRIMARY ? &ipc->shared->rx_sec : &ipc->shared->rx_pri,
	*tx = role == PRIMARY ? &ipc->shared->tx_pri : &ipc->shared->tx_sec;
	uint32_t num_recv;

	num_recv = ring_deq(&rx->hdr, rx->mask, (uint64_t *)(uintptr_t)data, num);

	if (num_recv > 0U)
		if (odp_unlikely(!fill_tx_buffers(&ipc->ops, tx, num_recv)))
			ODPH_DBG("Error filling TX buffer channel\n");

	return num_recv;
}

int dma_ipc_send(dma_ipc_t *dma_ipc, void *data[], uint32_t num)
{
	ipc_t *ipc = (ipc_t *)dma_ipc;
	channel_t *tx, *rx;
	uint64_t bufs[num];
	uint32_t num_recv;
	odp_dma_transfer_param_t trs_param;
	odp_dma_seg_t *seg;
	const dma_ipc_ops_t *ops = &ipc->ops;
	int ret;

	if (ipc->role == PRIMARY) {
		tx = &ipc->shared->tx_sec;
		rx = &ipc->shared->rx_pri;
	} else {
		tx = &ipc->shared->tx_pri;
		rx =  &ipc->shared->rx_sec;
	}

	num_recv = ring_deq(&tx->hdr, tx->mask, bufs, num);

	if (num_recv == 0U)
		return 0;

	odp_dma_transfer_param_init(&trs_param);
	trs_param.num_src = num_recv;
	trs_param.num_dst = num_recv;

	odp_dma_seg_t src_segs[num_recv];
	odp_dma_seg_t dst_segs[num_recv];

	for (uint32_t i = 0U; i < num_recv; ++i) {
		seg = &src_segs[i];
		seg->addr = data[i];
		seg->len = ops->data_len;
		seg->offset = 0U;
		seg->all_hints = 0U;
		seg = &dst_segs[i];
		seg->addr = (void *)(uintptr_t)bufs[i];
		seg->len = ops->data_len;
		seg->offset = 0U;
		seg->all_hints = 0U;
	}

	trs_param.src_seg = src_segs;
	trs_param.dst_seg = dst_segs;
	ret = odp_dma_transfer(ipc->dma, &trs_param, NULL);

	if (odp_unlikely(ret <= 0)) {
		ODPH_DBG("DMA transfer failed\n");
		return ret;
	}

	ops->free_fn(ops->pool, data, num_recv);
	ring_enq(&rx->hdr, rx->mask, bufs, num_recv);

	return num_recv;
}

int dma_ipc_send_start(dma_ipc_t *dma_ipc, void *data[], uint32_t num,
		       const dma_ipc_compl_t *compl, dma_ipc_transfer_t *trs)
{
	ipc_t *ipc = (ipc_t *)dma_ipc;
	channel_t *tx = ipc->role == PRIMARY ? &ipc->shared->tx_sec : &ipc->shared->tx_pri;
	const odp_dma_compl_mode_t mode = compl->mode;
	odp_dma_t dma = ipc->dma;
	odp_dma_transfer_id_t trs_id = ODP_DMA_TRANSFER_ID_INVALID;
	transfer_t *transfer;
	uint64_t bufs[num], buf;
	uint32_t num_recv;
	odp_dma_transfer_param_t trs_param;
	odp_dma_seg_t *seg;
	const dma_ipc_ops_t *ops = &ipc->ops;
	odp_dma_compl_param_t compl_param;
	int ret;

	num_recv = ring_deq(&tx->hdr, tx->mask, bufs, num);

	if (num_recv == 0U) {
		return 0;
	}

	if (odp_unlikely(odp_stash_get_ptr(ipc->inflight, (uintptr_t *)&transfer, 1) != 1))
		return 0;

	if (mode == ODP_DMA_COMPL_POLL) {
		trs_id = odp_dma_transfer_id_alloc(dma);

		if (odp_unlikely(trs_id == ODP_DMA_TRANSFER_ID_INVALID)) {
			if (odp_stash_put_ptr(ipc->inflight, (uintptr_t *)&transfer, 1) != 1)
				ODPH_DBG("Error putting to inflight stash\n");

			return 0;
		}
	}

	odp_dma_transfer_param_init(&trs_param);
	trs_param.num_src = num_recv;
	trs_param.num_dst = num_recv;

	odp_dma_seg_t src_segs[num_recv];
	odp_dma_seg_t dst_segs[num_recv];

	for (uint32_t i = 0U; i < num_recv; ++i) {
		seg = &src_segs[i];
		seg->addr = data[i];
		seg->len = ops->data_len;
		seg->offset = 0U;
		seg->all_hints = 0U;
		seg = &dst_segs[i];
		buf = bufs[i];
		seg->addr = (void *)(uintptr_t)buf;
		transfer->pending.data[i] = buf;
		seg->len = ops->data_len;
		seg->offset = 0U;
		seg->all_hints = 0U;
	}

	trs_param.src_seg = src_segs;
	trs_param.dst_seg = dst_segs;
	odp_dma_compl_param_init(&compl_param);
	compl_param.compl_mode = compl->mode;

	if (compl->mode == ODP_DMA_COMPL_POLL) {
		compl_param.transfer_id = trs_id;
	} else {
		compl_param.event = compl->ev;
		compl_param.queue = compl->q;
	}

	compl_param.user_ptr = compl->usr;
	ret = odp_dma_transfer_start(dma, &trs_param, &compl_param);

	if (odp_unlikely(ret <= 0)) {
		if (trs_id != ODP_DMA_TRANSFER_ID_INVALID)
			odp_dma_transfer_id_free(dma, trs_id);

		if (odp_stash_put_ptr(ipc->inflight, (uintptr_t *)&transfer, 1) != 1)
			ODPH_DBG("Error putting to inflight stash\n");

		return ret;
	}

	transfer->transfer_id = mode == ODP_DMA_COMPL_POLL ? trs_id : ODP_DMA_TRANSFER_ID_INVALID;
	transfer->pending.num = num_recv;
	*(transfer_t **)trs = transfer;

	return num_recv;
}

static void finish_transfer(ipc_t *ipc, transfer_t *transfer, channel_t *channel)
{
	uint64_t *data = transfer->pending.data;
	const uint32_t num = transfer->pending.num;
	const dma_ipc_ops_t *ops = &ipc->ops;

	ring_enq(&channel->hdr, channel->mask, data, num);
	ops->free_fn(ops->pool, (void *)(uintptr_t)data, num);
	transfer->pending.num = 0U;

	if (odp_unlikely(odp_stash_put_ptr(ipc->inflight, (uintptr_t *)&transfer, 1) != 1))
		ODPH_DBG("Error putting to inflight stash\n");
}

int dma_ipc_done(dma_ipc_t *dma_ipc, dma_ipc_transfer_t trs)
{
	transfer_t *transfer = (transfer_t *)trs;
	ipc_t *ipc = (ipc_t *)dma_ipc;
	int ret = 1;
	channel_t *rx = ipc->role == PRIMARY ? &ipc->shared->rx_pri : &ipc->shared->rx_sec;

	if (transfer->transfer_id != ODP_DMA_TRANSFER_ID_INVALID) {
		ret = odp_dma_transfer_done(ipc->dma, transfer->transfer_id, NULL);

		if (ret > 0) {
			odp_dma_transfer_id_free(ipc->dma, transfer->transfer_id);
			finish_transfer(ipc, transfer, rx);
		}
	} else {
		finish_transfer(ipc, transfer, rx);
	}

	return ret;
}

/*
Open issues:
  - How to present DMA capabilities (e.g. max sessions, inflight, in data needing to match out
    data)
  - Helper module with explicit DMA?
  - Controlled teardown
  - Buffer size for both sizes
*/
