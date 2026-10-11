// "DS mode" receiver, see ntr_ds.h for the packet layout.

#include "ntr_ds.h"
#include <string.h>

void ds_assembler_reset(struct ds_assembler *a)
{
    memset(a, 0, sizeof(*a));
    for (int i = 0; i < DS_SCREEN_W * DS_SCREEN_H; ++i)
        a->rgba[i * 4 + 3] = 255;
}

static void ds_show(struct ds_assembler *a, int screen, ds_publish_fn publish, void *user)
{
    if (a->pending && publish)
        publish(screen, a->rgba, DS_SCREEN_H, DS_SCREEN_W, a->bytes, user);
    a->pending = false;
    a->bytes = 0;
}

void ds_flush(struct ds_assembler *a, int screen, ds_publish_fn publish, void *user)
{
    ds_show(a, screen, publish, user);
}

static void ds_put_line(struct ds_assembler *a, int y, const uint8_t *src, int format)
{
    // screen pixel (x, y) lives at row x, column (H - 1 - y) of the rotated buffer
    uint8_t *dst = a->rgba + (DS_SCREEN_H - 1 - y) * 4;
    for (int x = 0; x < DS_SCREEN_W; ++x, src += 2, dst += DS_SCREEN_H * 4) {
        unsigned p = src[0] | (src[1] << 8);
        unsigned r, g, b;
        if (format == DS_FORMAT_RGB565) {
            r = (p >> 11) & 0x1f;
            g = (p >> 5) & 0x3f;
            b = p & 0x1f;
            g = (g << 2) | (g >> 4);
        } else {
            r = p & 0x1f;
            g = (p >> 5) & 0x1f;
            b = (p >> 10) & 0x1f;
            g = (g << 3) | (g >> 2);
        }
        dst[0] = (uint8_t)((r << 3) | (r >> 2));
        dst[1] = (uint8_t)g;
        dst[2] = (uint8_t)((b << 3) | (b >> 2));
        dst[3] = 255;
    }
}

int ds_feed(struct ds_assembler a_all[DS_SCREEN_COUNT], const uint8_t *buf, int len, ds_publish_fn publish, void *user)
{
    if (len < DS_HDR_SIZE || memcmp(buf, "NDS1", 4) != 0)
        return -1;
    uint16_t frame_id = (uint16_t)(buf[4] | (buf[5] << 8));
    int screen = buf[6];
    int format = buf[7];
    int line_start = buf[8];
    int line_count = buf[9];
    if (screen >= DS_SCREEN_COUNT || format > DS_FORMAT_RGB565 || line_count == 0 ||
        line_start + line_count > DS_SCREEN_H ||
        len != DS_HDR_SIZE + line_count * DS_SCREEN_W * 2)
        return -1;

    struct ds_assembler *a = &a_all[screen];
    if (a->started && frame_id != a->frame_id) {
        if ((int16_t)(frame_id - a->frame_id) < 0)
            return -1; // late packet from an older frame
        ds_show(a, screen, publish, user); // show what we have of the previous frame
        memset(a->lines_seen, 0, sizeof(a->lines_seen));
        a->lines_seen_count = 0;
    } else if (a->started && a->lines_seen_count == DS_SCREEN_H) {
        return screen; // duplicate of a frame already shown
    }
    a->started = true;
    a->frame_id = frame_id;

    const uint8_t *src = buf + DS_HDR_SIZE;
    for (int i = 0; i < line_count; ++i, src += DS_SCREEN_W * 2) {
        int y = line_start + i;
        ds_put_line(a, y, src, format);
        if (!a->lines_seen[y]) {
            a->lines_seen[y] = 1;
            ++a->lines_seen_count;
        }
    }
    a->pending = true;
    a->bytes += len;

    if (a->lines_seen_count == DS_SCREEN_H)
        ds_show(a, screen, publish, user);
    return screen;
}

#ifndef NTR_DS_TEST

#include "main.h"
#include "ntr_common.h"
#include "ntr_rp.h"
#include <stdlib.h>
#ifndef _WIN32
#include <sys/select.h>
#endif

static struct ds_assembler ds_asm[DS_SCREEN_COUNT];

static void ds_publish(int screen, const uint8_t *rgba, int width, int height, int bytes, UNUSED void *user)
{
    rp_publish_frame(screen == 0 ? SCREEN_TOP : SCREEN_BOT, rgba, width, height, bytes);
}

static int ds_port(void)
{
    const char *env = getenv("NTRVIEWER_DS_PORT");
    int port = env ? atoi(env) : 0;
    return port > 0 && port < 65536 ? port : DS_PORT_DEFAULT;
}

thread_ret_t ds_recv_thread_func(UNUSED void *arg)
{
    static uint8_t buf[2048];
    int port = ds_port();

    while (program_running) {
        SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (!socket_valid(sock)) {
            err_log("ds: socket creation failed\n");
            Sleep(SOCKET_RESET_INTERVAL_MS);
            continue;
        }
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
            err_log("ds: bind failed for UDP port %d\n", port);
            closesocket(sock);
            Sleep(SOCKET_RESET_INTERVAL_MS);
            continue;
        }
        int rcvbuf = 1024 * 1024;
        setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (char *)&rcvbuf, sizeof(rcvbuf));
        err_log("ds: listening for DS mode frames on UDP port %d\n", port);

        for (int i = 0; i < DS_SCREEN_COUNT; ++i)
            ds_assembler_reset(&ds_asm[i]);
        bool streaming = false;
        uint32_t last_packet = 0;
        uint32_t last_screen_packet[DS_SCREEN_COUNT] = {0};

        while (program_running) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(sock, &fds);
            struct timeval tv = {0, 50 * 1000};
            int ret = select((int)sock + 1, &fds, NULL, NULL, &tv);
            uint32_t now = iclock() / 1000;
            if (ret < 0) {
                err_log("ds: select failed: %d\n", socket_errno());
                break;
            }
            if (ret > 0) {
                struct sockaddr_in from;
                socklen_t from_len = sizeof(from);
                int len = recvfrom(sock, (char *)buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);
                if (len < 0) {
                    // Windows reports ICMP errors here; just keep listening
                    continue;
                }
                int screen = ds_feed(ds_asm, buf, len, ds_publish, NULL);
                if (screen >= 0) {
                    last_packet = now;
                    last_screen_packet[screen] = now;
                    if (!streaming) {
                        uint8_t *o = (uint8_t *)&from.sin_addr.s_addr;
                        err_log("ds: DS mode stream started from %d.%d.%d.%d\n", o[0], o[1], o[2], o[3]);
                        streaming = true;
                    }
                }
            }
            for (int i = 0; i < DS_SCREEN_COUNT; ++i) {
                if (ds_asm[i].pending && now - last_screen_packet[i] >= DS_FLUSH_MS)
                    ds_flush(&ds_asm[i], i, ds_publish, NULL);
            }
            if (streaming && now - last_packet >= 2000) {
                err_log("ds: DS mode stream stopped\n");
                streaming = false;
            }
        }
        closesocket(sock);
    }
    return 0;
}

#endif
