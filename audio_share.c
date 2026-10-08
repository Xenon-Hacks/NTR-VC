/* audio_share.c -- see audio_share.h */
#include "audio_share.h"
#include "ntr_audio.h"

#include <stddef.h>
#include <string.h>

#define AS_MAGIC "NTRAUDIO"
#define AS_VERSION 1u
#define AS_HEADER_SIZE 64u
#define AS_RING_FRAMES 262144u          /* ~8 s at 32728 Hz */
#define AS_FRAME_BYTES ((uint32_t)RP_AUDIO_CHANNELS * 2u)
#define AS_TOTAL_SIZE (AS_HEADER_SIZE + AS_RING_FRAMES * AS_FRAME_BYTES)

struct as_header {
    char magic[8];
    uint32_t version;
    uint32_t header_size;
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t ring_frames;
    uint32_t stream_id;
    uint64_t write_count;
    uint64_t write_time_ms;
    uint32_t reserved[4];
};
_Static_assert(sizeof(struct as_header) == AS_HEADER_SIZE, "audio_share header must be 64 bytes");

static struct as_header *as_hdr;
static uint8_t *as_ring;

#ifdef _WIN32
#include <windows.h>
static HANDLE as_mapping;

static uint64_t as_now_ms(void) { return (uint64_t)GetTickCount64(); }

static void *as_map(void)
{
    as_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                    0, AS_TOTAL_SIZE, L"Local\\NTRViewerHR_Audio");
    if (!as_mapping)
        return NULL;
    void *p = MapViewOfFile(as_mapping, FILE_MAP_ALL_ACCESS, 0, 0, AS_TOTAL_SIZE);
    if (!p) {
        CloseHandle(as_mapping);
        as_mapping = NULL;
    }
    return p;
}

static void as_unmap(void *p)
{
    if (p)
        UnmapViewOfFile(p);
    if (as_mapping)
        CloseHandle(as_mapping);
    as_mapping = NULL;
}
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#define AS_POSIX_NAME "/ntrviewer_hr_audio"

static uint64_t as_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

static void *as_map(void)
{
    int fd = shm_open(AS_POSIX_NAME, O_CREAT | O_RDWR, 0600);
    if (fd < 0)
        return NULL;
    if (ftruncate(fd, AS_TOTAL_SIZE) != 0) {
        close(fd);
        return NULL;
    }
    void *p = mmap(NULL, AS_TOTAL_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    return p == MAP_FAILED ? NULL : p;
}

static void as_unmap(void *p)
{
    if (p)
        munmap(p, AS_TOTAL_SIZE);
    shm_unlink(AS_POSIX_NAME);
}
#endif

void audio_share_init(void)
{
    if (as_hdr)
        return;
    void *p = as_map();
    if (!p)
        return;                       /* sharing is optional: the viewer works without it */
    as_hdr = (struct as_header *)p;
    as_ring = (uint8_t *)p + AS_HEADER_SIZE;
    memset(as_hdr, 0, AS_HEADER_SIZE);
    as_hdr->version = AS_VERSION;
    as_hdr->header_size = AS_HEADER_SIZE;
    as_hdr->sample_rate = RP_AUDIO_SAMPLE_RATE;
    as_hdr->channels = RP_AUDIO_CHANNELS;
    as_hdr->ring_frames = AS_RING_FRAMES;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    memcpy(as_hdr->magic, AS_MAGIC, 8);   /* written last: readers see a complete header */
}

void audio_share_close(void)
{
    if (!as_hdr)
        return;
    memset(as_hdr->magic, 0, 8);
    as_unmap(as_hdr);
    as_hdr = NULL;
    as_ring = NULL;
}

void audio_share_write(const uint8_t *pcm, int nframes)
{
    if (!as_hdr || !pcm || nframes <= 0)
        return;
    uint64_t w = __atomic_load_n(&as_hdr->write_count, __ATOMIC_RELAXED);
    while (nframes > 0) {
        uint32_t at = (uint32_t)(w % AS_RING_FRAMES);
        uint32_t n = AS_RING_FRAMES - at;
        if (n > (uint32_t)nframes)
            n = (uint32_t)nframes;
        memcpy(as_ring + (size_t)at * AS_FRAME_BYTES, pcm, (size_t)n * AS_FRAME_BYTES);
        pcm += (size_t)n * AS_FRAME_BYTES;
        nframes -= (int)n;
        w += n;
    }
    __atomic_store_n(&as_hdr->write_time_ms, as_now_ms(), __ATOMIC_RELAXED);
    __atomic_store_n(&as_hdr->write_count, w, __ATOMIC_RELEASE);   /* readers see the samples first */
}

void audio_share_new_stream(void)
{
    if (as_hdr)
        __atomic_add_fetch(&as_hdr->stream_id, 1, __ATOMIC_RELEASE);
}
