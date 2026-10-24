/* mem_proxy -- see mem_proxy.h for the protocol. */
#include "mem_proxy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef CRITICAL_SECTION mp_mutex_t;
typedef CONDITION_VARIABLE mp_cond_t;
typedef HANDLE mp_thread_t;
#define MP_THREAD_RET DWORD WINAPI
static void mp_mutex_init(mp_mutex_t *m) { InitializeCriticalSection(m); }
static void mp_lock(mp_mutex_t *m) { EnterCriticalSection(m); }
static void mp_unlock(mp_mutex_t *m) { LeaveCriticalSection(m); }
static void mp_cond_init(mp_cond_t *c) { InitializeConditionVariable(c); }
static void mp_cond_wait_ms(mp_cond_t *c, mp_mutex_t *m, int ms) { SleepConditionVariableCS(c, m, ms < 0 ? 0 : (DWORD)ms); }
static void mp_cond_broadcast(mp_cond_t *c) { WakeAllConditionVariable(c); }
static uint64_t mp_now_us(void) {
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (uint64_t)(t.QuadPart / freq.QuadPart) * 1000000 + (uint64_t)(t.QuadPart % freq.QuadPart) * 1000000 / freq.QuadPart;
}
#define mp_close closesocket
typedef int mp_socklen_t;
#else
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
typedef int SOCKET;
#define INVALID_SOCKET (-1)
typedef pthread_mutex_t mp_mutex_t;
typedef pthread_cond_t mp_cond_t;
typedef pthread_t mp_thread_t;
#define MP_THREAD_RET void *
static void mp_mutex_init(mp_mutex_t *m) { pthread_mutex_init(m, NULL); }
static void mp_lock(mp_mutex_t *m) { pthread_mutex_lock(m); }
static void mp_unlock(mp_mutex_t *m) { pthread_mutex_unlock(m); }
static void mp_cond_init(mp_cond_t *c) {
    pthread_condattr_t a;
    pthread_condattr_init(&a);
#ifndef __APPLE__
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
#endif
    pthread_cond_init(c, &a);
}
static void mp_cond_wait_ms(mp_cond_t *c, mp_mutex_t *m, int ms) {
    struct timespec ts;
#ifdef __APPLE__
    clock_gettime(CLOCK_REALTIME, &ts);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    if (ms < 0)
        ms = 0;
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000;
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000;
    }
    pthread_cond_timedwait(c, m, &ts);
}
static void mp_cond_broadcast(mp_cond_t *c) { pthread_cond_broadcast(c); }
static uint64_t mp_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}
#define mp_close close
typedef socklen_t mp_socklen_t;
#endif

#ifdef MEM_PROXY_STANDALONE
#define mp_log(...) fprintf(stderr, "[mem_proxy] " __VA_ARGS__)
#else
#include "const.h"
#define mp_log(...) err_log(__VA_ARGS__)
#endif

#define MP_MAX_JOBS 8
#define MP_CHUNK 0x4000          /* largest command NTR-HR accepts */
#define MP_MAX_DATA 0x8000       /* largest read/write one request may ask for */
#define MP_TIMEOUT_MS 2000
#define MP_HDR 16
#define MP_PKT_MAX (MP_HDR + 12 + MP_MAX_DATA)
#define MP_GAME_PID 0xFFFFFFFFu
#define MP_V1_MAX_DATA 1024      /* scripting/citra.py receive buffer */

enum { J_FREE, J_QUEUED, J_SENDING, J_SENT, J_DONE };

struct mp_job {
    int state;
    bool abandoned;              /* its request gave up while the control thread was sending it */
    uint32_t gen;
    uint32_t cmd;
    uint32_t args[16];
    uint8_t *wdata;              /* write payload (owned) */
    uint32_t wlen;
    uint32_t seq;
    uint64_t sent_us;
    /* result */
    uint32_t status, pid, detail, count;
    uint8_t *out;                /* reply data (owned) */
    uint32_t out_len;
};

static struct {
    mp_mutex_t lock;
    mp_cond_t done_cond;         /* a job finished */
    mp_cond_t queued_cond;       /* a job was queued */
    bool inited;
    volatile bool running;
    bool link_up;
    bool supported;              /* this connection's NTR-HR answered a memory command */
    uint32_t gen;
    struct mp_job jobs[MP_MAX_JOBS];
    uint32_t current_pid;
    uint32_t game_pid;
    uint32_t n_requests, n_failures, last_rtt_us, avg_rtt_us;
    SOCKET sock;
    mp_thread_t thread;
} mp;

