/* diag.c -- see diag.h */
#include "diag.h"
#include "frame_share.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

uint32_t diag_last_ms[DIAG_T_COUNT];
uint64_t diag_count[DIAG_T_COUNT];
int diag_stage_udp, diag_stage_dec, diag_stage_ren;

#define HB_INTERVAL_S      10
#define STALL_MS           10000
#define LOG_ROTATE_BYTES   (64u * 1024u * 1024u)
#define MAX_DUMPS          4

static uint32_t start_ms;
static volatile int diag_run;
static int diag_enabled;
static char stamp[32];
static int log_part;

static const char *stage_name(int s)
{
    switch (s) {
    case DIAG_UDP_STARTUP: return "udp:startup";
    case DIAG_UDP_RECVFROM: return "udp:recvfrom";
    case DIAG_UDP_POLL: return "udp:poll/wait";
    case DIAG_UDP_ACTION: return "udp:socket_action";
    case DIAG_UDP_REPLY: return "udp:reply";
    case DIAG_DEC_IDLE: return "dec:wait-queue";
    case DIAG_DEC_DECODE: return "dec:decoding";
    case DIAG_DEC_PUBLISH_LOCK: return "dec:waiting-status_lock";
    case DIAG_DEC_PUBLISH: return "dec:publishing";
    case DIAG_REN_IDLE: return "ren:idle";
    case DIAG_REN_WAIT_FRAME: return "ren:wait-frame";
    case DIAG_REN_SYNC_WAIT: return "ren:wait-sync";
    case DIAG_REN_DRAW: return "ren:draw";
    case DIAG_REN_UI: return "ren:ui";
    case DIAG_REN_PRESENT: return "ren:present";
    }
    return "?";
}

const char *diag_ts(void)
{
    static __thread char buf[24];
    uint32_t t = diag_now_ms() - start_ms;
    unsigned s = t / 1000;
    snprintf(buf, sizeof(buf), "%02u:%02u:%02u.%03u", s / 3600, s / 60 % 60, s % 60, t % 1000);
    return buf;
}

static void make_stamp(void)
{
    time_t now = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tmv);
}

static void open_log(void)
{
    char name[128];
    /* part 0 is the first log; rotation cycles through part2..part5 and
       never comes back to append to the first file */
    if (log_part)
        snprintf(name, sizeof(name), "ntrviewer_log_%s_part%d.txt", stamp, log_part + 1);
    else
        snprintf(name, sizeof(name), "ntrviewer_log_%s.txt", stamp);
    if (freopen(name, log_part ? "w" : "a", stderr))
        setvbuf(stderr, NULL, _IONBF, 0);
}

#ifdef _WIN32
/* ------------------------------------------------------------------ Windows */
#include <windows.h>
#include <tlhelp32.h>

typedef BOOL (WINAPI *MiniDumpWriteDump_t)(HANDLE, DWORD, HANDLE, int, void *, void *, void *);
typedef struct { DWORD cb, PageFaultCount; SIZE_T PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage,
    QuotaPagedPoolUsage, QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage, PagefileUsage, PeakPagefileUsage,
    PrivateUsage; } PMC_EX;
typedef BOOL (WINAPI *GetPMI_t)(HANDLE, PMC_EX *, DWORD);
typedef BOOL (WINAPI *GetHandleCount_t)(HANDLE, PDWORD);

