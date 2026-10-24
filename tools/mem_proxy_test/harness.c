/* Test harness: the real mem_proxy.c plus a copy of ntr_hb.c's control-loop
   structure (wait / drain / send memory commands / heartbeat every 250 ms),
   connected to a fake 3DS (fake_3ds.py). Windows only.
   Build: gcc -O2 -DMEM_PROXY_STANDALONE -I../.. harness.c ../../mem_proxy.c -lws2_32 -o harness.exe
   Run:   harness.exe <port> [seconds] */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "mem_proxy.h"

#define TCP_MAGIC 0x12345678
#define HB_MS 250

struct hdr { uint32_t magic, seq, type, cmd, args[16], data_len; };

static uint64_t now_us(void) {
    static LARGE_INTEGER f; LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (uint64_t)(t.QuadPart * 1000000.0 / f.QuadPart);
}

static int recv_all(SOCKET s, char *b, int n, int first_nonblock) {
    int pos = 0;
    while (pos < n) {
        int r = recv(s, b + pos, n - pos, 0);
        if (r > 0) { pos += r; continue; }
        if (r == 0) return -1;
        if (WSAGetLastError() == WSAEWOULDBLOCK) {
            if (pos == 0 && first_nonblock) return 0;
            WSAPOLLFD p = { s, POLLIN, 0 };
            if (WSAPoll(&p, 1, 3000) <= 0) return -1;
            continue;
        }
        return -1;
    }
    return n;
}

static int send_all(SOCKET s, const char *b, int n) {
    int pos = 0;
    while (pos < n) {
        int r = send(s, b + pos, n - pos, 0);
        if (r > 0) { pos += r; continue; }
        if (r < 0 && WSAGetLastError() == WSAEWOULDBLOCK) {
            WSAPOLLFD p = { s, POLLOUT, 0 };
            if (WSAPoll(&p, 1, 3000) <= 0) return -1;
            continue;
        }
        return -1;
    }
    return n;
}

static int send_hdr(SOCKET s, uint32_t seq, uint32_t cmd, const uint32_t *args, uint32_t len) {
    struct hdr h = { TCP_MAGIC, seq, 0, cmd, {0}, len };
    if (args) memcpy(h.args, args, sizeof(h.args));
    return send_all(s, (char *)&h, sizeof(h));
}

/* 1 handled, 0 nothing, -1 broken */
static int handle_packet(SOCKET s, long *n_hb) {
    struct hdr h;
    int r = recv_all(s, (char *)&h, sizeof(h), 1);
    if (r <= 0) return r;
    if (h.magic != TCP_MAGIC) { fprintf(stderr, "broken protocol\n"); return -1; }
    char *buf = NULL;
    if (h.data_len) {
        buf = malloc(h.data_len + 1);
        if (recv_all(s, buf, (int)h.data_len, 0) <= 0) { free(buf); return -1; }
        buf[h.data_len] = 0;
    }
    if (h.cmd == 0) ++*n_hb;
    else if (h.cmd == 5 || h.cmd == 9 || h.cmd == 10) mem_proxy_reply(h.seq, h.cmd, h.args, (uint8_t *)buf, h.data_len);
    free(buf);
    return 1;
}

int main(int argc, char **argv) {
    int port = argc > 1 ? atoi(argv[1]) : 18000;
    double secs = argc > 2 ? atof(argv[2]) : 1e9;
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
    timeBeginPeriod(1);
    if (mem_proxy_init()) return 1;
    uint64_t end = now_us() + (uint64_t)(secs * 1e6);
    long n_hb = 0, n_conn = 0;
    while (now_us() < end) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in a = {0};
        a.sin_family = AF_INET; a.sin_port = htons((u_short)port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(s, (struct sockaddr *)&a, sizeof(a)) != 0) { closesocket(s); Sleep(200); continue; }
        u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
        ++n_conn;
        fprintf(stderr, "[harness] connected (%ld)\n", n_conn);
        mem_proxy_link(true);
        uint32_t seq = 0;
        uint64_t next_hb = now_us() + HB_MS * 1000;
        for (;;) {
            int64_t left = (int64_t)(next_hb - now_us());
            if (left > 0) {
                int ms = (int)((left + 999) / 1000);
                if (mem_proxy_has_work()) { WSAPOLLFD p = { s, POLLIN, 0 }; WSAPoll(&p, 1, ms < 2 ? ms : 2); }
                else mem_proxy_wait(ms);
            }
            int r = 0;
            for (int n = 0; n < 64; ++n) if ((r = handle_packet(s, &n_hb)) <= 0) break;
            if (r < 0) break;
            uint32_t cmd, args[16]; const uint8_t *d; uint32_t len; int tok; int bad = 0;
            while ((tok = mem_proxy_next(&cmd, args, &d, &len)) >= 0) {
                uint32_t sq = seq++;
                int ok = send_hdr(s, sq, cmd, args, len) >= 0 && (!len || send_all(s, (const char *)d, (int)len) >= 0);
                mem_proxy_sent(tok, sq);
                if (!ok) { bad = 1; break; }
            }
            if (bad) break;
            if ((int64_t)(now_us() - next_hb) < 0) continue;
            next_hb = now_us() + HB_MS * 1000;
            if (send_hdr(s, seq++, 0, NULL, 0) < 0) break;
            if (now_us() >= end) break;
        }
        mem_proxy_link(false);
        closesocket(s);
        fprintf(stderr, "[harness] disconnected, heartbeats answered so far %ld\n", n_hb);
    }
    mem_proxy_shutdown();
    return 0;
}