static void job_free(struct mp_job *j) {
    free(j->wdata);
    free(j->out);
    memset(j, 0, sizeof(*j));
    j->state = J_FREE;
}

/* --- control-link side ------------------------------------------------ */

void mem_proxy_link(bool up) {
    if (!mp.inited)
        return;
    mp_lock(&mp.lock);
    mp.link_up = up;
    mp.supported = false;
    if (!up) {
        for (int i = 0; i < MP_MAX_JOBS; ++i) {
            struct mp_job *j = &mp.jobs[i];
            if (j->state == J_QUEUED || j->state == J_SENDING || j->state == J_SENT) {
                if (j->abandoned) {
                    job_free(j);
                } else {
                    j->state = J_DONE;
                    j->status = MEM_PROXY_ST_NO_LINK;
                }
            }
        }
        mp_cond_broadcast(&mp.done_cond);
    }
    mp_unlock(&mp.lock);
}

bool mem_proxy_has_work(void) {
    if (!mp.inited)
        return false;
    bool work = false;
    mp_lock(&mp.lock);
    for (int i = 0; i < MP_MAX_JOBS && !work; ++i)
        work = mp.jobs[i].state == J_QUEUED || mp.jobs[i].state == J_SENT;
    mp_unlock(&mp.lock);
    return work;
}

void mem_proxy_wait(int timeout_ms) {
    if (!mp.inited) {
#ifdef _WIN32
        Sleep(timeout_ms);
#else
        usleep(timeout_ms * 1000);
#endif
        return;
    }
    mp_lock(&mp.lock);
    bool queued = false;
    for (int i = 0; i < MP_MAX_JOBS && !queued; ++i)
        queued = mp.jobs[i].state == J_QUEUED;
    if (!queued)
        mp_cond_wait_ms(&mp.queued_cond, &mp.lock, timeout_ms);
    mp_unlock(&mp.lock);
}

int mem_proxy_next(uint32_t *cmd, uint32_t args[16], const uint8_t **data, uint32_t *data_len) {
    if (!mp.inited)
        return -1;
    int token = -1;
    mp_lock(&mp.lock);
    /* oldest queued job first (lowest gen) */
    struct mp_job *best = NULL;
    int best_i = -1;
    for (int i = 0; i < MP_MAX_JOBS; ++i) {
        struct mp_job *j = &mp.jobs[i];
        if (j->state == J_QUEUED && (!best || (int32_t)(j->gen - best->gen) < 0)) {
            best = j;
            best_i = i;
        }
    }
    if (best && mp.link_up) {
        best->state = J_SENDING;
        *cmd = best->cmd;
        memcpy(args, best->args, sizeof(best->args));
        *data = best->wdata;
        *data_len = best->wlen;
        token = (int)((best->gen & 0xffffff) << 4) | best_i;
    }
    mp_unlock(&mp.lock);
    return token;
}

void mem_proxy_sent(int token, uint32_t seq) {
    if (!mp.inited || token < 0)
        return;
    int i = token & 0xf;
    uint32_t gen = (uint32_t)token >> 4;
    mp_lock(&mp.lock);
    struct mp_job *j = &mp.jobs[i];
    if ((j->gen & 0xffffff) == gen && j->state == J_SENDING) {
        if (j->abandoned) {
            job_free(j);
        } else {
            j->state = J_SENT;
            j->seq = seq;
            j->sent_us = mp_now_us();
        }
    }
    mp_unlock(&mp.lock);
}

