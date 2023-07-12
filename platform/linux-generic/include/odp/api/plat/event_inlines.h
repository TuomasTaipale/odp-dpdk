/* Copyright (c) 2018, Linaro Limited
 * Copyright (c) 2022-2023, Nokia
 * All rights reserved.
 *
 * SPDX-License-Identifier:     BSD-3-Clause
 */

#ifndef ODP_PLAT_EVENT_INLINES_H_
#define ODP_PLAT_EVENT_INLINES_H_

#include <odp/api/buffer_types.h>
#include <odp/api/dma.h>
#include <odp/api/event_types.h>
#include <odp/api/packet_types.h>
#include <odp/api/timer_types.h>

#include <odp/api/plat/buffer_inline_types.h>
#include <odp/api/plat/event_inline_types.h>
#include <odp/api/plat/event_vector_inline_types.h>
#include <odp/api/plat/packet_inline_types.h>
#include <odp/api/plat/timer_inline_types.h>

/** @cond _ODP_HIDE_FROM_DOXYGEN_ */

#ifndef _ODP_NO_INLINE
	/* Inline functions by default */
	#define _ODP_INLINE static inline
	#define odp_event_type __odp_event_type
	#define odp_event_type_multi __odp_event_type_multi
	#define odp_event_user_area __odp_event_user_area
	#define odp_event_subtype __odp_event_subtype
	#define odp_event_types __odp_event_types
	#define odp_event_flow_id __odp_event_flow_id
	#define odp_event_flow_id_set __odp_event_flow_id_set
#else
	#define _ODP_INLINE
#endif

static inline odp_event_type_t __odp_event_type_get(odp_event_t event)
{
	int8_t type;

	type = _odp_event_hdr_field(event, int8_t, event_type);

	return (odp_event_type_t)type;
}

_ODP_INLINE odp_event_type_t odp_event_type(odp_event_t event)
{
	return __odp_event_type_get(event);
}

_ODP_INLINE int odp_event_type_multi(const odp_event_t event[], int num,
				     odp_event_type_t *type_out)
{
	int i;
	odp_event_type_t type = __odp_event_type_get(event[0]);

	for (i = 1; i < num; i++) {
		if (__odp_event_type_get(event[i]) != type)
			break;
	}

	*type_out = type;

	return i;
}

_ODP_INLINE void *odp_event_user_area(odp_event_t event)
{
	const odp_event_type_t type = __odp_event_type_get(event);

	switch (type) {
	case ODP_EVENT_BUFFER:
		return _odp_buffer_get((odp_buffer_t)event, void *, uarea_addr);
	case ODP_EVENT_PACKET:
		return _odp_pkt_get((odp_packet_t)event, void *, user_area);
	case ODP_EVENT_PACKET_VECTOR:
		return _odp_event_vect_get((odp_packet_vector_t)event, void *, uarea_addr);
	case ODP_EVENT_TIMEOUT:
		return _odp_timeout_hdr_field((odp_timeout_t)event, void *, uarea_addr);
	case ODP_EVENT_DMA_COMPL:
		return odp_dma_compl_user_area((odp_dma_compl_t)event);
	default:
		return NULL;
	}
}

_ODP_INLINE odp_event_subtype_t odp_event_subtype(odp_event_t event)
{
	if (__odp_event_type_get(event) != ODP_EVENT_PACKET)
		return ODP_EVENT_NO_SUBTYPE;

	return (odp_event_subtype_t)_odp_pkt_get((odp_packet_t)event, int8_t, subtype);
}

_ODP_INLINE odp_event_type_t odp_event_types(odp_event_t event,
					     odp_event_subtype_t *subtype)
{
	odp_event_type_t event_type = __odp_event_type_get(event);

	*subtype = event_type == ODP_EVENT_PACKET ?
			(odp_event_subtype_t)_odp_pkt_get((odp_packet_t)event, int8_t, subtype) :
			ODP_EVENT_NO_SUBTYPE;

	return event_type;
}

_ODP_INLINE uint32_t odp_event_flow_id(odp_event_t event)
{
	return _odp_event_hdr_field(event, uint8_t, flow_id);
}

_ODP_INLINE void odp_event_flow_id_set(odp_event_t event, uint32_t id)
{
	uint8_t *flow_id = _odp_event_hdr_ptr(event, uint8_t, flow_id);

	*flow_id = (uint8_t)id;
}

/** @endcond */

#endif
