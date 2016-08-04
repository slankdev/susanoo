

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>

#include <stcp/ifaddr.h>
#include <stcp/config.h>


static char* af2str(af_t af)
{
    static char str[8];
    switch (af) {
        case STCP_AF_INET:
            strcpy(str, "INET");
            break;
        case STCP_AF_LINK:
            strcpy(str, "LINK");
            break;
        default:
            strcpy(str, "UNKNOWN");
            break;
    }
    return str;
}

void ifaddr::init(const void* d, size_t l)
{
    log& log = log::instance();
    log.push(af2str(family));

    switch (family) {
        case STCP_AF_INET:
            {
                fprintf(stderr, "Not Impl yet\n");
                exit(-1);
                break;
            }
        case STCP_AF_LINK:
            {
                if (l != ETHER_ADDR_LEN) {
                    fprintf(stderr, "Invalid Address len\n");
                    exit(-1);
                }
                memcpy(&raw.link, d, l);

#if 0
                char str[32];
                sprintf(str, "%02x:%02x:%02x:%02x:%02x:%02x", 
                        raw.link.addr_bytes[0], raw.link.addr_bytes[1], 
                        raw.link.addr_bytes[2], raw.link.addr_bytes[3], 
                        raw.link.addr_bytes[3], raw.link.addr_bytes[5]);
                log.write(INFO, "set address %s", str);
#else
                log.write(INFO, "set address %02x:%02x:%02x:%02x:%02x:%02x",
                        raw.link.addr_bytes[0], raw.link.addr_bytes[1], 
                        raw.link.addr_bytes[2], raw.link.addr_bytes[3], 
                        raw.link.addr_bytes[3], raw.link.addr_bytes[5]);
#endif
                break;
            }
        default:
            {
                fprintf(stderr, "Unknown address family\n");
                exit(-1);
                break;
            }
    }
    log.pop();
}