static void write_dump(const char *kind, EXCEPTION_POINTERS *ep)
{
    static int dumps;
    if (__atomic_add_fetch(&dumps, 1, __ATOMIC_RELAXED) > MAX_DUMPS)
        return;
    HMODULE dbg = LoadLibraryA("dbghelp.dll");
    if (!dbg)
        return;
    MiniDumpWriteDump_t fn = (MiniDumpWriteDump_t)(void *)GetProcAddress(dbg, "MiniDumpWriteDump");
    char name[160];
    snprintf(name, sizeof(name), "ntrviewer_%s_%s.dmp", kind, stamp);
    HANDLE f = CreateFileA(name, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (fn && f != INVALID_HANDLE_VALUE) {
        struct { DWORD tid; EXCEPTION_POINTERS *ep; BOOL client; } ei = { GetCurrentThreadId(), ep, FALSE };
        /* 0x1000 ThreadInfo | 0x4 WithHandleData | 0x40 IndirectlyReferencedMemory | 0x2 WithDataSegs */
        BOOL ok = fn(GetCurrentProcess(), GetCurrentProcessId(), f, 0x1000 | 0x4 | 0x40 | 0x2,
                     ep ? &ei : NULL, NULL, NULL);
        fprintf(stderr, "[%s] DIAG minidump %s %s\n", diag_ts(), name, ok ? "written" : "FAILED");
    }
    if (f != INVALID_HANDLE_VALUE)
        CloseHandle(f);
    FreeLibrary(dbg);
}

static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep)
{
    fprintf(stderr, "[%s] DIAG CRASH exception 0x%08lx at %p\n", diag_ts(),
            (unsigned long)ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
    write_dump("crash", ep);
    return EXCEPTION_CONTINUE_SEARCH;
}

static int thread_count(void)
{
    int n = 0;
    HANDLE h = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    THREADENTRY32 te = { .dwSize = sizeof(te) };
    DWORD pid = GetCurrentProcessId();
    if (Thread32First(h, &te))
        do { if (te.th32OwnerProcessID == pid) ++n; } while (Thread32Next(h, &te));
    CloseHandle(h);
    return n;
}

static void proc_stats(char *out, size_t n)
{
    static GetPMI_t pmi;
    static GetHandleCount_t hc;
    static int init;
    if (!init) {
        HMODULE k = GetModuleHandleA("kernel32.dll");
        pmi = (GetPMI_t)(void *)GetProcAddress(k, "K32GetProcessMemoryInfo");
        hc = (GetHandleCount_t)(void *)GetProcAddress(k, "GetProcessHandleCount");
        init = 1;
    }
    PMC_EX m = { .cb = sizeof(m) };
    double priv = -1, ws = -1, pws = -1;
    unsigned long pf = 0;
    if (pmi && pmi(GetCurrentProcess(), &m, sizeof(m))) {
        priv = m.PrivateUsage / 1048576.0;
        ws = m.WorkingSetSize / 1048576.0;
        pws = m.PeakWorkingSetSize / 1048576.0;
        pf = m.PageFaultCount;
    }
    DWORD h = 0;
    if (hc) hc(GetCurrentProcess(), &h);
    snprintf(out, n, "priv=%.1fMB ws=%.1fMB peak_ws=%.1fMB faults=%lu handles=%lu gdi=%lu user=%lu threads=%d",
             priv, ws, pws, pf, (unsigned long)h,
             (unsigned long)GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS),
             (unsigned long)GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS), thread_count());
}

static uint64_t log_size(void)
{
    long p = ftell(stderr);
    return p < 0 ? 0 : (uint64_t)p;
}

uint32_t diag_now_ms(void) { return (uint32_t)GetTickCount64(); }
static void sleep_ms(unsigned ms) { Sleep(ms); }
static HANDLE diag_thread;
static DWORD WINAPI diag_main(void *);
static void start_thread(void) { diag_thread = CreateThread(NULL, 0, diag_main, NULL, 0, NULL); }
static void join_thread(void) { if (diag_thread) { WaitForSingleObject(diag_thread, 3000); CloseHandle(diag_thread); } }
#define DIAG_THREAD_RET DWORD WINAPI
#else
/* ------------------------------------------------------------------ POSIX */
#include <pthread.h>
#include <unistd.h>

uint32_t diag_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000);
}
static void sleep_ms(unsigned ms) { usleep(ms * 1000u); }
static void write_dump(const char *kind, void *ep) { (void)kind; (void)ep; }
static uint64_t log_size(void) { long p = ftell(stderr); return p < 0 ? 0 : (uint64_t)p; }

static void proc_stats(char *out, size_t n)
{
    long pages = 0, rss = 0;
    FILE *f = fopen("/proc/self/statm", "r");
    if (f) { if (fscanf(f, "%ld %ld", &pages, &rss) != 2) pages = rss = 0; fclose(f); }
    long ps = sysconf(_SC_PAGESIZE);
    snprintf(out, n, "vsz=%.1fMB rss=%.1fMB", pages * ps / 1048576.0, rss * ps / 1048576.0);
}
static pthread_t diag_thread;
static void *diag_main(void *);
static void start_thread(void) { pthread_create(&diag_thread, NULL, diag_main, NULL); }
static void join_thread(void) { pthread_join(diag_thread, NULL); }
#define DIAG_THREAD_RET void *
#endif


static uint32_t age_of(int k)
{
    return diag_now_ms() - __atomic_load_n(&diag_last_ms[k], __ATOMIC_RELAXED);
}

static uint64_t cnt(int k) { return __atomic_load_n(&diag_count[k], __ATOMIC_RELAXED); }

static void log_stages(const char *why)
{
    fprintf(stderr, "[%s] DIAG %s | stages: %s %s %s | age_ms pkt=%u dec_top=%u dec_bot=%u render=%u main=%u\n",
            diag_ts(), why,
            stage_name(__atomic_load_n(&diag_stage_udp, __ATOMIC_RELAXED)),
            stage_name(__atomic_load_n(&diag_stage_dec, __ATOMIC_RELAXED)),
            stage_name(__atomic_load_n(&diag_stage_ren, __ATOMIC_RELAXED)),
            (unsigned)age_of(DIAG_T_PKT), (unsigned)age_of(DIAG_T_DEC_TOP), (unsigned)age_of(DIAG_T_DEC_BOT),
            (unsigned)age_of(DIAG_T_RENDER), (unsigned)age_of(DIAG_T_MAIN));
}