bool mem_proxy_reply(uint32_t seq, uint32_t cmd, const uint32_t args[16], const uint8_t *data, uint32_t data_len) {
    if (!mp.inited)
        return false;
    bool matched = false;
    mp_lock(&mp.lock);
    for (int i = 0; i < MP_MAX_JOBS; ++i) {
        struct mp_job *j = &mp.jobs[i];
        if (j->state != J_SENT || j->seq != seq || j->cmd != cmd)
            continue;
        matched = true;
        mp.supported = true;
        j->status = args[0];
        j->pid = args[1];
        j->detail = args[2];
        if (cmd == MEM_PROXY_CMD_LISTPROCESS) {
            j->count = args[1];
            j->pid = args[2];
            if (args[2])
                mp.game_pid = args[2];
        } else if (j->args[0] == MP_GAME_PID && j->status == MEM_PROXY_ST_OK) {
            mp.game_pid = args[1];
        }
        if (data_len) {
            j->out = (uint8_t *)malloc(data_len);
            if (j->out) {
                memcpy(j->out, data, data_len);
                j->out_len = data_len;
            } else {
                j->status = MEM_PROXY_ST_BAD_REQUEST;
            }
        }
        uint32_t rtt = (uint32_t)(mp_now_us() - j->sent_us);
        mp.last_rtt_us = rtt;
        mp.avg_rtt_us = mp.avg_rtt_us ? (mp.avg_rtt_us * 7 + rtt) / 8 : rtt;
        ++mp.n_requests;
        if (j->status != MEM_PROXY_ST_OK)
            ++mp.n_failures;
        j->state = J_DONE;
        mp_cond_broadcast(&mp.done_cond);
        break;
    }
    mp_unlock(&mp.lock);
    return matched;
}

/* --- request side (UDP thread) ------------------------------------------ */

/* Queue up to n jobs (cmd/args/wdata already set in tmpl[]) and wait for all of them.
   On return out[k] holds each finished job (state J_DONE or a timeout status); the caller frees with jobs_release. */
static int run_jobs(struct mp_job *tmpl, int n, struct mp_job **out) {
    for (int k = 0; k < n; ++k)
        out[k] = NULL;
    mp_lock(&mp.lock);
    if (!mp.link_up) {
        mp_unlock(&mp.lock);
        for (int k = 0; k < n; ++k)
            free(tmpl[k].wdata);
        return MEM_PROXY_ST_NO_LINK;
    }
    int got = 0;
    for (int i = 0; i < MP_MAX_JOBS && got < n; ++i) {
        if (mp.jobs[i].state != J_FREE)
            continue;
        struct mp_job *j = &mp.jobs[i];
        *j = tmpl[got];
        j->state = J_QUEUED;
        j->abandoned = false;
        j->gen = ++mp.gen;
        out[got++] = j;
    }
    if (got < n) { /* no free slots (stuck commands): give up */
        for (int k = 0; k < got; ++k) {
            job_free(out[k]);
            out[k] = NULL;
        }
        for (int k = got; k < n; ++k)
            free(tmpl[k].wdata);
        mp_unlock(&mp.lock);
        return MEM_PROXY_ST_TIMEOUT;
    }
    mp_cond_broadcast(&mp.queued_cond);

    uint64_t deadline = mp_now_us() + (uint64_t)MP_TIMEOUT_MS * 1000;
    for (;;) {
        bool all = true;
        for (int k = 0; k < n; ++k)
            all = all && out[k]->state == J_DONE;
        if (all)
            break;
        uint64_t now = mp_now_us();
        if (now >= deadline || !mp.running) {
            for (int k = 0; k < n; ++k) {
                struct mp_job *j = out[k];
                if (j->state == J_DONE)
                    continue;
                ++mp.n_failures;
                if (j->state == J_SENDING) {
                    j->abandoned = true;    /* the control thread frees it in mem_proxy_sent */
                    out[k] = NULL;
                } else {
                    j->state = J_DONE;      /* a late reply finds no J_SENT job and is dropped */
                    j->status = MEM_PROXY_ST_TIMEOUT;
                }
            }
            break;
        }
        mp_cond_wait_ms(&mp.done_cond, &mp.lock, (int)((deadline - now) / 1000) + 1);
    }
    mp_unlock(&mp.lock);

    int status = MEM_PROXY_ST_OK;
    for (int k = 0; k < n && status == MEM_PROXY_ST_OK; ++k)
        status = out[k] ? (int)out[k]->status : MEM_PROXY_ST_TIMEOUT;
    return status;
}

static void jobs_release(struct mp_job **out, int n) {
    mp_lock(&mp.lock);
    for (int k = 0; k < n; ++k)
        if (out[k] && out[k]->state == J_DONE)
            job_free(out[k]);
    mp_unlock(&mp.lock);
}

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static uint32_t get32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

static uint8_t reply_buf[MP_PKT_MAX];

static void send_reply(const struct sockaddr_in *to, uint32_t ver, uint32_t id, uint32_t type, const uint8_t *data, uint32_t len) {
    put32(reply_buf + 0, ver);
    put32(reply_buf + 4, id);
    put32(reply_buf + 8, type);
    put32(reply_buf + 12, len);
    if (len && data != reply_buf + MP_HDR)
        memmove(reply_buf + MP_HDR, data, len);
    sendto(mp.sock, (const char *)reply_buf, (int)(MP_HDR + len), 0, (const struct sockaddr *)to, sizeof(*to));
}

