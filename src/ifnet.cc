

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>

#include <stcp/config.h>
#include <stcp/ifnet.h>
#include <stcp/stcp.h>
#include <stcp/socket.h>
#include <stcp/arch/dpdk/rte.h>
#include <stcp/dataplane.h>



namespace stcp {



void ifnet::init()
{
    eth_conf port_conf;
    memset(&port_conf, 0, sizeof port_conf);
    port_conf.rxmode.max_rx_pkt_len = ETHER_MAX_LEN;
    rte::eth_dev_configure(port_id, num_rx_rings, num_tx_rings, &port_conf);

    dataplane& d = core::dplane;
    for (uint16_t ring=0; ring<num_rx_rings; ring++) {
        rte::eth_rx_queue_setup(port_id, ring, rx_ring_size,
                rte::eth_dev_socket_id(port_id), NULL, d.mp);
    }
    for (uint16_t ring=0; ring<num_tx_rings; ring++) {
        rte::eth_tx_queue_setup(port_id, ring, tx_ring_size,
                rte::eth_dev_socket_id(port_id), NULL);
    }
    rte::eth_dev_start(port_id);

    if (promiscuous_mode)
        rte::eth_promiscuous_enable(port_id);

    if (rte::eth_dev_socket_id(port_id) > 0 &&
            rte::eth_dev_socket_id(port_id) != (int)rte::socket_id()) {
        std::string str;
        str = "WARNING: port" + std::to_string(port_id);
        str += "is on remote NUMA node to ";
        str += "polling thread. \n\tPerformance will ";
        str += "not be optimal. \n ";
        throw rte::exception(str.c_str());
    }



    struct stcp_ether_addr addr;
    rte::eth_macaddr_get(port_id, &addr);
    stcp_sockaddr s(STCP_AF_LINK);
    for (size_t i=0; i<stcp_ether_addr::addrlen; i++)
        s.sa_data[i] = addr.addr_bytes[i];
    ifaddr ifa(STCP_AF_LINK, &s);
    addrs.push_back(ifa);
}

uint16_t ifnet::io_rx()
{
    mbuf* bufs[BURST_SIZE];
    uint16_t num_rx = rte::eth_rx_burst(port_id, 0, bufs, BURST_SIZE);
    if (unlikely(num_rx == 0)) return 0;

    for (uint16_t i=0; i<num_rx; i++) {
        rx.push(bufs[i]);
    }

    return num_rx;
}

uint16_t ifnet::io_tx(size_t num_request_to_send)
{
    if (num_request_to_send > tx.size()) {
        num_request_to_send = tx.size();
    }

    mbuf* bufs[BURST_SIZE];
    uint16_t num_tx_sum = 0;
    size_t i=0;
    for (size_t num_sent=0; num_sent<num_request_to_send; num_sent+=i) {
        for (i=0; i+num_sent<num_request_to_send; i++) {
            bufs[i] = tx_pop();
        }
        uint16_t num_tx = rte::eth_tx_burst(port_id, 0, bufs, i);

        if (num_tx < i) {
            for (uint16_t j=0; j<i-num_tx; j++) {
                rte::pktmbuf_free(bufs[num_tx+j]);
            }
        }
        num_tx_sum += num_tx;
    }

    return num_tx_sum;
}

static const char* af2str(stcp_sa_family af)
{
    switch (af) {
        case STCP_AF_LINK: return "AF_LINK";
        case STCP_AF_INET: return "AF_INET";
        case STCP_AF_INMASK: return "AF_INMASK";
        default : return "unknown";
    }
}


void ifnet::print_stat(size_t rootx, size_t rooty) const
{
    core::screen.move(rooty, rootx);
    core::screen.printwln(" %s: %s", name.c_str(), promiscuous_mode?"PROMISC":"");

    for (const ifaddr& ifa : addrs) {
        if (ifa.family == STCP_AF_LINK) {
            core::screen.printwln(
                    "  %-10s %02x:%02x:%02x:%02x:%02x:%02x "
                , af2str(ifa.family)
                , ifa.raw.sa_data[0], ifa.raw.sa_data[1]
                , ifa.raw.sa_data[2], ifa.raw.sa_data[3]
                , ifa.raw.sa_data[4], ifa.raw.sa_data[5]);
        } else if (ifa.family == STCP_AF_INET || ifa.family == STCP_AF_INMASK) {
            const struct stcp_sockaddr_in* sin =
                reinterpret_cast<const stcp_sockaddr_in*>(&ifa.raw);
            core::screen.printwln("  %-10s %d.%d.%d.%d "
                , af2str(ifa.family)
                , sin->sin_addr.addr_bytes[0], sin->sin_addr.addr_bytes[1]
                , sin->sin_addr.addr_bytes[2], sin->sin_addr.addr_bytes[3]);
        } else {
            core::screen.printwln("  unknown ");
        }
    }
}


void ifnet::ioctl(uint64_t request, void* arg)
{
    switch (request) {
        case STCP_SIOCSIFADDR:
        {
            stcp_ifreq* ifr = reinterpret_cast<stcp_ifreq*>(arg);
            ioctl_siocsifaddr(ifr);
            break;
        }
        case STCP_SIOCGIFADDR:
        {
            stcp_ifreq* ifr = reinterpret_cast<stcp_ifreq*>(arg);
            ioctl_siocgifaddr(ifr);
            break;
        }
        case STCP_SIOCSIFHWADDR:
        {
            stcp_ifreq* ifr = reinterpret_cast<stcp_ifreq*>(arg);
            ioctl_siocsifhwaddr(ifr);
            break;
        }
        case STCP_SIOCGIFHWADDR:
        {
            stcp_ifreq* ifr = reinterpret_cast<stcp_ifreq*>(arg);
            ioctl_siocgifhwaddr(ifr);
            break;
        }
        case STCP_SIOCSIFNETMASK:
        {
            stcp_ifreq* ifr = reinterpret_cast<stcp_ifreq*>(arg);
            ioctl_siocsifnetmask(ifr);
            break;
        }
        case STCP_SIOCGIFNETMASK:
        {
            stcp_ifreq* ifr = reinterpret_cast<stcp_ifreq*>(arg);
            ioctl_siocgifnetmask(ifr);
            break;
        }
        case STCP_SIOCPROMISC:
        {
            const uint64_t* val = reinterpret_cast<const uint64_t*>(arg);
            ioctl_siocpromisc(val);
            break;
        }
        default:
        {
            throw exception("invalid arguments");
            break;
        }
    }
}

/*
 * Description
 *      This function must call before calling core::init().
 * Argument
 *      one : set
 *      else: unset
 */
void ifnet::ioctl_siocpromisc(const uint64_t* val)
{
    promiscuous_mode = *val==1 ? true : false;
}


void ifnet::ioctl_siocsifaddr(const stcp_ifreq* ifr)
{
    bool in_addr_setted = false;

    for (size_t i=0; i<addrs.size(); i++) {
        if (addrs[i].family == STCP_AF_INET) {
            const struct stcp_sockaddr_in* sin =
                reinterpret_cast<const stcp_sockaddr_in*>(&ifr->if_addr);
            stcp_sockaddr_in* s = reinterpret_cast<stcp_sockaddr_in*>(&addrs[i].raw);
            s->sin_addr = sin->sin_addr;
            in_addr_setted = true;
        }
    }

    if (in_addr_setted == false) {
        struct ifaddr ifa_new(STCP_AF_INET, &ifr->if_addr);
        addrs.push_back(ifa_new);
    }

    stcp_in_addr ad;
    const stcp_sockaddr_in* s = reinterpret_cast<const stcp_sockaddr_in*>(&ifr->if_addr);
    for (size_t i=0; i<stcp_in_addr::addrlen; i++)
        ad.addr_bytes[i] = s->sin_addr.addr_bytes[i];
    core::ip.set_ipaddr(&ad);
}

void ifnet::ioctl_siocsifnetmask(const stcp_ifreq* ifr)
{
    bool in_addr_setted = false;

    for (size_t i=0; i<addrs.size(); i++) {
        if (addrs[i].family == STCP_AF_INMASK) {
            const struct stcp_sockaddr_in* sin =
                reinterpret_cast<const stcp_sockaddr_in*>(&ifr->if_addr);
            stcp_sockaddr_in* s = reinterpret_cast<stcp_sockaddr_in*>(&addrs[i].raw);
            s->sin_addr = sin->sin_addr;
            in_addr_setted = true;
        }
    }

    if (in_addr_setted == false) {
        struct ifaddr ifa_new(STCP_AF_INMASK, &ifr->if_addr);
        addrs.push_back(ifa_new);
    }
}


void ifnet::ioctl_siocgifaddr(stcp_ifreq* ifr)
{
    for (ifaddr ifa : addrs) {
        if (ifa.family == STCP_AF_INET) {
            struct stcp_sockaddr_in* sin =
                reinterpret_cast<stcp_sockaddr_in*>(&ifr->if_addr);
            struct stcp_sockaddr_in* s =
                reinterpret_cast<stcp_sockaddr_in*>(&ifa.raw);
            sin->sin_fam  = STCP_AF_INET;
            sin->sin_addr = s->sin_addr;
            return;
        }
    }
    throw exception("not found inet address");
}

void ifnet::ioctl_siocgifnetmask(stcp_ifreq* ifr)
{
    for (ifaddr ifa : addrs) {
        if (ifa.family == STCP_AF_INMASK) {
            struct stcp_sockaddr_in* sin =
                reinterpret_cast<stcp_sockaddr_in*>(&ifr->if_addr);
            struct stcp_sockaddr_in* s =
                reinterpret_cast<stcp_sockaddr_in*>(&ifa.raw);
            sin->sin_fam  = STCP_AF_INMASK;
            sin->sin_addr = s->sin_addr;
            return;
        }
    }
    throw exception("not found inet address");
}

void ifnet::ioctl_siocsifhwaddr(const stcp_ifreq* ifr)
{
    bool in_addr_setted = false;

    for (size_t i=0; i<addrs.size(); i++) {
        if (addrs[i].family == STCP_AF_LINK) {
            addrs[i].raw.sa_data[0] = ifr->if_hwaddr.sa_data[0];
            addrs[i].raw.sa_data[1] = ifr->if_hwaddr.sa_data[1];
            addrs[i].raw.sa_data[2] = ifr->if_hwaddr.sa_data[2];
            addrs[i].raw.sa_data[3] = ifr->if_hwaddr.sa_data[3];
            addrs[i].raw.sa_data[4] = ifr->if_hwaddr.sa_data[4];
            addrs[i].raw.sa_data[5] = ifr->if_hwaddr.sa_data[5];
            in_addr_setted = true;
        }
    }

    if (in_addr_setted == false) {
        struct ifaddr ifa_new(STCP_AF_LINK, &ifr->if_hwaddr);
        addrs.push_back(ifa_new);
    }
}


void ifnet::ioctl_siocgifhwaddr(stcp_ifreq* ifr)
{
    for (ifaddr ifa : addrs) {
        if (ifa.family == STCP_AF_LINK) {
            ifr->if_hwaddr.sa_data[0] = ifa.raw.sa_data[0];
            ifr->if_hwaddr.sa_data[1] = ifa.raw.sa_data[1];
            ifr->if_hwaddr.sa_data[2] = ifa.raw.sa_data[2];
            ifr->if_hwaddr.sa_data[3] = ifa.raw.sa_data[3];
            ifr->if_hwaddr.sa_data[4] = ifa.raw.sa_data[4];
            ifr->if_hwaddr.sa_data[5] = ifa.raw.sa_data[5];
            return;
        }
    }
    throw exception("not fount inet address");
}




} /* stcp */

