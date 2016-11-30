
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stcp/config.h>

namespace slank {

inline mempool* pool_create(
        const char* name,
        unsigned    n,
        unsigned    cache_size,
        uint16_t    priv_size,
        uint16_t    data_room_size,
        int         socket_id)
{
    return rte::pktmbuf_pool_create(name, n, cache_size,
            priv_size, data_room_size, socket_id);
}


} /* namespace slank */
