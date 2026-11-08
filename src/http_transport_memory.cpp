#ifdef ARDUINO_ARCH_ESP32
#include "http_transport_memory.h"
#include <lwip/sockets.h>
#include <lwip/tcpip.h>
#include <lwip/priv/tcp_priv.h>

namespace {
template<class Action> bool withSocketPcb(int socket, Action action) {
    sockaddr_in local{},peer{};
    socklen_t localSize=sizeof(local),peerSize=sizeof(peer);
    // Socket APIs run before taking the core lock; they may post to tcpip.
    if(socket<0 || getsockname(socket,reinterpret_cast<sockaddr*>(&local),&localSize)!=0 ||
       getpeername(socket,reinterpret_cast<sockaddr*>(&peer),&peerSize)!=0 ||
       local.sin_family!=AF_INET || peer.sin_family!=AF_INET) return false;
    bool found=false;
    LOCK_TCPIP_CORE();
    for(auto* pcb=tcp_active_pcbs;pcb;pcb=pcb->next) {
        if(IP_IS_V4(&pcb->local_ip) && IP_IS_V4(&pcb->remote_ip) &&
           pcb->local_port==ntohs(local.sin_port) && pcb->remote_port==ntohs(peer.sin_port) &&
           ip4_addr_get_u32(ip_2_ip4(&pcb->local_ip))==local.sin_addr.s_addr &&
           ip4_addr_get_u32(ip_2_ip4(&pcb->remote_ip))==peer.sin_addr.s_addr) {
            action(pcb);found=true;
            break;
        }
    }
    UNLOCK_TCPIP_CORE();
    return found;
}
}

void configureHttpTransport(int socket) {
#ifdef BOARD_ESP32_3248S035C
    withSocketPcb(socket, [](tcp_pcb* pcb) {
        // The pinned SDK has no TCP_MAXSEG socket option. Apply this only
        // before the first response, under the same lwIP lock as queue reads.
        // Short segments kept HTTP/WS progressing in the measured Sunton
        // network where 1436-byte MSS repeatedly lost complete response bodies.
        // Preserve smaller peer limits and all send-credit/window bookkeeping.
        if (pcb->mss > 536 && pcb->snd_buf == TCP_SND_BUF && !pcb->snd_queuelen)
            pcb->mss = 536;
    });
#else
    (void)socket;
#endif
}

// The pinned Arduino SDK has LWIP_SO_LINGER=0, so close() alone can
// retain an abandoned send queue through minutes of TCP retransmissions.
void abortHttpTransport(int socket) {
    withSocketPcb(socket,[](tcp_pcb* pcb){tcp_abort(pcb);});
}

size_t httpSendAllowance(int socket) {
#ifdef BOARD_ESP32_3248S035C
    size_t allowance=0;
    withSocketPcb(socket,[&](tcp_pcb* pcb){
        // Observe ACK progress without changing lwIP's send credit or its
        // select()/writable-event thresholds. Pace the application instead.
        constexpr size_t limit=3072;
        const size_t queued=TCP_SND_BUF-pcb->snd_buf;
        if(queued<limit) allowance=limit-queued;
    });
    return allowance;
#else
    return size_t(-1);
#endif
}
#endif