static DIAG_THREAD_RET diag_main(void *arg)
{
    (void)arg;
    uint64_t last_pkt = 0, last_top = 0, last_bot = 0, last_ren = 0;
    int stall_net = 0, stall_dec = 0, stall_ren = 0;
    unsigned tick = 0;

    while (diag_run) {
        sleep_ms(1000);
        ++tick;

        /* ---- stall detection ---- */
        int net = cnt(DIAG_T_PKT) && age_of(DIAG_T_PKT) > STALL_MS;
        int dec = cnt(DIAG_T_PKT) && cnt(DIAG_T_DEC_TOP) && !net && age_of(DIAG_T_PKT) < 2000 &&
                  age_of(DIAG_T_DEC_TOP) > STALL_MS;
        int ren = cnt(DIAG_T_RENDER) && age_of(DIAG_T_RENDER) > STALL_MS;

        if (net != stall_net) {
            stall_net = net;
            log_stages(net ? "STALL net: no UDP packets for >10s (normal if the game screen is static or the 3DS side stopped sending)"
                           : "RECOVER net: packets are flowing again");
        }
        if (dec != stall_dec) {
            stall_dec = dec;
            log_stages(dec ? "STALL decode: packets arrive but no top frame decoded for >10s" : "RECOVER decode");
            if (dec) write_dump("stall_decode", NULL);
        }
        if (ren != stall_ren) {
            stall_ren = ren;
            log_stages(ren ? "STALL render: render loop has not finished an iteration for >10s" : "RECOVER render");
            if (ren) write_dump("stall_render", NULL);
        }

        /* ---- heartbeat ---- */
        if (tick % HB_INTERVAL_S)
            continue;

        uint64_t pkt = cnt(DIAG_T_PKT), top = cnt(DIAG_T_DEC_TOP), bot = cnt(DIAG_T_DEC_BOT), ren_c = cnt(DIAG_T_RENDER);
        char ps[200];
        proc_stats(ps, sizeof(ps));
        fprintf(stderr,
                "[%s] HB pkts=%llu(+%llu) dec_top=%llu(+%llu) dec_bot=%llu(+%llu) render=%llu(+%llu) shared_idx=%u "
                "age_ms[pkt=%u top=%u render=%u main=%u] stages[%s %s %s] %s\n",
                diag_ts(), (unsigned long long)pkt, (unsigned long long)(pkt - last_pkt),
                (unsigned long long)top, (unsigned long long)(top - last_top),
                (unsigned long long)bot, (unsigned long long)(bot - last_bot),
                (unsigned long long)ren_c, (unsigned long long)(ren_c - last_ren),
                (unsigned)frame_share_get_index(),
                (unsigned)age_of(DIAG_T_PKT), (unsigned)age_of(DIAG_T_DEC_TOP), (unsigned)age_of(DIAG_T_RENDER),
                (unsigned)age_of(DIAG_T_MAIN),
                stage_name(__atomic_load_n(&diag_stage_udp, __ATOMIC_RELAXED)),
                stage_name(__atomic_load_n(&diag_stage_dec, __ATOMIC_RELAXED)),
                stage_name(__atomic_load_n(&diag_stage_ren, __ATOMIC_RELAXED)), ps);
        last_pkt = pkt; last_top = top; last_bot = bot; last_ren = ren_c;

        if (log_size() > LOG_ROTATE_BYTES) {
            fprintf(stderr, "[%s] DIAG log full, rotating\n", diag_ts());
            log_part = log_part % 4 + 1;
            open_log();
        }
    }
    return 0;
}

void diag_init(void)
{
    if (diag_enabled)
        return;
    const char *off = getenv("NTRVIEWER_NO_LOG");
    if (off && *off == '1')
        return;
    diag_enabled = 1;
    start_ms = diag_now_ms();
    make_stamp();
    open_log();
    fprintf(stderr, "[%s] DIAG start %s (build %s %s)\n", diag_ts(), stamp, __DATE__, __TIME__);
#ifdef _WIN32
    SetUnhandledExceptionFilter(crash_filter);
#endif
    diag_run = 1;
    start_thread();
}

void diag_close(void)
{
    if (!diag_enabled)
        return;
    diag_run = 0;
    join_thread();
    fprintf(stderr, "[%s] DIAG clean exit\n", diag_ts());
    diag_enabled = 0;
}
