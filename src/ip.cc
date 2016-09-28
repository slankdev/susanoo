



#include <stcp/ip.h>
#include <stcp/socket.h>
#include <stcp/stcp.h>

namespace slank {


    
void ip_module::proc() 
{
    // while (m.rx_size() > 0) {
    //     ...
    // }

    while (m.tx_size() > 0) {
        mbuf* msg = m.tx_pop();
        
    }

    // m.proc();
}




void ip_module::stat()
{
    m.stat();
    printf("\n");
    printf("\tRouting-Table\n");
    printf("\t%-16s%-16s%-16s%-6s%-3s\n", "Destination", "Gateway", "Genmask", "Flags", "if");
    for (stcp_rtentry& rt : rttable) {

        std::string str_dest;
        if (rt.rt_flags & STCP_RTF_GATEWAY) {
            str_dest = "defalt";
        } else if (rt.rt_flags & STCP_RTF_LOCAL) {
            str_dest = "link-local";
        } else {
            str_dest = p_sockaddr_to_str(&rt.rt_route);
        }

        std::string flag_str = "";
        if (rt.rt_flags & STCP_RTF_GATEWAY  )   flag_str += "G";
        if (rt.rt_flags & STCP_RTF_MASK     )   flag_str += "M";
        if (rt.rt_flags & STCP_RTF_LOCAL    )   flag_str += "L";
        if (rt.rt_flags & STCP_RTF_BROADCAST)   flag_str += "B";

        std::string gateway_str;
        if (rt.rt_flags & STCP_RTF_LOCAL) {
            gateway_str = "*";
        } else {
            gateway_str = p_sockaddr_to_str(&rt.rt_gateway);
        }

        printf("\t%-16s%-16s%-16s%-6s%-3u\n",
                str_dest.c_str(),
                gateway_str.c_str(),
                p_sockaddr_to_str(&rt.rt_genmask),
                flag_str.c_str(),
                rt.rt_port);
    }
}


void ip_module::ioctl(uint64_t request, void* args)
{
    switch (request) {
        case STCP_SIOCADDRT:
        {
            const stcp_rtentry* rt = reinterpret_cast<const stcp_rtentry*>(args); 
            ioctl_siocaddrt(rt);
            break;
        }
        case STCP_SIOCADDGW:
        {
            stcp_rtentry* rt = reinterpret_cast<stcp_rtentry*>(args); 
            ioctl_siocaddgw(rt);
            break;
        }
        case STCP_SIOCDELRT:
        {
            stcp_rtentry* rt = reinterpret_cast<stcp_rtentry*>(args); 
            ioctl_siocdelrt(rt);
            break;
        }
        case STCP_SIOCGETRTS:
        {
            std::vector<stcp_rtentry>** table = 
                reinterpret_cast<std::vector<stcp_rtentry>**>(args);
            ioctl_siocgetrts(table);
            break;
        }
        default:
        {
            throw slankdev::exception("invalid arguments");
            break;
        }
    }
}





/*
 * This function evaluate all elements
 * of stcp_rtentry.
 */
void ip_module::ioctl_siocaddrt(const stcp_rtentry* rt)
{
    rttable.push_back(*rt);
}


/*
 * This function evaluate only these.
 *  - rt.rt_port
 *  - rt.rt_gateway
 */
void ip_module::ioctl_siocaddgw(stcp_rtentry* rt)
{
    rt->rt_route.inet_addr(0, 0, 0, 0);
    rt->rt_genmask.inet_addr(0, 0, 0, 0);
    rt->rt_flags = STCP_RTF_GATEWAY;
    rttable.push_back(*rt);
}


void ip_module::ioctl_siocdelrt(const stcp_rtentry* rt)
{
    for (size_t i=0; i<rttable.size(); i++) {
        if (*rt == rttable[i]) {
            rttable.erase(rttable.begin() + i);
            return;
        }
    }
    throw slankdev::exception("not found routing info");
}

void ip_module::ioctl_siocgetrts(std::vector<stcp_rtentry>** table)
{
    *table = &rttable;
}




void ip_module::route_resolv(const stcp_sockaddr* dst, stcp_sockaddr* next, uint8_t* port)
{
    dpdk_core& dpdk = core::instance().dpdk;

    for (size_t i=0; i<dpdk.devices.size(); i++) {
        if (is_linklocal(i, dst)) {
            *next = *dst;
            *port = i;
            return;
        }

    }

    for (stcp_rtentry& rt : rttable) {
        if (rt.rt_flags & STCP_RTF_GATEWAY) {
            *next = rt.rt_gateway;
            *port = rt.rt_port;
            return;
        }
    }

    throw slankdev::exception("not found route");
}

bool ip_module::is_linklocal(uint8_t port, const stcp_sockaddr* addr)
{
    dpdk_core& dpdk = core::instance().dpdk;
    stcp_sockaddr inaddr;
    stcp_sockaddr inmask;
    stcp_sockaddr innet;
    bool inaddr_exist = false;
    bool inmask_exist = false;

    for (ifaddr& ifa : dpdk.devices[port].addrs) {
        if (ifa.family == STCP_AF_INET) {
            inaddr = ifa.raw;
            inaddr_exist = true;
        }

        if (ifa.family == STCP_AF_INMASK) {
            inmask = ifa.raw;
            inmask_exist = true;
        }
    }

    if (!inaddr_exist || !inmask_exist) {
        throw slankdev::exception("inaddr or inmask is not exist");
    }
    
    stcp_sockaddr_in* inaddr_sin = reinterpret_cast<stcp_sockaddr_in*>(&inaddr);
    stcp_sockaddr_in* inmask_sin = reinterpret_cast<stcp_sockaddr_in*>(&inmask);
    stcp_sockaddr_in* innet_sin  = reinterpret_cast<stcp_sockaddr_in*>(&innet );
    
    for (int i=0; i<4; i++) {
        innet_sin->sin_addr.addr_bytes[i] =   inaddr_sin->sin_addr.addr_bytes[i] 
                                            & inmask_sin->sin_addr.addr_bytes[i];
    }

    const stcp_sockaddr_in* sin = reinterpret_cast<const stcp_sockaddr_in*>(addr);
    for (int i=0; i<4; i++) {
        if ((inmask_sin->sin_addr.addr_bytes[i] & sin->sin_addr.addr_bytes[i])
                != innet_sin->sin_addr.addr_bytes[i])
            return false;
    }
    return true;
}

void ip_module::sendto(const void* buf, size_t bufsize, const stcp_sockaddr* dst)
{
    mbuf* msg = 
        rte::pktmbuf_alloc(::slank::core::instance().dpdk.get_mempool());
    copy_to_mbuf(msg, buf, bufsize);
 
    tx_push(msg, dst);
}


// #<{(| ip hdr |)}>#
// 0x45, 0x00, 0x00, 0x54, 0x7e, 0x4d, 0x40, 0x00, 
// 0x40, 0x01, 0x7e, 0x9a, 0xc0, 0xa8, 0xde, 0x0b, 
// 0xc0, 0xa8, 0xde, 0x64, 
void ip_module::tx_push(mbuf* msg, const stcp_sockaddr* dst)
{
    const stcp_sockaddr_in* sin = reinterpret_cast<const stcp_sockaddr_in*>(dst);
    
    stcp_ip_header* ih 
        = reinterpret_cast<stcp_ip_header*>(mbuf_push(msg, sizeof(stcp_ip_header)));
    
    ih->version_ihl        = 0x45;
    ih->type_of_service   = 0x00;
    ih->total_length      = rte::bswap16(0x0054);
    ih->packet_id         = rte::bswap16(0x7e4d);
    ih->fragment_offset   = rte::bswap16(0x4000);
    ih->time_to_live      = 0x40;
    ih->next_proto_id     = 0x01;
    ih->hdr_checksum      = rte::bswap16(0x7e9a);
    ih->src.addr_bytes[0] = 192;
    ih->src.addr_bytes[1] = 168;
    ih->src.addr_bytes[2] = 222;
    ih->src.addr_bytes[3] = 11 ;
    ih->dst.addr_bytes[0] = sin->sin_addr.addr_bytes[0];
    ih->dst.addr_bytes[1] = sin->sin_addr.addr_bytes[1];
    ih->dst.addr_bytes[2] = sin->sin_addr.addr_bytes[2];
    ih->dst.addr_bytes[3] = sin->sin_addr.addr_bytes[3];

    stcp_sockaddr next;
    uint8_t port;
    route_resolv(dst, &next, &port);

    msg->port = port;
    m.tx_push(msg);
}


};
