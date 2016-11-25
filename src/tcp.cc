

#include <assert.h>
#include <stcp/tcp.h>
#include <stcp/config.h>
#define UNUSED(x) (void)(x)

namespace slank {

size_t tcp_module::mss = 1460;

inline uint16_t data_length(const stcp_tcp_header* th,
        const stcp_ip_header* ih)
{
    uint16_t iptotlen = rte::bswap16(ih->total_length);
    uint16_t iphlen = (ih->version_ihl & 0x0f)<<2;
    uint16_t tcphlen  = ((th->data_off>>4)<<2);
    return iptotlen - iphlen - tcphlen;
}

inline void swap_port(stcp_tcp_header* th)
{
    uint16_t tmp = th->sport;
    th->sport    = th->dport;
    th->dport    = tmp;
}

inline bool HAVE(stcp_tcp_header* th, tcp_flags type)
{
    return ((th->tcp_flags & type) != 0x00);
}


/*
 * msg's head must points ip-header
 */
void tcp_module::tx_push(mbuf* msg, const stcp_sockaddr_in* dst)
{
    mbuf_pull(msg, sizeof(stcp_ip_header));
    core::ip.tx_push(msg, dst, STCP_IPPROTO_TCP);
}


stcp_tcp_sock::stcp_tcp_sock() :
    accepted(false),
    dead(false),
    head(nullptr),
    num_connected(0),
    state(TCPS_CLOSED),
    port(0),
    pair_port(0),
    si(0, 0)
{
    DEBUG("[%15p] SOCK CNSTRCTR \n", this);
}

/*
 * Constructor for LISTEN socket
 */
stcp_tcp_sock::stcp_tcp_sock(tcpstate s, uint16_t lp, uint16_t rp,
            uint32_t arg_iss, uint32_t arg_irs, stcp_tcp_sock* h) :
    accepted(false),
    dead(false),
    head(h),
    num_connected(0),
    state(s),
    port(lp),
    pair_port(rp),
    si(arg_iss, arg_irs)
{
    DEBUG("[%15p] SOCK CNSTRCTR(%s,%u,%u,%u,%u,%p) \n",
            this, tcpstate2str(state), port, pair_port, si.iss_H(), si.irs_H(), head);
}

stcp_tcp_sock::~stcp_tcp_sock()
{
    DEBUG("[%15p] SOCK DESTRUCTOR \n", this);
    if (head) {
        head->num_connected --;
    }
}

stcp_tcp_sock* stcp_tcp_sock::alloc_new_sock_connected(tcpstate st,
        uint16_t lp, uint16_t rp, uint32_t arg_iss, uint32_t arg_irs,
        stcp_tcp_sock* head)
{
    stcp_tcp_sock* s = new stcp_tcp_sock(st, lp, rp, arg_iss, arg_irs, head);
    core::tcp.socks.push_back(s);
    return s;
}




void stcp_tcp_sock::write(mbuf* msg)
{
    if (state != TCPS_ESTABLISHED) {
        std::string errstr = "Not Open Port state=";
        errstr += tcpstate2str(state);
        throw exception(errstr.c_str());
    }
    txq.push(msg);
}


mbuf* stcp_tcp_sock::read()
{
    while (rxq.size() == 0) {
        if (state == TCPS_CLOSED) {
            std::string errstr = "Not Open Port state=";
            errstr += tcpstate2str(state);
            throw exception(errstr.c_str());
        }
    }

    mbuf* m = rxq.pop();
    DEBUG("[%15p] READ datalen=%zd\n", this, rte::pktmbuf_pkt_len(m));
    return m;
}



/*
 * This function blocks until alloc connection.
 */
stcp_tcp_sock* stcp_tcp_sock::accept(struct stcp_sockaddr_in* addr)
{
    UNUSED(addr);

    while (wait_accept.size() == 0) ;

    /*
     * Dequeue wait_accept and return that.
     */
    stcp_tcp_sock* sock = wait_accept.pop();
    DEBUG("[%15p] ACCEPT return new socket [%p]\n", this, sock);
    return sock;
}



void stcp_tcp_sock::proc()
{

    /*
     * TODO
     * Proc msgs from txq to tcp_module
     */


    /*
     * Othre proc;
     */
    switch (state) {
        case TCPS_CLOSE_WAIT:
            proc_CLOSE_WAIT();
            break;
        case TCPS_ESTABLISHED:
            proc_ESTABLISHED();
            break;
        case TCPS_LISTEN:
        case TCPS_CLOSED:
        case TCPS_SYN_SENT:
        case TCPS_SYN_RCVD:
        case TCPS_FIN_WAIT_1:
        case TCPS_FIN_WAIT_2:
        case TCPS_CLOSING:
        case TCPS_LAST_ACK:
        case TCPS_TIME_WAIT:
            /*
             * TODO
             * Not Implement yet.
             * No Operation
             */
            break;
        default:
            throw exception("UNKNOWN TCP STATE!!!");
            break;
    }
}


void stcp_tcp_sock::proc_ESTABLISHED()
{
    while (!txq.empty()) {
        mbuf* msg = txq.pop();
        size_t data_len = rte::pktmbuf_pkt_len(msg);
        DEBUG("[%15p] proc_ESTABLISHED send(txq.pop(), %zd)\n",
                this, rte::pktmbuf_pkt_len(msg));

        stcp_tcp_header* th = reinterpret_cast<stcp_tcp_header*>(
                mbuf_push(msg, sizeof(stcp_tcp_header)));
        stcp_ip_header*  ih = reinterpret_cast<stcp_ip_header*>(
                mbuf_push(msg, sizeof(stcp_ip_header)));

        /*
         * Craft IP header for tcp checksum
         */
        ih->total_length  = rte::bswap16(rte::pktmbuf_pkt_len(msg));
        ih->next_proto_id = STCP_IPPROTO_TCP;
        ih->src           = addr.sin_addr;
        ih->dst           = pair.sin_addr;

        /*
         * Craft TCP header
         */
        th->sport     = port     ;
        th->dport     = pair_port;
        th->seq_num   = si.snd_nxt_N();
        th->ack_num   = si.rcv_nxt_N();
        th->data_off  = sizeof(stcp_tcp_header) >> 2 << 4;
        th->tcp_flags = TCPF_PSH|TCPF_ACK;
        th->rx_win    = si.snd_win_N();
        th->cksum     = 0x0000;
        th->tcp_urp   = 0; // TODO hardcode

        th->cksum = rte_ipv4_udptcp_cksum(
                reinterpret_cast<ipv4_hdr*>(ih), th);

        /*
         * send to ip module
         */
        core::tcp.tx_push(msg, &pair);

        /*
         * TODO KOKOJANAKUNE
         */
        si.snd_nxt_H(si.snd_nxt_H() + data_len);

    }
}


void stcp_tcp_sock::proc_CLOSE_WAIT()
{
    /*
     * TODO
     * These must be implemented
     */

    mbuf* msg = rte::pktmbuf_alloc(core::dpdk.get_mempool());

    /*
     * Init Mbuf
     */
    msg->pkt_len  = sizeof(stcp_tcp_header) + sizeof(stcp_ip_header);
    msg->data_len = sizeof(stcp_tcp_header) + sizeof(stcp_ip_header);
    stcp_ip_header*  ih = rte::pktmbuf_mtod<stcp_ip_header*>(msg);
    stcp_tcp_header* th = rte::pktmbuf_mtod_offset<stcp_tcp_header*>(
                                    msg, sizeof(stcp_ip_header));

    /*
     * Craft IP hdr for TCP-checksum
     */
    ih->src = addr.sin_addr;
    ih->dst = pair.sin_addr;
    ih->next_proto_id = STCP_IPPROTO_TCP;
    ih->total_length  = rte::bswap16(
            sizeof(stcp_tcp_header) + sizeof(stcp_ip_header));


    /*
     * Craft TCP FIN
     */
    th->sport     = port     ;
    th->dport     = pair_port;
    th->seq_num   = si.snd_nxt_N();
    th->ack_num   = si.rcv_nxt_N();
    th->data_off  = sizeof(stcp_tcp_header)>>2 << 4;
    th->tcp_flags = TCPF_FIN|TCPF_ACK;
    th->rx_win    = si.snd_win_N();
    th->cksum     = 0x0000;
    th->tcp_urp   = 0x0000; // TODO hardcode

    th->cksum = rte_ipv4_udptcp_cksum(
            reinterpret_cast<ipv4_hdr*>(ih), th);

    /*
     * Update Stream infos
     */
    si.snd_nxt_inc_H(1);

    /*
     * Send packet
     */
    core::tcp.tx_push(msg, &pair);

    /*
     * Move TCP-State
     */
    move_state(TCPS_LAST_ACK);
}



void tcp_module::proc()
{
    for (size_t i=0; i<socks.size(); i++) {
        socks[i]->proc();
    }
}



void stcp_tcp_sock::proc_RST(mbuf* msg, stcp_tcp_header* th, stcp_sockaddr_in* dst)
{
    UNUSED(msg);
    UNUSED(th);
    UNUSED(dst);

    switch (state) {
        /*
         * TODO implement
         * each behaviours
         */
        case TCPS_ESTABLISHED:
        case TCPS_LISTEN:
        case TCPS_CLOSED:
        case TCPS_SYN_SENT:
        case TCPS_SYN_RCVD:
        case TCPS_FIN_WAIT_1:
        case TCPS_FIN_WAIT_2:
        case TCPS_CLOSE_WAIT:
        case TCPS_CLOSING:
        case TCPS_LAST_ACK:
        case TCPS_TIME_WAIT:
        default:
        {
            throw exception("NOT implement");
            break;
        }

    }
}


void stcp_tcp_sock::bind(const struct stcp_sockaddr_in* addr, size_t addrlen)
{
    if (addrlen < sizeof(sockaddr_in))
        throw exception("Invalid addrlen");
    port = addr->sin_port;
}
void stcp_tcp_sock::listen(size_t backlog)
{
    if (backlog < 1) throw exception("OKASHII");
    num_connected = 0;
    max_connect   = backlog;
    move_state(TCPS_LISTEN);
}


void stcp_tcp_sock::move_state_DEBUG(tcpstate next_state)
{
    DEBUG("[%15p] %s -> %s (MOVE state debug) \n", this,
            tcpstate2str(state),
            tcpstate2str(next_state) );
    state = next_state;
}

void stcp_tcp_sock::move_state(tcpstate next_state)
{
// TODO
#if 0
    DEBUG("[%15p] %s -> %s \n", this,
            tcpstate2str(state),
            tcpstate2str(next_state) );
#endif

    switch (state) {
        case TCPS_CLOSED     :
            move_state_from_CLOSED(next_state);
            break;
        case TCPS_LISTEN     :
            move_state_from_LISTEN(next_state);
            break;
        case TCPS_SYN_SENT   :
            move_state_from_SYN_SENT(next_state);
            break;
        case TCPS_SYN_RCVD   :
            move_state_from_SYN_RCVD(next_state);
            break;
        case TCPS_ESTABLISHED:
            move_state_from_ESTABLISHED(next_state);
            break;
        case TCPS_FIN_WAIT_1 :
            move_state_from_FIN_WAIT_1(next_state);
            break;
        case TCPS_FIN_WAIT_2 :
            move_state_from_FIN_WAIT_2(next_state);
            break;
        case TCPS_CLOSE_WAIT :
            move_state_from_CLOSE_WAIT(next_state);
            break;
        case TCPS_CLOSING    :
            move_state_from_CLOSING(next_state);
            break;
        case TCPS_LAST_ACK   :
            move_state_from_LAST_ACK(next_state);
            break;
        case TCPS_TIME_WAIT  :
            move_state_from_TIME_WAIT(next_state);
            break;
        default:
            throw exception("invalid tcp sock state");
            break;
    }
}


void stcp_tcp_sock::move_state_from_CLOSED(tcpstate next_state)
{
    switch (next_state) {
        case TCPS_LISTEN:
        case TCPS_SYN_SENT:
            state = next_state;
            break;
        default:
            throw exception("invalid state-change");
            break;
    }
}
void stcp_tcp_sock::move_state_from_LISTEN(tcpstate next_state)
{
    switch (next_state) {
        case TCPS_CLOSED:
        case TCPS_SYN_SENT:
        case TCPS_SYN_RCVD:
            state = next_state;
            break;
        default:
            throw exception("invalid state-change");
            break;
    }
}
void stcp_tcp_sock::move_state_from_SYN_SENT(tcpstate next_state)
{
    switch (next_state) {
        case TCPS_CLOSED:
        case TCPS_SYN_RCVD:
        case TCPS_ESTABLISHED:
            state = next_state;
            break;
        default:
            throw exception("invalid state-change");
            break;
    }
}
void stcp_tcp_sock::move_state_from_SYN_RCVD(tcpstate next_state)
{
    switch (next_state) {
        case TCPS_ESTABLISHED:
        case TCPS_FIN_WAIT_1:
            state = next_state;
            break;
        default:
            throw exception("invalid state-change");
            break;
    }
}
void stcp_tcp_sock::move_state_from_ESTABLISHED(tcpstate next_state)
{
    switch (next_state) {
        case TCPS_FIN_WAIT_1:
        case TCPS_CLOSE_WAIT:
            state = next_state;
            break;
        default:
            throw exception("invalid state-change");
            break;
    }
}
void stcp_tcp_sock::move_state_from_FIN_WAIT_1(tcpstate next_state)
{
    switch (next_state) {
        case TCPS_CLOSING:
        case TCPS_FIN_WAIT_2:
            state = next_state;
            break;
        default:
            throw exception("invalid state-change");
            break;
    }
}
void stcp_tcp_sock::move_state_from_FIN_WAIT_2(tcpstate next_state)
{
    switch (next_state) {
        case TCPS_TIME_WAIT:
            state = next_state;
            break;
        default:
            throw exception("invalid state-change");
            break;
    }
}
void stcp_tcp_sock::move_state_from_CLOSE_WAIT(tcpstate next_state)
{
    switch (next_state) {
        case TCPS_LAST_ACK:
            state = next_state;
            break;
        default:
            throw exception("invalid state-change");
            break;
    }
}
void stcp_tcp_sock::move_state_from_CLOSING(tcpstate next_state)
{
    switch (next_state) {
        case TCPS_TIME_WAIT:
            state = next_state;
            break;
        default:
            throw exception("invalid state-change");
            break;
    }
}
void stcp_tcp_sock::move_state_from_LAST_ACK(tcpstate next_state)
{
    switch (next_state) {
        case TCPS_CLOSED:
            state = next_state;
            dead = true;
            break;
        default:
            throw exception("invalid state-change");
            break;
    }
}
void stcp_tcp_sock::move_state_from_TIME_WAIT(tcpstate next_state)
{
    switch (next_state) {
        case TCPS_CLOSED:
            state = next_state;
            dead = true;
            break;
        default:
            throw exception("invalid state-change");
            break;
    }
}









/*
 * msg: points ip_header
 */
void stcp_tcp_sock::rx_push(mbuf* msg,stcp_sockaddr_in* src)
{
    stcp_ip_header*  ih
        = rte::pktmbuf_mtod<stcp_ip_header*>(msg);
    stcp_tcp_header* th
        = rte::pktmbuf_mtod_offset<stcp_tcp_header*>(msg, sizeof(stcp_ip_header));

    {
        /*
         * TODO Proc TCP-Option
         * ERASE zeroclear tcp options
         */
        uint8_t* buf = reinterpret_cast<uint8_t*>(th);
        buf += sizeof(stcp_tcp_header);
        size_t tcpoplen = th->data_off/4 - sizeof(stcp_tcp_header);
        memset(buf, 0x00, tcpoplen);
    }

    /*
     * TODO
     * Drop or Reply RSTACK to independent packet.
     */

    switch (state) {
        case TCPS_CLOSED:
            rx_push_CLOSED(msg, src, ih, th);
            break;
        case TCPS_LISTEN:
            rx_push_LISTEN(msg, src, ih, th);
            break;
        case TCPS_SYN_SENT:
            rx_push_SYN_SEND(msg, src, ih, th);
            break;
        case TCPS_SYN_RCVD:
        case TCPS_ESTABLISHED:
        case TCPS_FIN_WAIT_1:
        case TCPS_FIN_WAIT_2:
        case TCPS_CLOSE_WAIT:
        case TCPS_CLOSING:
        case TCPS_LAST_ACK:
        case TCPS_TIME_WAIT:
            rx_push_ELSESTATE(msg, src, ih, th);
            break;
        default:
            throw exception("OKASHII");
    }
}

void stcp_tcp_sock::rx_push_CLOSED(mbuf* msg, stcp_sockaddr_in* src,
        stcp_ip_header* ih, stcp_tcp_header* th)
{
    if (HAVE(th, TCPF_RST)) {
        rte::pktmbuf_free(msg);
    } else {
        if (HAVE(th, TCPF_ACK)) {
            swap_port(th);
            th->ack_num   = th->seq_num + rte::bswap32(data_length(th, ih));
            th->seq_num   = 0;
            th->tcp_flags = TCPF_RST|TCPF_ACK;
        } else {
            swap_port(th);
            th->seq_num   = th->ack_num;
            th->tcp_flags = TCPF_RST;
        }
        th->cksum     = 0x0000;
        th->tcp_urp   = 0x0000;

        th->cksum = rte_ipv4_udptcp_cksum(
            reinterpret_cast<ipv4_hdr*>(ih), th);

        core::tcp.tx_push(msg, src);
    }
    return;
}

void stcp_tcp_sock::rx_push_LISTEN(mbuf* msg, stcp_sockaddr_in* src,
        stcp_ip_header* ih, stcp_tcp_header* th)
{
    /*
     * 1: RST Check
     */
    if (HAVE(th, TCPF_RST)) {
        rte::pktmbuf_free(msg);
        return;
    }

    /*
     * 2: ACK Check
     */
    if (HAVE(th, TCPF_ACK)) {
        rte::pktmbuf_free(msg);
        return;
    }

    /*
     * 3: SYN Check
     */
    if (HAVE(th, TCPF_SYN)) {
        /*
         * Securty Check is not implemented
         * TODO: not imple yet
         */

        /*
         * Priority Check
         * TODO: not imple yet
         */

        stcp_tcp_sock* newsock = alloc_new_sock_connected(
                TCPS_SYN_RCVD, port, th->sport,
                rte::rand() % 0xffffffff, rte::bswap32(th->seq_num), this);
        num_connected ++;
        wait_accept.push(newsock);

        newsock->addr.sin_addr = ih->dst;
        newsock->pair.sin_addr = ih->src;

        newsock->si.rcv_nxt_H(rte::bswap32(th->seq_num) + 1);

        swap_port(th);
        th->seq_num   = newsock->si.iss_N();
        th->ack_num   = newsock->si.rcv_nxt_N();
        th->tcp_flags = TCPF_SYN|TCPF_ACK;
        th->rx_win    = newsock->si.snd_win_N();
        th->tcp_urp   = 0x0000; // TODO hardcode
        th->cksum     = 0x0000;

        th->cksum = rte_ipv4_udptcp_cksum(
                reinterpret_cast<ipv4_hdr*>(ih), th);

        core::tcp.tx_push(msg, src);

        newsock->si.snd_nxt_H(newsock->si.iss_H() + 1);
        newsock->si.snd_una_N(newsock->si.iss_N());
        return;
    }

    /*
     * 4: Else Text Control
     */
    throw exception("OKASHII");
}

void stcp_tcp_sock::rx_push_SYN_SEND(mbuf* msg, stcp_sockaddr_in* src,
        stcp_ip_header* ih, stcp_tcp_header* th)
{
    UNUSED(ih);

    /*
     * 1: ACK Check
     */
    if (HAVE(th, TCPF_ACK)) {
        if (rte::bswap32(th->ack_num) <= si.iss_H() ||
                rte::bswap32(th->ack_num) > si.snd_nxt_H()) {
            if (HAVE(th, TCPF_RST)) {
                swap_port(th);
                th->seq_num   = th->ack_num;
                th->tcp_flags = TCPF_RST;

                core::tcp.tx_push(msg, src);
            } else {
                rte::pktmbuf_free(msg);
            }
            printf("SLANKDEVSLANKDEV error: connection reset\n");
            move_state(TCPS_CLOSED);
            return;
        }
    }

    /*
     * 2: RST Check
     */
    if (HAVE(th, TCPF_RST)) {
        rte::pktmbuf_free(msg);
        return;
    }

    /*
     * 3: Securty and Priority Check
     * TODO not implement yet
     */

    /*
     * 4: SYN Check
     */
    assert(!HAVE(th, TCPF_ACK) && !HAVE(th, TCPF_RST));

    if (HAVE(th, TCPF_SYN)) {
        si.rcv_nxt_H(rte::bswap32(th->seq_num) + 1);
        si.irs_N(th->seq_num);

        if (HAVE(th, TCPF_ACK)) {
            si.snd_una_N(th->ack_num);
        }

        if (si.snd_una_H() > si.iss_H()) {
            move_state(TCPS_ESTABLISHED);
            swap_port(th);
            th->seq_num = si.snd_nxt_N();
            th->ack_num = si.rcv_nxt_N();
            th->tcp_flags = TCPF_ACK;
        } else {
            move_state(TCPS_SYN_RCVD);
            swap_port(th);
            th->seq_num = si.iss_N();
            th->ack_num = si.rcv_nxt_N();
            th->tcp_flags = TCPF_SYN|TCPF_ACK;
        }
        core::tcp.tx_push(msg, src);
    }

    /*
     * 5: (!SYN && !RST) Pattern
     */
    if (!HAVE(th, TCPF_SYN) && !HAVE(th, TCPF_RST)) {
        rte::pktmbuf_free(msg);
        return;
    }

    throw exception("OKASHII");
}



/*
 * rx_push_XXXX()
 * - TCPS_SYN_RCVD:
 * - TCPS_ESTABLISHED:
 * - TCPS_FIN_WAIT_1:
 * - TCPS_FIN_WAIT_2:
 * - TCPS_CLOSE_WAIT:
 * - TCPS_CLOSING:
 * - TCPS_LAST_ACK:
 * - TCPS_TIME_WAIT:
 */
void stcp_tcp_sock::rx_push_ELSESTATE(mbuf* msg, stcp_sockaddr_in* src,
        stcp_ip_header* ih, stcp_tcp_header* th)
{
    /*
     * 1: Sequence Number Check
     */
    switch (state) {

        case TCPS_SYN_RCVD:
        case TCPS_ESTABLISHED:
        case TCPS_FIN_WAIT_1:
        case TCPS_FIN_WAIT_2:
        case TCPS_CLOSE_WAIT:
        case TCPS_CLOSING:
        case TCPS_LAST_ACK:
        case TCPS_TIME_WAIT:
        {
            bool pass = false;
            if (data_length(th, ih) == 0) {
                if (th->rx_win == 0) {
                    if (th->seq_num == si.rcv_nxt_N()) {
                        pass = true;
                    }
                } else { /* win > 0 */
                    if (si.rcv_nxt_H() <= rte::bswap32(th->seq_num)
                        && rte::bswap32(th->seq_num) <= si.rcv_nxt_H()+si.rcv_win_H()) {
                        pass = true;
                    }
                }
            } else { /* data_length > 0 */
                if (th->rx_win > 0) {
                    bool cond1 = si.rcv_nxt_H() <= rte::bswap32(th->seq_num) &&
                        rte::bswap32(th->seq_num) < si.rcv_nxt_H() + si.rcv_win_H();
                    bool cond2 = si.rcv_nxt_H() <=
                        rte::bswap32(th->seq_num)+data_length(th, ih)-1 &&
                        rte::bswap32(th->seq_num)+data_length(th, ih)-1 <
                            si.rcv_nxt_H() + si.rcv_win_H();
                    if (cond1 || cond2) {
                        pass = true;
                    }
                }
            }

            if (!pass) {
                rte::pktmbuf_free(msg);
                return;
            }

            if (HAVE(th, TCPF_RST)) {
                rte::pktmbuf_free(msg);
                return;
            }

            ih->src = addr.sin_addr;
            ih->dst = src->sin_addr;
            ih->next_proto_id = STCP_IPPROTO_TCP;
            ih->total_length = rte::bswap16(rte::pktmbuf_pkt_len(msg));

            swap_port(th);
            th->seq_num = si.snd_nxt_N();
            th->ack_num = si.rcv_nxt_N();
            th->tcp_flags = TCPF_ACK;
            th->rx_win = si.snd_win_N();
            th->cksum = 0x0000;
            th->tcp_urp = 0x0000; // TODO hardcode

            th->cksum = rte_ipv4_udptcp_cksum(
                    reinterpret_cast<ipv4_hdr*>(ih), th);

            mbuf* nmsg = rte::pktmbuf_clone(msg, core::dpdk.get_mempool());
            core::tcp.tx_push(nmsg, src);
            break;
        }

        case TCPS_CLOSED:
        case TCPS_LISTEN:
        case TCPS_SYN_SENT:
            throw exception("OKASHII");
        default:
            throw exception("OKASHII: unknown state");
    }

    /*
     * 2: TCPF_RST Check
     */
    switch (state) {
        case TCPS_SYN_RCVD:
        {
            if (HAVE(th, TCPF_RST)) {
                printf("SLANKDEVSLANKDEV conection reset\n");
                rte::pktmbuf_free(msg);
                move_state(TCPS_CLOSED);
                return;
            }
            break;
        }

        case TCPS_ESTABLISHED:
        case TCPS_FIN_WAIT_1:
        case TCPS_FIN_WAIT_2:
        case TCPS_CLOSE_WAIT:
        {
            if (HAVE(th, TCPF_RST)) {
                printf("SLANKDEVSLANKDEV conection reset\n");
                rte::pktmbuf_free(msg);
                move_state(TCPS_CLOSED);
                return;
            }
            break;
        }

        case TCPS_CLOSING:
        case TCPS_LAST_ACK:
        case TCPS_TIME_WAIT:
        {
            if (HAVE(th, TCPF_RST)) {
                rte::pktmbuf_free(msg);
                move_state(TCPS_CLOSED);
                return;
            }
            break;
        }

        case TCPS_CLOSED:
        case TCPS_LISTEN:
        case TCPS_SYN_SENT:
            throw exception("OKASHII");
        default:
            throw exception("OKASHII: unknown state");
    }

    /*
     * 3: Securty and Priority Check
     * TODO: not implement yet
     */

    /*
     * 4: TCPF_SYN Check
     */
    switch (state) {
        case TCPS_SYN_RCVD:
        case TCPS_ESTABLISHED:
        case TCPS_FIN_WAIT_1:
        case TCPS_FIN_WAIT_2:
        case TCPS_CLOSE_WAIT:
        case TCPS_CLOSING:
        case TCPS_LAST_ACK:
        case TCPS_TIME_WAIT:
        {
            if (HAVE(th, TCPF_SYN)) {
                printf("SLANKDEVSLANKDEV conection reset\n");
                rte::pktmbuf_free(msg);
                move_state(TCPS_CLOSED);
                return;
            }
            break;
        }

        case TCPS_CLOSED:
        case TCPS_LISTEN:
        case TCPS_SYN_SENT:
            throw exception("OKASHII");
        default:
            throw exception("OKASHII: unknown state");
    }

    /*
     * 5: TCPF_ACK Check
     */
    if (HAVE(th, TCPF_ACK)) {
        switch (state) {
            case TCPS_SYN_RCVD:
            {
                DEBUG("AADD: snd_una: %u 0x%04x\n", si.snd_una_H(), si.snd_una_H());
                DEBUG("AADD: acknum : %u 0x%04x\n", rte::bswap32(th->ack_num),
                                                    rte::bswap32(th->ack_num));
                DEBUG("AADD: snd_nxt: %u 0x%04x\n", si.snd_nxt_H(), si.snd_nxt_H());
                if (si.snd_una_H() <= rte::bswap32(th->ack_num) &&
                        rte::bswap32(th->ack_num) <= si.snd_nxt_H()) {
                    DEBUG("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\n");
                    move_state(TCPS_ESTABLISHED);
                } else {
                    DEBUG("asdfasdfasdfasdfasdfsaadfsdfsdfasdfasdfasd\n");
                    swap_port(th);
                    th->seq_num = th->ack_num;
                    th->tcp_flags = TCPF_RST;

                    core::tcp.tx_push(msg, src);
                    return;
                }
                break;
            }

            case TCPS_ESTABLISHED:
            case TCPS_CLOSE_WAIT:
            case TCPS_CLOSING:
            {
                if (si.snd_una_H() < th->ack_num && th->ack_num <= si.snd_nxt_H()) {
                    si.snd_una_N(th->ack_num);
                }

                if (rte::bswap32(th->ack_num) < si.snd_una_H()) {
                    swap_port(th);
                    th->seq_num = th->ack_num;
                    th->tcp_flags = TCPF_RST;

                    core::tcp.tx_push(msg, src);
                    return;
                }

                if (si.snd_una_H() < rte::bswap32(th->ack_num) &&
                        rte::bswap32(th->ack_num) <= si.snd_nxt_H()) {
                    if ((si.snd_wl1_H() < rte::bswap32(th->seq_num))
                            || (si.snd_wl1_N() == th->seq_num)
                            || (si.snd_wl2_N() == th->ack_num)) {
                        si.snd_win_N(th->rx_win );
                        si.snd_wl1_N(th->seq_num);
                        si.snd_wl2_N(th->ack_num);
                    }
                }

                if (state == TCPS_CLOSING) {
                    if (si.snd_nxt_H() <= rte::bswap32(th->ack_num)) {
                        move_state(TCPS_TIME_WAIT);
                    }
                    rte::pktmbuf_free(msg);
                }
                break;
            }

            case TCPS_FIN_WAIT_1:
            {
                move_state(TCPS_FIN_WAIT_2);
                break;
            }
            case TCPS_FIN_WAIT_2:
            {
                printf("OK\n");
                break;
            }
            case TCPS_LAST_ACK:
            {
                if (si.snd_nxt_H() <= rte::bswap32(th->ack_num)) {
                    move_state(TCPS_CLOSED);
                    return;
                }
                break;
            }
            case TCPS_TIME_WAIT:
            {
                throw exception("TODO: NOT IMPEL YET");
                break;
            }

            case TCPS_CLOSED:
            case TCPS_LISTEN:
            case TCPS_SYN_SENT:
                throw exception("OKASHII");
            default:
                throw exception("OKASHII: unknown state");
        }

    } else {
        rte::pktmbuf_free(msg);
        return;
    }

    /*
     * 6: URG Check
     * TODO: not implement yet
     */
    /*
     * 4: Text Segment Control
     */
    switch (state) {
        case TCPS_ESTABLISHED:
        case TCPS_FIN_WAIT_1:
        case TCPS_FIN_WAIT_2:
        {
            si.rcv_nxt_inc_H(data_length(th, ih));

            swap_port(th);
            th->seq_num = si.snd_nxt_N();
            th->ack_num = si.rcv_nxt_N();
            th->tcp_flags = TCPF_ACK;

            core::tcp.tx_push(msg, src);
            return;
            break;
        }

        case TCPS_CLOSE_WAIT:
        case TCPS_CLOSING:
        case TCPS_LAST_ACK:
        case TCPS_TIME_WAIT:

        case TCPS_CLOSED:
        case TCPS_LISTEN:
        case TCPS_SYN_SENT:
            throw exception("OKASHII");

        case TCPS_SYN_RCVD:
            throw exception("RFC MITEIGIIIII");

        default:
            throw exception("OKASHII: unknown state");
    }

    /*
     * 6: TCPF_FIN Check
     */
    if (HAVE(th, TCPF_FIN)) {
        printf("SLANKDEVSLANKDEV connection closing\n");
        switch (state) {
            case TCPS_CLOSED:
            case TCPS_LISTEN:
            case TCPS_SYN_SENT:
                rte::pktmbuf_free(msg);
                return;
                break;
            case TCPS_SYN_RCVD:
            case TCPS_ESTABLISHED:
                move_state(TCPS_CLOSE_WAIT);
                break;
            case TCPS_FIN_WAIT_1:
                move_state(TCPS_CLOSING);
                break;
            case TCPS_FIN_WAIT_2:
                move_state(TCPS_TIME_WAIT);
                break;
            case TCPS_CLOSE_WAIT:
                break;
            case TCPS_CLOSING:
                break;
            case TCPS_LAST_ACK:
                break;
            case TCPS_TIME_WAIT:
                break;

            default:
                throw exception("OKASHII: unknown state");
        }
        return;
    }
}



void stcp_tcp_sock::print_stat() const
{
    stat& s = stat::instance();
    s.write("\t%u/tcp state=%s[this=%p] rx/tx=%zd/%zd %u/%u %u",
            rte::bswap16(port),
            tcpstate2str(state), this,
            rxq.size(), txq.size(),
            si.snd_nxt_H(), si.rcv_nxt_H(),
            rte::bswap16(pair_port));

#if 0
    switch (state) {
#if 0
        case TCPS_LISTEN:
            s.write("\t - socket alloced %zd/%zd", num_connected, max_connect);
            s.write("\n\n\n");
            break;
        case TCPS_CLOSED:
            s.write("\n\n\n");
            break;
        default:
            s.write("\n\n\n");
            break;
#endif
        case TCPS_LISTEN:
            s.write("\t - socket alloced %zd/%zd", num_connected, max_connect);
            break;
        case TCPS_ESTABLISHED:
            s.write("\t - iss    : %u", iss    );
            // s.write("\t - snd_una: %u", snd_una);
            s.write("\t - snd_nxt: %u", snd_nxt);
            // s.write("\t - snd_win: %u", snd_win);
            // s.write("\t - snd_up : %u", snd_up );
            // s.write("\t - snd_wl1: %u", snd_wl1);
            // s.write("\t - snd_wl2: %u", snd_wl2);
            s.write("\t - irs    : %u", irs    );
            s.write("\t - rcv_nxt: %u", rcv_nxt);
            // s.write("\t - rcv_wnd: %u", rcv_wnd);
            // s.write("\t - rcv_up : %u", rcv_up );
            s.write("\t - port/pair_port: %u/%u",
                    rte::bswap16(port),
                    rte::bswap16(pair_port));
            s.write("\t - addr: %s", addr.c_str());
            s.write("\t - pair: %s", pair.c_str());
            break;
        default:
            break;
    }
#endif
}


void tcp_module::print_stat() const
{
    stat& s = stat::instance();
    s.write("TCP module");
    s.write("\tRX Packets %zd", rx_cnt);
    s.write("\tTX Packets %zd", tx_cnt);

    if (!socks.empty()) {
        s.write("");
        s.write("\tNetStat %zd ports", socks.size());
    }
    for (size_t i=0; i<socks.size(); i++) {
        socks[i]->print_stat();
    }
}



void tcp_module::rx_push(mbuf* msg, stcp_sockaddr_in* src)
{
    stcp_tcp_header* th
        = rte::pktmbuf_mtod<stcp_tcp_header*>(msg);
    rx_cnt++;

    bool find_socket = false;
    uint16_t dst_port = th->dport;
    for (stcp_tcp_sock* sock : socks) {
        if (sock->port == dst_port) {
            mbuf* m = rte::pktmbuf_clone(msg, core::dpdk.get_mempool());
            mbuf_push(m, sizeof(stcp_ip_header));
            sock->rx_push(m, src);
            find_socket = true;
        }
    }

    if (!find_socket) {
        /*
         * Send Port Unreachable as TCP-RSTACK
         */
        send_RSTACK(msg, src);
    }
    rte::pktmbuf_free(msg);
}


void tcp_module::send_RSTACK(mbuf* msg, stcp_sockaddr_in* dst)
{
    stcp_ip_header* ih = reinterpret_cast<stcp_ip_header*>(
            mbuf_push(msg, sizeof(stcp_ip_header)));
    stcp_tcp_header* th
        = rte::pktmbuf_mtod_offset<stcp_tcp_header*>(msg, sizeof(stcp_tcp_header));

    /*
     * Delete TCP Option field
     */
    size_t optionlen =
        rte::pktmbuf_pkt_len(msg) - sizeof(stcp_ip_header) - sizeof(stcp_tcp_header);
    rte::pktmbuf_trim(msg, optionlen);


    /*
     * Set IP header for TCP checksum
     */
    ih->src           = ih->dst;
    ih->dst           = dst->sin_addr;
    ih->next_proto_id = STCP_IPPROTO_TCP;
    ih->total_length  = rte::bswap16(rte::pktmbuf_pkt_len(msg));

    /*
     * Set TCP header
     *
     */
    swap_port(th);
    th->ack_num  = th->seq_num + rte::bswap32(1);
    th->seq_num  = 0;

    th->data_off = sizeof(stcp_tcp_header)/4 << 4;
    th->tcp_flags    = TCPF_RST|TCPF_ACK;
    th->rx_win   = 0;
    th->cksum    = 0x0000;
    th->tcp_urp  = 0x0000;

    th->cksum = rte_ipv4_udptcp_cksum(
            reinterpret_cast<ipv4_hdr*>(ih), th);

    core::tcp.tx_push(msg, dst);
}






} /* namespace slank */
