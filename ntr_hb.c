#include "ntr_hb.h"
#include "main.h"
#include "ntr_common.h"
#include "mem_proxy.h"
#ifndef _WIN32
#include <netinet/tcp.h>
#endif

#include <stdlib.h>

#define HEART_BEAT_EVERY_MS 250
// the 3DS answers every heartbeat; this long without one means it's gone
#define HEART_BEAT_TIMEOUT_MS 5000

// stream re-request when video stops but the control link is still up
#define RP_RESEND_FIRST_MS 5000
#define RP_RESEND_MAX_MS 60000

// auto-reconnect backoff after the control link drops mid-stream
#define RECONNECT_FIRST_MS 1000
#define RECONNECT_MAX_MS 15000

#define TCP_MAGIC 0x12345678
#define TCP_ARGS_COUNT 16

struct tcp_packet_hdr {
    uint32_t magic;
    uint32_t seq;
    uint32_t type;
    uint32_t cmd;
    uint32_t args[TCP_ARGS_COUNT];

    uint32_t data_len;
};

atomic_int menu_work_state, nwm_work_state, nwm_o3ds_work_state;
atomic_int menu_work_req_state, nwm_work_req_state, nwm_o3ds_work_req_state;
atomic_bool menu_remote_play;

static int socket_close(SOCKET sock)
{
    int status = 0;

    status = shutdown(sock, SD_BOTH);
    if (status != 0) {
        err_log("socket shutdown failed: %d\n", socket_errno());
    }
    status = closesocket(sock);

    return status;
}

static SOCKET tcp_connect(int port, uint32_t addr)
{
    struct sockaddr_in servaddr = {0};
    SOCKET sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (!socket_valid(sockfd))
    {
        err_log("socket creation failed: %d\n", socket_errno());
        return INVALID_SOCKET;
    }

    if (!socket_set_nonblock(sockfd, 1))
    {
        err_log("socket_set_nonblock failed: %d\n", socket_errno());
        closesocket(sockfd);
        return INVALID_SOCKET;
    }

    servaddr.sin_family = AF_INET;
    char ip_addr_buf[16];
    snprintf(
        ip_addr_buf, sizeof(ip_addr_buf),
        "%d.%d.%d.%d",
        (int)((uint8_t *)&addr)[0],
        (int)((uint8_t *)&addr)[1],
        (int)((uint8_t *)&addr)[2],
        (int)((uint8_t *)&addr)[3]);
    servaddr.sin_addr.s_addr = htonl(__builtin_bswap32(addr));
    servaddr.sin_port = htons(port);

    err_log("connecting to %s:%d ...\n", ip_addr_buf, port);
    int ret = connect(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr));
    if (ret != 0 && socket_errno() != WSAEWOULDBLOCK && socket_errno() != EINPROGRESS)
    {
        err_log("connection failed: %d\n", socket_errno());
        socket_close(sockfd);
        return INVALID_SOCKET;
    }

    // Windows reports a failed non-blocking connect (e.g. port closed) in the
    // except set, not the write set; without it a refusal looks like a timeout
    fd_set wset, eset;
    struct timeval tv;
    FD_ZERO(&wset);
    FD_SET(sockfd, &wset);
    FD_ZERO(&eset);
    FD_SET(sockfd, &eset);
    tv.tv_sec = 2;
    tv.tv_usec = 0;

    if (select(sockfd + 1, NULL, &wset, &eset, &tv) >= 1)
    {
        int so_error = 0;
        socklen_t len = sizeof(so_error);

        getsockopt(sockfd, SOL_SOCKET, SO_ERROR, (char *)&so_error, &len);

        if (so_error == 0 && FD_ISSET(sockfd, &wset))
        {
            err_log("connected\n");
            // small packets (heartbeats, memory commands) go out at once
            int one = 1;
            setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
#ifdef _WIN32
            // ACK every segment at once. A memory reply bigger than one segment otherwise
            // waits ~50 ms: the 3DS (Nagle) holds its last segment until the first is ACKed,
            // and Windows delays that ACK. SIO_TCP_SET_ACK_FREQUENCY = _WSAIOW(IOC_VENDOR, 23).
            {
                DWORD ack_freq = 1, bytes = 0;
                if (WSAIoctl(sockfd, _WSAIOW(IOC_VENDOR, 23), &ack_freq, sizeof(ack_freq), NULL, 0, &bytes, NULL, NULL) != 0)
                    err_log("ack frequency not set: %d\n", socket_errno());
            }
#endif
            return sockfd;
        }
        if (so_error == WSAECONNREFUSED)
            err_log("connection refused: the 3DS is on the network but nothing listens on port %d\n", port);
        else
            err_log("connection failed: %d\n", so_error);
        closesocket(sockfd);
        return INVALID_SOCKET;
    }

    closesocket(sockfd);
    err_log("connection timeout\n");
    return INVALID_SOCKET;
}