/* Read [addr, addr + size) of pid into dst (in MP_CHUNK commands). Returns status; *pid_out = pid used. */
static int do_read(uint32_t pid, uint32_t addr, uint32_t size, uint8_t *dst, uint32_t *pid_out, uint32_t *detail_out) {
    struct mp_job tmpl[MP_MAX_DATA / MP_CHUNK];
    struct mp_job *out[MP_MAX_DATA / MP_CHUNK];
    int n = 0;
    for (uint32_t off = 0; off < size; off += MP_CHUNK) {
        struct mp_job *t = &tmpl[n++];
        memset(t, 0, sizeof(*t));
        t->cmd = MEM_PROXY_CMD_READMEM;
        t->args[0] = pid;
        t->args[1] = addr + off;
        t->args[2] = size - off < MP_CHUNK ? size - off : MP_CHUNK;
    }
    int st = run_jobs(tmpl, n, out);
    uint32_t off = 0;
    *pid_out = pid;
    *detail_out = 0;
    for (int k = 0; k < n; ++k) {
        struct mp_job *j = out[k];
        if (!j)
            continue;
        if (j->status != MEM_PROXY_ST_OK) {
            *detail_out = j->detail;
        } else if (j->out_len != j->args[2]) {
            st = MEM_PROXY_ST_BAD_REQUEST;
        } else {
            memcpy(dst + off, j->out, j->out_len);
            *pid_out = j->pid;
        }
        off += j->args[2];
    }
    jobs_release(out, n);
    return st;
}

static int probe(void) {
    struct mp_job t;
    struct mp_job *out[1];
    memset(&t, 0, sizeof(t));
    t.cmd = MEM_PROXY_CMD_LISTPROCESS;
    int st = run_jobs(&t, 1, out);
    jobs_release(out, 1);
    return st;
}

static int do_write(uint32_t pid, uint32_t addr, uint32_t flags, const uint8_t *src, uint32_t size, uint32_t *pid_out, uint32_t *detail_out) {
    *pid_out = pid;
    *detail_out = 0;
    mp_lock(&mp.lock);
    bool supported = mp.supported;
    mp_unlock(&mp.lock);
    /* an NTR-HR without memory commands would not consume the write's data and the link would break */
    if (!supported) {
        int st = probe();
        if (st != MEM_PROXY_ST_OK)
            return st;
    }
    struct mp_job tmpl[MP_MAX_DATA / MP_CHUNK];
    struct mp_job *out[MP_MAX_DATA / MP_CHUNK];
    int n = 0;
    for (uint32_t off = 0; off < size; off += MP_CHUNK) {
        struct mp_job *t = &tmpl[n++];
        memset(t, 0, sizeof(*t));
        uint32_t len = size - off < MP_CHUNK ? size - off : MP_CHUNK;
        t->cmd = MEM_PROXY_CMD_WRITEMEM;
        t->args[0] = pid;
        t->args[1] = addr + off;
        t->args[2] = len;
        t->args[3] = flags;
        t->wdata = (uint8_t *)malloc(len);
        if (!t->wdata) {
            for (int k = 0; k < n - 1; ++k)
                free(tmpl[k].wdata);
            return MEM_PROXY_ST_BAD_REQUEST;
        }
        memcpy(t->wdata, src + off, len);
        t->wlen = len;
    }
    int st = run_jobs(tmpl, n, out);
    for (int k = 0; k < n; ++k)
        if (out[k] && out[k]->status != MEM_PROXY_ST_OK)
            *detail_out = out[k]->detail;
        else if (out[k])
            *pid_out = out[k]->pid;
    jobs_release(out, n);
    return st;
}

/* Process list as Azahar's 0x14-byte entries {u32 pid, u64 title id, char name[8]} into dst; returns count or -status. */
static int do_list(uint8_t *dst, int max_entries) {
    struct mp_job t;
    struct mp_job *out[1];
    memset(&t, 0, sizeof(t));
    t.cmd = MEM_PROXY_CMD_LISTPROCESS;
    int st = run_jobs(&t, 1, out);
    int count = 0;
    struct mp_job *j = out[0];
    if (st == MEM_PROXY_ST_OK && j) {
        int n = (int)(j->out_len / 24);
        for (int i = 0; i < n && count < max_entries; ++i) {
            const uint8_t *e = j->out + i * 24;
            uint8_t *d = dst + count * 0x14;
            memcpy(d, e, 4);          /* pid */
            memcpy(d + 4, e + 8, 8);  /* title id */
            memcpy(d + 12, e + 16, 8);/* name */
            ++count;
        }
    }
    jobs_release(out, 1);
    return st == MEM_PROXY_ST_OK ? count : -st;
}

