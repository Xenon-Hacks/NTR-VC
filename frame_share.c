/* frame_share.c -- see frame_share.h */
#include "frame_share.h"

#include <stddef.h>
#include <string.h>

#define FS_MAGIC "NTRFRAME"
#define FS_VERSION 1u
#define FS_HEADER_SIZE 64u
#define FS_MAX_PIXELS (240u * 800u)
#define FS_TOTAL_SIZE (FS_HEADER_SIZE + FS_MAX_PIXELS * 4u)

struct fs_header {
    char magic[8];
    uint32_t version;
    uint32_t header_size;
    uint32_t seq;
    uint32_t frame_index;
    uint32_t width;
    uint32_t height;
    uint32_t channels;
    uint32_t is_wide;
    uint64_t frame_time_ms;
    int32_t quality_x10;
    uint32_t reserved;
    uint64_t quality_time_ms;
};
_Static_assert(sizeof(struct fs_header) == FS_HEADER_SIZE, "frame_share header must be 64 bytes");

static struct fs_header *fs_hdr;
static uint8_t *fs_pixels;

#ifdef _WIN32
#include <windows.h>
static HANDLE fs_mapping;

static uint64_t fs_now_ms(void) { return (uint64_t)GetTickCount64(); }

static void *fs_map(void)
{
    fs_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                    0, FS_TOTAL_SIZE, L"Local\\NTRViewerHR_TopScreen");
    if (!fs_mapping)
        return NULL;
    void *p = MapViewOfFile(fs_mapping, FILE_MAP_ALL_ACCESS, 0, 0, FS_TOTAL_SIZE);
    if (!p) {
        CloseHandle(fs_mapping);
        fs_mapping = NULL;
    }
    return p;
}

static void fs_unmap(void *p)
{
    if (p)
        UnmapViewOfFile(p);
    if (fs_mapping)
        CloseHandle(fs_mapping);
    fs_mapping = NULL;
}
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#define FS_POSIX_NAME "/ntrviewer_hr_top_screen"

static uint64_t fs_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

static void *fs_map(void)
{
    int fd = shm_open(FS_POSIX_NAME, O_CREAT | O_RDWR, 0600);
    if (fd < 0)
        return NULL;
    if (ftruncate(fd, FS_TOTAL_SIZE) != 0) {
        close(fd);
        return NULL;
    }
    void *p = mmap(NULL, FS_TOTAL_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    return p == MAP_FAILED ? NULL : p;
}

static void fs_unmap(void *p)
{
    if (p)
        munmap(p, FS_TOTAL_SIZE);
    shm_unlink(FS_POSIX_NAME);
}
#endif

void frame_share_init(void)
{
    if (fs_hdr)
        return;
    void *p = fs_map();
    if (!p)
        return;                       /* sharing is optional: the viewer works without it */
    fs_hdr = (struct fs_header *)p;
    fs_pixels = (uint8_t *)p + FS_HEADER_SIZE;
    memset(fs_hdr, 0, FS_HEADER_SIZE);
    fs_hdr->version = FS_VERSION;
    fs_hdr->header_size = FS_HEADER_SIZE;
    fs_hdr->channels = 4;
    fs_hdr->quality_x10 = -1;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    memcpy(fs_hdr->magic, FS_MAGIC, 8);   /* written last: readers see a complete header */
}

void frame_share_close(void)
{
    if (!fs_hdr)
        return;
    memset(fs_hdr->magic, 0, 8);
    fs_unmap(fs_hdr);
    fs_hdr = NULL;
    fs_pixels = NULL;
}

/* Called with the screen's status lock held, so frames are published one
   at a time and in the same order the viewer displays them. */
void frame_share_publish(const uint8_t *bgra, int width, int height, int is_wide)
{
    if (!fs_hdr || !bgra || width <= 0 || height <= 0 ||
        (unsigned)width * (unsigned)height > FS_MAX_PIXELS)
        return;
    uint32_t seq = __atomic_load_n(&fs_hdr->seq, __ATOMIC_RELAXED);
    __atomic_store_n(&fs_hdr->seq, seq + 1, __ATOMIC_RELAXED);    /* odd: writing */
    __atomic_thread_fence(__ATOMIC_RELEASE);
    memcpy(fs_pixels, bgra, (size_t)width * (size_t)height * 4u);
    fs_hdr->width = (uint32_t)width;
    fs_hdr->height = (uint32_t)height;
    fs_hdr->is_wide = (uint32_t)is_wide;
    fs_hdr->frame_index += 1;
    fs_hdr->frame_time_ms = fs_now_ms();
    __atomic_store_n(&fs_hdr->seq, seq + 2, __ATOMIC_RELEASE);    /* even: complete */
}

void frame_share_set_quality(double percent)
{
    if (!fs_hdr)
        return;
    int32_t q = percent < 0 ? -1 : (int32_t)(percent * 10.0 + 0.5);
    __atomic_store_n(&fs_hdr->quality_x10, q, __ATOMIC_RELAXED);
    __atomic_store_n(&fs_hdr->quality_time_ms, fs_now_ms(), __ATOMIC_RELEASE);
}