#ifdef _WIN32
#include <iphlpapi.h>
#include <icmpapi.h>

// Round-trip time in ms if the 3DS answers one ping within a second, else -1.
// A reconnect that times out can mean the 3DS dropped off Wi-Fi (no ping) or
// that it's still on Wi-Fi but NTR has stopped answering on port 8000 (ping
// OK); logging which one turns the next drop's log into an answer.
static int ping_3ds_ms(uint32_t addr)
{
    HANDLE icmp = IcmpCreateFile();
    if (icmp == INVALID_HANDLE_VALUE)
        return -1;
    char data[] = "ntr-vc";
    char reply[sizeof(ICMP_ECHO_REPLY) + sizeof(data) + 8 + 32];
    // same byte order as sin_addr.s_addr in tcp_connect
    DWORD n = IcmpSendEcho(icmp, (IPAddr)htonl(__builtin_bswap32(addr)), data, sizeof(data), NULL, reply, sizeof(reply), 1000);
    int ret = -1;
    if (n > 0) {
        PICMP_ECHO_REPLY r = (PICMP_ECHO_REPLY)reply;
        if (r->Status == IP_SUCCESS)
            ret = (int)r->RoundTripTime;
    }
    IcmpCloseHandle(icmp);
    return ret;
}
#endif

static int tcp_send(SOCKET sockfd, char *buf, int size)
{
    int ret, pos = 0;
    int tmpsize = size;

    while (program_running && tmpsize) {
        if ((ret = send(sockfd, &buf[pos], tmpsize, 0)) < 0) {
            if (socket_errno() == WSAEWOULDBLOCK) {
                if (socket_poll(sockfd, POLLOUT)) {
                    continue;
                } else {
                    if (program_running)
                        err_log("socket send wait failed: %d\n", socket_errno());
                    return -1;
                }
            }
            return ret;
        }
        pos += ret;
        tmpsize -= ret;
    }

    return size;
}

static int tcp_recv(SOCKET sockfd, char *buf, int size)
{
    int ret, pos = 0;
    int tmpsize = size;

    while (program_running && tmpsize) {
        if ((ret = recv(sockfd, &buf[pos], tmpsize, 0)) <= 0) {
            if (ret < 0) {
                if (socket_errno() == WSAEWOULDBLOCK) {
                    if (pos) {
                        if (socket_poll(sockfd, POLLIN)) {
                            continue;
                        } else {
                            if (program_running)
                                err_log("socket recv wait failed: %d\n", socket_errno());
                            return -1;
                        }
                    } else {
                        return 0;
                    }
                }
            }
            return ret;
        }
        pos += ret;
        tmpsize -= ret;
    }

    return size;
}

static int tcp_send_packet_header(SOCKET s, uint32_t seq, uint32_t type, uint32_t cmd, uint32_t *argv, int argc, uint32_t data_len)
{
    struct tcp_packet_hdr packet;
    packet.magic = TCP_MAGIC;
    packet.seq = seq;
    packet.type = type;
    packet.cmd = cmd;
    for (int i = 0; i < TCP_ARGS_COUNT; ++i) {
        if (i < argc) {
            packet.args[i] = argv[i];
        } else {
            packet.args[i] = 0;
        }
    }
    packet.data_len = data_len;

    char *buf = (char *)&packet;
    int size = sizeof(packet);
    return tcp_send(s, buf, size);
}

// Largest reply data accepted from the 3DS (memory replies are at most 0x4000)
#define TCP_MAX_DATA_LEN (4 * 1024 * 1024)