static uint8_t req_buf[MP_PKT_MAX + 64];
static uint8_t data_buf[MP_MAX_DATA + 12];
static uint8_t list_buf[64 * 0x14];

static void handle_request(const uint8_t *p, int len, const struct sockaddr_in *from) {
    if (len < MP_HDR)
        return;
    uint32_t ver = get32(p), id = get32(p + 4), type = get32(p + 8), size = get32(p + 12);
    if ((ver != 1 && ver != 2) || size != (uint32_t)(len - MP_HDR))
        return;
    const uint8_t *d = p + MP_HDR;
    uint8_t *r = reply_buf + MP_HDR;
    uint32_t pid_used = 0, detail = 0;
    int st;

    switch (type) {
    case 1: { /* read */
        if (size < 8)
            return;
        uint32_t addr = get32(d), n = get32(d + 4);
        if (n == 0 || n > MP_MAX_DATA) {
            send_reply(from, ver, id, type, NULL, 0);
            return;
        }
        st = do_read(mp.current_pid, addr, n, data_buf, &pid_used, &detail);
        if (st == MEM_PROXY_ST_OK) {
            memcpy(r, data_buf, n);
            send_reply(from, ver, id, type, r, n);
        } else {
            send_reply(from, ver, id, type, NULL, 0);
        }
        return;
    }
    case 2: { /* write */
        if (size < 8)
            return;
        uint32_t addr = get32(d), n = get32(d + 4);
        if (n == 0 || n > MP_MAX_DATA || n != size - 8)
            return;
        st = do_write(mp.current_pid, addr, 0, d + 8, n, &pid_used, &detail);
        if (st == MEM_PROXY_ST_OK)
            send_reply(from, ver, id, type, NULL, 0);
        return; /* Azahar sends nothing back for a failed write */
    }
    case 3:
    case 0x100: { /* process list */
        uint32_t start = size >= 4 ? get32(d) : 0, max = size >= 8 ? get32(d + 4) : 0x7FFFFFFF;
        int count = do_list(list_buf, 64);
        if (count < 0)
            count = 0;
        uint32_t fit = ver == 1 ? (MP_V1_MAX_DATA - 4) / 0x14 : 64;
        uint32_t n = start < (uint32_t)count ? (uint32_t)count - start : 0;
        if (n > max) n = max;
        if (n > fit) n = fit;
        put32(r, n);
        if (n)
            memcpy(r + 4, list_buf + start * 0x14, n * 0x14);
        send_reply(from, ver, id, type, r, 4 + n * 0x14);
        return;
    }
    case 4: { /* set/get process */
        uint32_t mode = size >= 4 ? get32(d) : 0, pid = size >= 8 ? get32(d + 4) : 0;
        if (mode == 1) {
            mp.current_pid = pid ? pid : MP_GAME_PID;
            send_reply(from, ver, id, type, NULL, 0);
        } else {
            uint32_t cur = mp.current_pid;
            if (cur == MP_GAME_PID && mp.game_pid)
                cur = mp.game_pid;
            put32(r, cur);
            send_reply(from, ver, id, type, r, 4);
        }
        return;
    }
    case 0x101: { /* read with pid and status */
        if (size < 12) {
            put32(r, MEM_PROXY_ST_BAD_REQUEST); put32(r + 4, 0); put32(r + 8, 0);
            send_reply(from, ver, id, type, r, 12);
            return;
        }
        uint32_t pid = get32(d), addr = get32(d + 4), n = get32(d + 8);
        if (n == 0 || n > MP_MAX_DATA)
            st = MEM_PROXY_ST_BAD_REQUEST;
        else
            st = do_read(pid, addr, n, data_buf, &pid_used, &detail);
        put32(r, (uint32_t)st);
        put32(r + 4, pid_used);
        put32(r + 8, detail);
        if (st == MEM_PROXY_ST_OK) {
            memcpy(r + 12, data_buf, n);
            send_reply(from, ver, id, type, r, 12 + n);
        } else {
            send_reply(from, ver, id, type, r, 12);
        }
        return;
    }
    case 0x102: { /* write with pid, flags and status */
        if (size < 12 || size - 12 > MP_MAX_DATA || size == 12) {
            st = MEM_PROXY_ST_BAD_REQUEST;
        } else {
            uint32_t pid = get32(d), addr = get32(d + 4), flags = get32(d + 8);
            st = do_write(pid, addr, flags, d + 12, size - 12, &pid_used, &detail);
        }
        put32(r, (uint32_t)st);
        put32(r + 4, pid_used);
        put32(r + 8, detail);
        send_reply(from, ver, id, type, r, 12);
        return;
    }
    case 0x103: { /* status */
        mp_lock(&mp.lock);
        put32(r, mp.link_up);
        put32(r + 4, mp.supported);
        put32(r + 8, mp.n_requests);
        put32(r + 12, mp.n_failures);
        put32(r + 16, mp.last_rtt_us);
        put32(r + 20, mp.avg_rtt_us);
        put32(r + 24, mp.game_pid);
        mp_unlock(&mp.lock);
        send_reply(from, ver, id, type, r, 28);
        return;
    }
    default:
        return;
    }
}

