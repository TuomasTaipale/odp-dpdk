#ifndef ODP_API_DMA_IPC_H_
#define ODP_API_DMA_IPC_H_

#include <odp_api.h>

/**
 * TODO
 */
typedef void *dma_ipc_t;

/**
 * TODO
 */
typedef void *dma_ipc_transfer_t;

/**
 * TODO
 */
typedef struct {
	int (*alloc_fn)(void *pool, void *data[], int num, uint32_t data_len);
	void (*free_fn)(void *pool, void *data[], int num);
	void *pool;
	uint32_t data_len;
} dma_ipc_ops_t;

/**
 * TODO
 */
typedef struct {
	odp_instance_t *peer;
	dma_ipc_ops_t *ops;
} dma_ipc_param_t;

/**
 * TODO
 */
typedef enum {
	DMA_IPC_SYNC,
	DMA_IPC_POLL,
	DMA_IPC_EVENT
} dma_ipc_mode_t;

/**
 * TODO
 */
typedef struct {
	odp_dma_compl_mode_t mode;
	odp_event_t ev;
	odp_queue_t q;
	void *usr;
} dma_ipc_compl_t;

/**
 * TODO
 */
void dma_ipc_param_init(dma_ipc_param_t *param);

/**
 * TODO
 */
dma_ipc_t * dma_ipc_open(const dma_ipc_param_t *param);

/**
 * TODO
 */
void dma_ipc_close(dma_ipc_t *dma_ipc);

/**
 * TODO
 */
int dma_ipc_recv(dma_ipc_t *dma_ipc, void *data[], uint32_t num);

/**
 * TODO
 */
int dma_ipc_send(dma_ipc_t *dma_ipc, void *data[], uint32_t num);

/**
 * TODO
 */
int dma_ipc_send_start(dma_ipc_t *dma_ipc, void *data[], uint32_t num,
		       const dma_ipc_compl_t *compl, dma_ipc_transfer_t *trs);

/**
 * TODO
 */
int dma_ipc_done(dma_ipc_t *dma_ipc, dma_ipc_transfer_t trs);

#endif