// Reads and handles one packet from the 3DS if one is waiting.
// 1 = handled one, 0 = nothing waiting, -1 = the link is broken.
static int tcp_handle_packet(SOCKET sockfd, bool mem_link, uint32_t *last_reply_us)
{
    struct tcp_packet_hdr header = {0};
    int ret = tcp_recv(sockfd, (char *)&header, sizeof(header));
    if (ret < 0 || !program_running)
    {
        if (program_running)
            err_log("tcp recv error: %d\n", socket_errno());
        return -1;
    }
    if (ret == 0)
        return 0;
    if (header.magic != TCP_MAGIC)
    {
        if (program_running)
            err_log("broken protocol\n");
        return -1;
    }
    *last_reply_us = iclock();

    if (header.data_len > TCP_MAX_DATA_LEN)
    {
        err_log("packet %d too large: %u\n", (int)header.cmd, (unsigned)header.data_len);
        return -1;
    }
    char *buf = NULL;
    if (header.data_len)
    {
        buf = malloc(header.data_len + 1);
        if (!buf)
        {
            err_log("tcp recv alloc failed: %d\n", (int)header.data_len);
            return -1;
        }
        if ((ret = tcp_recv(sockfd, buf, header.data_len)) < 0)
        {
            if (program_running)
                err_log("tcp recv error: %d\n", socket_errno());
            free(buf);
            return -1;
        }
        if (ret == 0)
        {
            // data not there yet: wait for it (tcp_recv only waits once it has started)
            if (!socket_poll(sockfd, POLLIN) || (ret = tcp_recv(sockfd, buf, header.data_len)) <= 0)
            {
                if (program_running)
                    err_log("tcp recv data error: %d\n", socket_errno());
                free(buf);
                return -1;
            }
        }
        buf[header.data_len] = 0;
    }

    if (header.cmd == 0)
    {
        // heartbeat reply: the 3DS's debug log
        if (buf)
            fprintf(stderr, "%s", buf);
    }
    else if (mem_link && (header.cmd == MEM_PROXY_CMD_LISTPROCESS || header.cmd == MEM_PROXY_CMD_READMEM || header.cmd == MEM_PROXY_CMD_WRITEMEM))
    {
        // a reply that matches nothing belongs to a request that already timed out
        mem_proxy_reply(header.seq, header.cmd, header.args, (const uint8_t *)buf, header.data_len);
    }
    else if (header.data_len)
    {
        err_log("unhandled packet type %d: size %d\n", header.cmd, header.data_len);
    }
    free(buf);
    return 1;
}

static bool rp_send_need_update;
static uint32_t rp_send_last_us;
static struct ntr_rp_config_t rp_config_last;
static int rp_port_last;
static int rp_quality_sent_last = -1;
static int rp_qos_sent_last = -1;

#include "ui_main_nk.h"

nk_bool ntr_auto_reconnect = nk_true;
nk_bool ntr_auto_update_params = nk_true;
nk_bool ntr_auto_quality = nk_true;
atomic_int ntr_jpeg_quality_auto = 0;
atomic_int ntr_qos_auto = 0;

static bool ntr_rp_config_valid(struct ntr_rp_config_t *config)
{
    return config->jpeg_quality && config->bandwidth_limit;
}