static MP_THREAD_RET udp_thread(void *arg) {
    (void)arg;
    while (mp.running) {
        struct sockaddr_in from;
        mp_socklen_t flen = sizeof(from);
        int n = recvfrom(mp.sock, (char *)req_buf, sizeof(req_buf), 0, (struct sockaddr *)&from, &flen);
        if (n < 0)
            continue; /* timeout (checks running) or a transient error */
        handle_request(req_buf, n, &from);
    }
    return 0;
}

int mem_proxy_init(void) {
    if (mp.inited)
        return 0;
    memset(&mp, 0, sizeof(mp));
    mp_mutex_init(&mp.lock);
    mp_cond_init(&mp.done_cond);
    mp_cond_init(&mp.queued_cond);
    mp.current_pid = MP_GAME_PID;

    mp.sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (mp.sock == INVALID_SOCKET) {
        mp_log("socket failed\n");
        return -1;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(MEM_PROXY_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(mp.sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        mp_log("bind 127.0.0.1:%d failed (another NTR-VC running?)\n", MEM_PROXY_PORT);
        mp_close(mp.sock);
        return -1;
    }
#ifdef _WIN32
    /* without this, an ICMP "port unreachable" from a client that went away makes recvfrom fail */
    BOOL no_reset = FALSE;
    DWORD ret_bytes = 0;
    WSAIoctl(mp.sock, _WSAIOW(IOC_VENDOR, 12) /* SIO_UDP_CONNRESET */, &no_reset, sizeof(no_reset), NULL, 0, &ret_bytes, NULL, NULL);
    DWORD tv = 500;
    setsockopt(mp.sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
    int buf = 4 * MP_PKT_MAX;
    setsockopt(mp.sock, SOL_SOCKET, SO_SNDBUF, (const char *)&buf, sizeof(buf));
    setsockopt(mp.sock, SOL_SOCKET, SO_RCVBUF, (const char *)&buf, sizeof(buf));
#else
    struct timeval tv = {0, 500000};
    setsockopt(mp.sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int buf = 4 * MP_PKT_MAX;
    setsockopt(mp.sock, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
    setsockopt(mp.sock, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));
#endif
    mp.running = true;
    mp.inited = true;
#ifdef _WIN32
    mp.thread = CreateThread(NULL, 0, udp_thread, NULL, 0, NULL);
    if (!mp.thread) {
#else
    if (pthread_create(&mp.thread, NULL, udp_thread, NULL) != 0) {
#endif
        mp_log("thread create failed\n");
        mp.running = false;
        mp.inited = false;
        mp_close(mp.sock);
        return -1;
    }
    mp_log("memory server on 127.0.0.1:%d (Azahar RPC format)\n", MEM_PROXY_PORT);
    return 0;
}

void mem_proxy_shutdown(void) {
    if (!mp.inited)
        return;
    mp.running = false;
    mp_lock(&mp.lock);
    mp_cond_broadcast(&mp.done_cond);
    mp_unlock(&mp.lock);
#ifdef _WIN32
    WaitForSingleObject(mp.thread, 2000);
    CloseHandle(mp.thread);
#else
    pthread_join(mp.thread, NULL);
#endif
    mp_close(mp.sock);
    mp.inited = false;
}