thread_ret_t tcp_thread_func(void *arg)
{
    struct tcp_thread_arg *t = (struct tcp_thread_arg *)arg;

#define RESET_SOCKET() do { \
    socket_close(sockfd); \
    sockfd = INVALID_SOCKET; \
    if (mem_link) \
        mem_proxy_link(false); \
    *(t->work_state) = CONNECTION_STATE_DISCONNECTED; \
    *(t->work_req_state) = CONNECTION_REQ_STATE_NONE; \
    if (t->remote_play) { \
        *(t->remote_play) = 0; \
        __atomic_store_n((uint32_t *)ntr_ip_octet_incoming, 0, __ATOMIC_RELAXED); \
    } \
    err_log("disconnected\n"); \
    if (t->remote_play && rp_sent && ntr_auto_reconnect) { \
        /* the link dropped under a running stream: retry it ourselves, */ \
        /* the 3DS may not stream again until it hears from us */ \
        retry_pending = true; \
        retry_delay_ms = RECONNECT_FIRST_MS; \
        retry_at_us = iclock() + RECONNECT_FIRST_MS * 1000; \
    } \
    rp_sent = false; \
} while (0)

    SOCKET sockfd = INVALID_SOCKET;
    int packet_seq = 0;
    bool rp_sent = false;          // remote play requested on this connection
    bool auto_connect_ok = true;   // false after the user disconnects
    bool retry_pending = false;
    int retry_delay_ms = 0;
    uint32_t retry_at_us = 0;
    int ping_state = 0;            // 1 answers ping, -1 doesn't, 0 not checked yet
    uint32_t last_reply_us = 0;    // last heartbeat reply from the 3DS
    int resend_delay_ms = RP_RESEND_FIRST_MS;
    uint32_t resend_at_us = 0;     // last time the stream was re-requested
    uint32_t rp_sent_us = 0;       // last time 901 was actually sent
    uint32_t next_hb_us = 0;       // when the next heartbeat goes out
    // the menu link (port 8000) also carries memory commands for mem_proxy
    const bool mem_link = t->remote_play != NULL;
    while (program_running)
    {
        uint32_t ip_octet_incoming;
        uint32_t ip_octet;

        ip_octet_incoming = t->remote_play && ntr_auto_reconnect && auto_connect_ok
            ? __atomic_load_n((uint32_t *)ntr_ip_octet_incoming, __ATOMIC_RELAXED) : 0;
        ip_octet = __atomic_load_n((uint32_t *)ntr_ip_octet, __ATOMIC_RELAXED);

        const bool user_connect = *(t->work_req_state) == CONNECTION_REQ_STATE_CONNECTING;
        const bool retry_due = retry_pending && ntr_auto_reconnect && ip_octet &&
            (int32_t)(iclock() - retry_at_us) >= 0;

        if (
            *(t->work_state) == CONNECTION_STATE_DISCONNECTED &&
            (
                user_connect ||
                (ip_octet_incoming && ip_octet_incoming == ip_octet) ||
                retry_due
            )
        )
        {
            *(t->work_req_state) = CONNECTION_REQ_STATE_NONE;
            if (user_connect) {
                auto_connect_ok = true;
                retry_pending = false;
            }
            sockfd = tcp_connect(t->port, ip_octet);
            if (!socket_valid(sockfd))
            {
                if (t->remote_play)
                {
                    *(t->remote_play) = 0;
                }
                if (retry_pending) {
#ifdef _WIN32
                    // logged once per change, not on every attempt
                    int ms = ping_3ds_ms(ip_octet);
                    int state = ms >= 0 ? 1 : -1;
                    if (state != ping_state) {
                        if (state > 0)
                            err_log("3DS answers ping (%d ms): it's on Wi-Fi, but NTR isn't answering on port %d\n", ms, t->port);
                        else
                            err_log("3DS doesn't answer ping: it's off the network (Wi-Fi dropped, asleep or crashed)\n");
                        ping_state = state;
                    }
#endif
                    retry_delay_ms = MIN(retry_delay_ms * 2, RECONNECT_MAX_MS);
                    retry_at_us = iclock() + (uint32_t)retry_delay_ms * 1000;
                }
                continue;
            }

            if (retry_pending && t->remote_play) {
                // ask for the stream again; same params are a no-op if it's still running
                *(t->remote_play) = 1;
            }
            if (ping_state)
                err_log("reconnected to the 3DS\n");
            ping_state = 0;
            retry_pending = false;
            packet_seq = 0;
            last_reply_us = iclock();
            next_hb_us = iclock() + HEART_BEAT_EVERY_MS * 1000;
            resend_delay_ms = RP_RESEND_FIRST_MS;
            resend_at_us = 0;
            *(t->work_state) = CONNECTION_STATE_CONNECTED;
            if (mem_link)
                mem_proxy_link(true);
        }
        else if (*(t->work_state) == CONNECTION_STATE_CONNECTED)
        {
            if (*(t->work_req_state) == CONNECTION_REQ_STATE_DISCONNECTING) {
                RESET_SOCKET();
                retry_pending = false;
                auto_connect_ok = false;
                continue;
            }

            // Wait for the next heartbeat. Memory commands (mem_proxy) cut the
            // wait short: new ones go out at once, and while replies are
            // outstanding the socket is watched every few ms.
            {
                int32_t left_us = (int32_t)(next_hb_us - iclock());
                if (left_us > 0) {
                    int left_ms = (int)((left_us + 999) / 1000);
                    if (mem_link && mem_proxy_has_work())
                        socket_poll_ms(sockfd, MIN(left_ms, 2));
                    else if (mem_link)
                        mem_proxy_wait(left_ms);
                    else
                        Sleep(left_ms);
                }
            }

            // everything the 3DS has sent so far (heartbeat replies, memory replies)
            int ret = 0;
            for (int n = 0; n < 64; ++n) {
                ret = tcp_handle_packet(sockfd, mem_link, &last_reply_us);
                if (ret <= 0)
                    break;
            }
            if (ret < 0) {
                RESET_SOCKET();
                continue;
            }

            if (mem_link) {
                uint32_t mcmd, margs[TCP_ARGS_COUNT];
                const uint8_t *mdata;
                uint32_t mlen;
                int token;
                bool send_failed = false;
                while ((token = mem_proxy_next(&mcmd, margs, &mdata, &mlen)) >= 0) {
                    uint32_t seq = (uint32_t)packet_seq++;
                    ret = tcp_send_packet_header(sockfd, seq, 0, mcmd, margs, TCP_ARGS_COUNT, mlen);
                    if (ret >= 0 && mlen)
                        ret = tcp_send(sockfd, (char *)mdata, (int)mlen);
                    mem_proxy_sent(token, seq);
                    if (ret < 0) {
                        if (program_running)
                            err_log("memory command send failed: %d\n", socket_errno());
                        send_failed = true;
                        break;
                    }
                }
                if (send_failed) {
                    RESET_SOCKET();
                    continue;
                }
            }

            if ((int32_t)(iclock() - next_hb_us) < 0)
                continue;
            next_hb_us = iclock() + HEART_BEAT_EVERY_MS * 1000;

            // A 3DS that drops off Wi-Fi without closing the connection never
            // produces a recv error, so missing replies are the only sign.
            if (t->remote_play && (int32_t)(iclock() - last_reply_us) > HEART_BEAT_TIMEOUT_MS * 1000)
            {
                err_log("3DS stopped answering heartbeats for %d ms\n", HEART_BEAT_TIMEOUT_MS);
                RESET_SOCKET();
                continue;
            }

            ret = tcp_send_packet_header(sockfd, packet_seq, 0, 0, 0, 0, 0);
            if (ret < 0)
            {
                if (program_running)
                    err_log("heart beat send failed: %d\n", socket_errno());
                RESET_SOCKET();
                continue;
            }
            ++packet_seq;

            // The control link can stay up while the stream itself dies
            // (NWM restarted or hung). Ask for it again after a silence, with
            // backoff so a 3DS that can't stream isn't nagged every 5 s.
            if (t->remote_play && rp_sent && ntr_auto_reconnect)
            {
                const uint32_t now_us = iclock();
                const uint32_t last_pkt_us = __atomic_load_n(&rp_last_packet_us, __ATOMIC_RELAXED);
                // quiet since the later of: last video packet, last stream request
                uint32_t since_us = now_us - last_pkt_us;
                if ((int32_t)(last_pkt_us - rp_sent_us) < 0)
                    since_us = now_us - rp_sent_us;
                if (resend_at_us && (int32_t)(last_pkt_us - resend_at_us) > 0)
                {
                    // video came back after a re-request
                    resend_delay_ms = RP_RESEND_FIRST_MS;
                    resend_at_us = 0;
                }
                if ((int32_t)since_us > resend_delay_ms * 1000)
                {
                    err_log("no video for %d s, asking the 3DS for the stream again\n", resend_delay_ms / 1000);
                    *(t->remote_play) = 1;
                    resend_at_us = now_us;
                    resend_delay_ms = MIN(resend_delay_ms * 2, RP_RESEND_MAX_MS);
                }
            }

            const uint32_t rp_send_next_us = iclock();

            const bool auto_quality = ntr_auto_quality;
            const int jpeg_quality_auto = ntr_auto_quality_cond ? ntr_jpeg_quality_auto : 0;
            const int qos_auto = ntr_auto_qos_cond ? ntr_qos_auto : 0;

            // slider is the ceiling; auto controller can only lower it
            const int rp_quality_auto = jpeg_quality_auto ? jpeg_quality_auto : NTR_JPEG_QUALITY_MAX;
            const int rp_quality_send = auto_quality
                ? MIN(ntr_rp_config.jpeg_quality, rp_quality_auto)
                : ntr_rp_config.jpeg_quality;

            const int rp_qos = ntr_rp_config.bandwidth_limit * 128 * 1024;
            const int rp_qos_auto = qos_auto ? (int)((uint64_t)qos_auto * 128 * 1024 / 1000) : rp_qos;
            const int rp_qos_send = auto_quality ? MIN(rp_qos, rp_qos_auto) : rp_qos;

            const bool rp_send_update =
                memcmp(&rp_config_last, &ntr_rp_config, sizeof(struct ntr_rp_config_t)) ||
                rp_port_last != ntr_rp_port_bound ||
                rp_quality_send != rp_quality_sent_last ||
                rp_qos_send != rp_qos_sent_last;
            rp_send_need_update = rp_send_need_update || rp_send_update;
            const bool rp_send_wait_timeout = (int32_t)(rp_send_next_us - rp_send_last_us) > 1000000;
            if (t->remote_play && (*(t->remote_play) || rp_send_update || rp_send_wait_timeout))
            {
                rp_send_last_us = rp_send_next_us;
                memcpy(&rp_config_last, &ntr_rp_config, sizeof(struct ntr_rp_config_t));
                rp_port_last = ntr_rp_port_bound;
                rp_quality_sent_last = rp_quality_send;
                rp_qos_sent_last = rp_qos_send;
                if (!rp_port_last || !ntr_rp_config_valid(&rp_config_last))
                    continue;
                if (!*(t->remote_play) && (!rp_send_wait_timeout || !rp_send_need_update || !ntr_auto_update_params))
                    continue;
                rp_send_need_update = false;

                *(t->remote_play) = 0;

                int kcp_mode = rp_config_last.kcp_mode % KCP_MODE_COUNT;
                int lossless_mode = rp_config_last.kcp_mode / KCP_MODE_COUNT;
                uint32_t args[] = {
                    ((uint32_t)rp_config_last.top_screen_priority << 8) | (uint32_t)rp_config_last.screen_priority_factor,
                    (uint32_t)rp_quality_send,
                    (uint32_t)rp_qos_send,
                    1404036572 /* guarding magic */,
                    (uint32_t)(uint16_t)rp_port_last |
                        (kcp_mode != KCP_MODE_NONE ? (uint32_t)(1 << 30) : (uint32_t)0) |
                        (kcp_mode == KCP_MODE_ON_DELTA ? (uint32_t)(1 << 31) : (uint32_t)0) |
                        (lossless_mode ?
                            (uint32_t)(1 << 29) |
                                (uint32_t)(((NTR_COLOR_BIAS_MAX - rp_config_last.lossless_color) & ((1 << 2) - 1)) << 27) :
                            (uint32_t)0),
                    (uint32_t)(rp_config_last.audio_enable ? 2 : 0), // 2: in-band kcp audio if available; 1: audio enable
                    (uint32_t)(rp_config_last.full_width ? 2 : 0) // 2: both; 1: right; 0: left
                };

                ret = tcp_send_packet_header(
                    sockfd, packet_seq, 0, 901,
                    args, sizeof(args) / sizeof(*args), 0);

                if (ret < 0)
                {
                    if (program_running)
                        err_log("remote play send failed: %d\n", socket_errno());
                    RESET_SOCKET();
                    continue;
                }
                ++packet_seq;
                rp_sent = true;
                rp_sent_us = iclock();

                rp_lock_wait(ui_nk_lock);
                switch (ui_view_mode) {
                    case VIEW_MODE_TOP:
                    case VIEW_MODE_BOT:
                        if (rp_config_last.screen_priority_factor) {
                            ui_view_mode = VIEW_MODE_TOP_BOT;
                        }
                        // fall-through
                    case VIEW_MODE_TOP_BOT:
                    case VIEW_MODE_SEPARATE:
                        if (!rp_config_last.screen_priority_factor) {
                            ui_view_mode = rp_config_last.top_screen_priority ? VIEW_MODE_TOP : VIEW_MODE_BOT;
                        }
                        break;
                }
                rp_lock_rel(ui_nk_lock);
            }
        }
        else
        {
            Sleep(REST_EVERY_MS);
        }
    }
    return 0;

#undef RESET_SOCKET
}
