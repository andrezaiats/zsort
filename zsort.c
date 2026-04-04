#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

/* ── Constants ─────────────────────────────────────────────────────── */

#define ISORT_THRESH    32

/* ── Record: 24 bytes ──────────────────────────────────────────────── */

typedef struct {
    uint64_t    key;    /* first 8 bytes as big-endian uint64 (prefix compare) */
    const char *ptr;    /* pointer to line start in mmap'd input */
    uint32_t    len;    /* line length excluding newline */
    uint32_t    _pad;
} line_t;

/* ── Inline comparison ─────────────────────────────────────────────── */

static inline int line_cmp(const line_t *a, const line_t *b)
{
    if (a->key != b->key)
        return (a->key < b->key) ? -1 : 1;
    uint32_t min_len = a->len < b->len ? a->len : b->len;
    int r = memcmp(a->ptr, b->ptr, min_len);
    if (r != 0) return r;
    return (a->len > b->len) - (a->len < b->len);
}

/* ── Build prefix key (first 8 bytes as big-endian uint64) ─────────── */

static inline uint64_t make_key(const char *ptr, uint32_t len)
{
    uint64_t k;
    const unsigned char *s = (const unsigned char *)ptr;
    if (len >= 8) {
        memcpy(&k, s, 8);
    } else {
        unsigned char buf[8] = {0};
        memcpy(buf, s, len);
        memcpy(&k, buf, 8);
    }
    /* Convert to big-endian for correct byte-order comparison */
    return __builtin_bswap64(k);
}

/* ── Shared state ──────────────────────────────────────────────────── */

static int              nthreads;
static pthread_t       *threads;
static pthread_barrier_t barrier;

static const char      *input_map;
static size_t           input_size;
static int              input_has_trailing_nl;
static size_t           total_lines;
static line_t          *records_a;
static line_t          *records_b;

/* Phase 1: line discovery */
static size_t          *thread_line_counts;
static size_t          *thread_line_offsets;

/* Phase 2: sample sort */
static int              num_buckets;
static line_t          *splitters;
static size_t          *bucket_sizes;
static size_t          *bucket_starts;
static size_t         **thread_bucket_counts;
static size_t         **thread_bucket_offsets;

/* Phase 3: work-stealing */
static volatile int     bucket_counter;

/* Output */
static char            *output_buf;
static size_t           output_size;
static int              out_fd;
static int              use_mmap_output;

/* Per-thread scan boundaries (computed from byte chunks) */
typedef struct {
    int    tid;
    size_t scan_start;
    size_t scan_end;
} thread_arg_t;

static thread_arg_t   *targs;

/* ── Introsort implementation ──────────────────────────────────────── */

static void insertion_sort(line_t *arr, size_t n)
{
    for (size_t i = 1; i < n; i++) {
        line_t tmp = arr[i];
        size_t j = i;
        while (j > 0 && line_cmp(&tmp, &arr[j - 1]) < 0) {
            arr[j] = arr[j - 1];
            j--;
        }
        arr[j] = tmp;
    }
}

static inline void swap_line(line_t *a, line_t *b)
{
    line_t tmp = *a;
    *a = *b;
    *b = tmp;
}

static void heapsort_siftdown(line_t *arr, size_t start, size_t end)
{
    size_t root = start;
    while (root * 2 + 1 <= end) {
        size_t child = root * 2 + 1;
        if (child + 1 <= end && line_cmp(&arr[child], &arr[child + 1]) < 0)
            child++;
        if (line_cmp(&arr[root], &arr[child]) < 0) {
            swap_line(&arr[root], &arr[child]);
            root = child;
        } else {
            return;
        }
    }
}

static void heapsort(line_t *arr, size_t n)
{
    if (n < 2) return;
    for (size_t i = n / 2; i > 0; i--)
        heapsort_siftdown(arr, i - 1, n - 1);
    for (size_t end = n - 1; end > 0; end--) {
        swap_line(&arr[0], &arr[end]);
        heapsort_siftdown(arr, 0, end - 1);
    }
}

static inline line_t median_of_three(line_t *a, line_t *b, line_t *c)
{
    if (line_cmp(a, b) < 0) {
        if (line_cmp(b, c) < 0) return *b;
        else if (line_cmp(a, c) < 0) return *c;
        else return *a;
    } else {
        if (line_cmp(a, c) < 0) return *a;
        else if (line_cmp(b, c) < 0) return *c;
        else return *b;
    }
}

static void introsort_impl(line_t *arr, size_t n, int depth_limit)
{
    while (n > ISORT_THRESH) {
        if (depth_limit == 0) {
            heapsort(arr, n);
            return;
        }
        depth_limit--;

        line_t pivot = median_of_three(&arr[0], &arr[n / 2], &arr[n - 1]);

        /* Hoare partition */
        size_t i = 0, j = n - 1;
        for (;;) {
            while (line_cmp(&arr[i], &pivot) < 0) i++;
            while (line_cmp(&arr[j], &pivot) > 0) j--;
            if (i >= j) break;
            swap_line(&arr[i], &arr[j]);
            i++;
            j--;
        }

        size_t left_size = j + 1;
        size_t right_size = n - left_size;
        if (left_size < right_size) {
            introsort_impl(arr, left_size, depth_limit);
            arr += left_size;
            n = right_size;
        } else {
            introsort_impl(arr + left_size, right_size, depth_limit);
            n = left_size;
        }
    }
    insertion_sort(arr, n);
}

static void introsort(line_t *arr, size_t n)
{
    if (n < 2) return;
    int depth = 0;
    size_t tmp = n;
    while (tmp > 1) { depth++; tmp >>= 1; }
    depth *= 2;
    introsort_impl(arr, n, depth);
}

/* ── Binary search for bucket index ────────────────────────────────── */

static inline int find_bucket(const line_t *rec)
{
    int lo = 0, hi = num_buckets - 2;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        if (line_cmp(rec, &splitters[mid]) <= 0)
            hi = mid - 1;
        else
            lo = mid + 1;
    }
    return lo;
}

/* ── Thread entry point ────────────────────────────────────────────── */

static void *thread_func(void *arg)
{
    thread_arg_t *ta = (thread_arg_t *)arg;
    int tid = ta->tid;
    size_t scan_start = ta->scan_start;
    size_t scan_end = ta->scan_end;

    /* ── Phase 1a: Count lines in this thread's region ── */
    size_t my_count = 0;
    if (scan_start < scan_end) {
        const char *p = input_map + scan_start;
        const char *end = input_map + scan_end;
        while (p < end) {
            const char *nl = memchr(p, '\n', (size_t)(end - p));
            if (nl) {
                my_count++;
                p = nl + 1;
            } else {
                /* Last thread, trailing line with no newline */
                if (tid == nthreads - 1)
                    my_count++;
                break;
            }
        }
    }
    thread_line_counts[tid] = my_count;
    pthread_barrier_wait(&barrier);

    /* ── Phase 1b: Prefix sum + allocate records (thread 0) ── */
    if (tid == 0) {
        size_t sum = 0;
        for (int t = 0; t < nthreads; t++) {
            thread_line_offsets[t] = sum;
            sum += thread_line_counts[t];
        }
        total_lines = sum;

        /* Allocate record arrays */
        size_t rec_size = total_lines * sizeof(line_t);
        if (rec_size > 0) {
            records_a = (line_t *)mmap(NULL, rec_size, PROT_READ | PROT_WRITE,
                                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            records_b = (line_t *)mmap(NULL, rec_size, PROT_READ | PROT_WRITE,
                                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (records_a == MAP_FAILED || records_b == MAP_FAILED) {
                perror("mmap records");
                _exit(1);
            }
            madvise(records_a, rec_size, MADV_HUGEPAGE);
            madvise(records_b, rec_size, MADV_HUGEPAGE);
        }
    }
    pthread_barrier_wait(&barrier);

    if (total_lines == 0)
        goto done;

    /* ── Phase 1c: Build line_t records ── */
    {
        size_t write_idx = thread_line_offsets[tid];
        const char *p = input_map + scan_start;
        const char *end = input_map + scan_end;
        while (p < end) {
            const char *line_start = p;
            const char *nl = memchr(p, '\n', (size_t)(end - p));
            uint32_t len;
            if (nl) {
                len = (uint32_t)(nl - line_start);
                p = nl + 1;
            } else {
                len = (uint32_t)(end - line_start);
                p = end;
            }
            records_a[write_idx].key = make_key(line_start, len);
            records_a[write_idx].ptr = line_start;
            records_a[write_idx].len = len;
            records_a[write_idx]._pad = 0;
            write_idx++;
        }
    }
    pthread_barrier_wait(&barrier);

    /* ── Phase 2: Sample Sort Partitioning ── */

    /* Phase 2a: Sampling (thread 0) */
    if (tid == 0) {
        if (total_lines <= (size_t)(nthreads * 64) || num_buckets <= 1) {
            num_buckets = 1;
        } else {
            size_t num_samples = (size_t)num_buckets * 64;
            if (num_samples > total_lines)
                num_samples = total_lines;

            line_t *samples = (line_t *)malloc(num_samples * sizeof(line_t));
            size_t step = total_lines / num_samples;
            if (step == 0) step = 1;
            for (size_t i = 0; i < num_samples; i++)
                samples[i] = records_a[i * step];

            introsort(samples, num_samples);

            splitters = (line_t *)malloc((size_t)(num_buckets - 1) * sizeof(line_t));
            for (int i = 0; i < num_buckets - 1; i++) {
                size_t idx = (size_t)(i + 1) * num_samples / (size_t)num_buckets;
                if (idx >= num_samples) idx = num_samples - 1;
                splitters[i] = samples[idx];
            }
            free(samples);
        }
    }
    pthread_barrier_wait(&barrier);

    if (num_buckets <= 1) {
        /* Small input or single bucket: sort records_a directly */
        if (tid == 0 && total_lines > 1)
            introsort(records_a, total_lines);
        pthread_barrier_wait(&barrier);
        goto output_phase;
    }

    /* Phase 2b: Each thread classifies its share of lines into buckets */
    {
        size_t per = total_lines / (size_t)nthreads;
        size_t rem = total_lines % (size_t)nthreads;
        size_t my_start = (size_t)tid * per + ((size_t)tid < rem ? (size_t)tid : rem);
        size_t my_count2 = per + ((size_t)tid < rem ? 1 : 0);
        size_t my_end = my_start + my_count2;

        memset(thread_bucket_counts[tid], 0, (size_t)num_buckets * sizeof(size_t));
        for (size_t i = my_start; i < my_end; i++)
            thread_bucket_counts[tid][find_bucket(&records_a[i])]++;

        pthread_barrier_wait(&barrier);

        /* Phase 2c: Prefix sum (thread 0) */
        if (tid == 0) {
            memset(bucket_sizes, 0, (size_t)num_buckets * sizeof(size_t));
            for (int t = 0; t < nthreads; t++)
                for (int b = 0; b < num_buckets; b++)
                    bucket_sizes[b] += thread_bucket_counts[t][b];

            size_t s = 0;
            for (int b = 0; b < num_buckets; b++) {
                bucket_starts[b] = s;
                s += bucket_sizes[b];
            }

            for (int b = 0; b < num_buckets; b++) {
                size_t off = bucket_starts[b];
                for (int t = 0; t < nthreads; t++) {
                    thread_bucket_offsets[t][b] = off;
                    off += thread_bucket_counts[t][b];
                }
            }
            bucket_counter = 0;
        }
        pthread_barrier_wait(&barrier);

        /* Phase 2d: Scatter records_a → records_b */
        for (size_t i = my_start; i < my_end; i++) {
            int b = find_bucket(&records_a[i]);
            records_b[thread_bucket_offsets[tid][b]++] = records_a[i];
        }
    }
    pthread_barrier_wait(&barrier);

    /* ── Phase 3: Per-bucket sort (work-stealing) ── */
    for (;;) {
        int b = __sync_fetch_and_add(&bucket_counter, 1);
        if (b >= num_buckets) break;
        size_t bs = bucket_starts[b];
        size_t bn = bucket_sizes[b];
        if (bn > 1)
            introsort(records_b + bs, bn);
    }
    pthread_barrier_wait(&barrier);

output_phase:;
    /* ── Phase 4: Parallel output ── */
    line_t *sorted = (num_buckets <= 1) ? records_a : records_b;

    /* Each thread computes total output bytes for its chunk of lines */
    {
        size_t per = total_lines / (size_t)nthreads;
        size_t rem = total_lines % (size_t)nthreads;
        size_t my_start = (size_t)tid * per + ((size_t)tid < rem ? (size_t)tid : rem);
        size_t my_count3 = per + ((size_t)tid < rem ? 1 : 0);
        size_t my_end = my_start + my_count3;

        size_t my_bytes = 0;
        for (size_t i = my_start; i < my_end; i++)
            my_bytes += sorted[i].len + 1;
        if (!input_has_trailing_nl && my_end == total_lines && my_count3 > 0)
            my_bytes--;
        thread_line_counts[tid] = my_bytes;
    }
    pthread_barrier_wait(&barrier);

    /* Thread 0: prefix sum + setup */
    if (tid == 0) {
        size_t s = 0;
        for (int t = 0; t < nthreads; t++) {
            thread_line_offsets[t] = s;
            s += thread_line_counts[t];
        }
        output_size = s;

        if (output_size > 0) {
            output_buf = (char *)mmap(NULL, output_size, PROT_READ | PROT_WRITE,
                                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (output_buf == MAP_FAILED) {
                perror("mmap output buf"); _exit(1);
            }
            madvise(output_buf, output_size, MADV_HUGEPAGE);
        }
    }
    pthread_barrier_wait(&barrier);

    if (output_size == 0) goto done;

    /* Parallel fill of output buffer */
    {
        size_t per = total_lines / (size_t)nthreads;
        size_t rem = total_lines % (size_t)nthreads;
        size_t my_start = (size_t)tid * per + ((size_t)tid < rem ? (size_t)tid : rem);
        size_t my_count4 = per + ((size_t)tid < rem ? 1 : 0);
        size_t my_end = my_start + my_count4;

        size_t out_pos = thread_line_offsets[tid];
        for (size_t i = my_start; i < my_end; i++) {
            if (i + 4 < my_end)
                __builtin_prefetch(sorted[i + 4].ptr, 0, 0);
            uint32_t len = sorted[i].len;
            memcpy(output_buf + out_pos, sorted[i].ptr, len);
            out_pos += len;
            if (i < total_lines - 1 || input_has_trailing_nl) {
                output_buf[out_pos] = '\n';
                out_pos++;
            }
        }
    }

done:
    pthread_barrier_wait(&barrier);
    return NULL;
}

/* ── Main ──────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s input [output]\n", argv[0]);
        return 1;
    }

    {
        const char *env = getenv("ZSORT_THREADS");
        if (env && atoi(env) > 0) {
            nthreads = atoi(env);
        } else {
            nthreads = (int)sysconf(_SC_NPROCESSORS_ONLN);
        }
        if (nthreads < 1) nthreads = 1;
    }

    /* Open and mmap input */
    int in_fd = open(argv[1], O_RDONLY);
    if (in_fd < 0) { perror("open input"); return 1; }

    struct stat st;
    if (fstat(in_fd, &st) < 0) { perror("fstat"); return 1; }
    input_size = (size_t)st.st_size;

    if (input_size == 0) {
        close(in_fd);
        if (argc >= 3) {
            int fd = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) close(fd);
        }
        return 0;
    }

    input_map = (const char *)mmap(NULL, input_size, PROT_READ,
                                    MAP_PRIVATE, in_fd, 0);
    if (input_map == MAP_FAILED) { perror("mmap input"); return 1; }
    madvise((void *)input_map, input_size, MADV_SEQUENTIAL);
    madvise((void *)input_map, input_size, MADV_HUGEPAGE);
    close(in_fd);

    input_has_trailing_nl = (input_map[input_size - 1] == '\n');

    /* Output setup */
    use_mmap_output = 0;
    output_buf = NULL;
    output_size = 0;
    if (argc >= 3) {
        out_fd = open(argv[2], O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (out_fd < 0) { perror("open output"); return 1; }
        struct stat out_st;
        if (fstat(out_fd, &out_st) == 0 && S_ISREG(out_st.st_mode))
            use_mmap_output = 1;
    } else {
        out_fd = STDOUT_FILENO;
    }

    /* Bucket count */
    num_buckets = nthreads * 8;
    if (num_buckets < 8) num_buckets = 8;

    /* Allocate per-thread structures */
    thread_line_counts = (size_t *)calloc((size_t)nthreads, sizeof(size_t));
    thread_line_offsets = (size_t *)calloc((size_t)nthreads, sizeof(size_t));
    targs = (thread_arg_t *)calloc((size_t)nthreads, sizeof(thread_arg_t));
    threads = (pthread_t *)calloc((size_t)nthreads, sizeof(pthread_t));
    splitters = NULL;
    bucket_sizes = (size_t *)calloc((size_t)num_buckets, sizeof(size_t));
    bucket_starts = (size_t *)calloc((size_t)(num_buckets + 1), sizeof(size_t));
    thread_bucket_counts = (size_t **)malloc((size_t)nthreads * sizeof(size_t *));
    thread_bucket_offsets = (size_t **)malloc((size_t)nthreads * sizeof(size_t *));
    for (int t = 0; t < nthreads; t++) {
        thread_bucket_counts[t] = (size_t *)calloc((size_t)num_buckets, sizeof(size_t));
        thread_bucket_offsets[t] = (size_t *)calloc((size_t)num_buckets, sizeof(size_t));
    }

    pthread_barrier_init(&barrier, NULL, (unsigned)nthreads);

    /* Compute per-thread scan boundaries: each thread owns lines whose '\n'
       falls in their byte chunk. Thread t>0 starts after the first '\n' at
       or after its chunk boundary. */
    size_t bytes_per = input_size / (size_t)nthreads;
    for (int t = 0; t < nthreads; t++) {
        size_t chunk_start = (size_t)t * bytes_per;
        size_t chunk_end = (t == nthreads - 1) ? input_size : (size_t)(t + 1) * bytes_per;

        size_t ss = chunk_start;
        if (t > 0) {
            while (ss < input_size && input_map[ss] != '\n')
                ss++;
            if (ss < input_size) ss++;
        }

        size_t se = chunk_end;
        if (t < nthreads - 1) {
            while (se < input_size && input_map[se] != '\n')
                se++;
            if (se < input_size) se++;
        } else {
            se = input_size;
        }

        targs[t].tid = t;
        targs[t].scan_start = ss;
        targs[t].scan_end = se;
    }

    /* Launch threads */
    for (int t = 1; t < nthreads; t++)
        pthread_create(&threads[t], NULL, thread_func, &targs[t]);
    thread_func(&targs[0]);
    for (int t = 1; t < nthreads; t++)
        pthread_join(threads[t], NULL);

    /* Write output buffer to fd */
    if (output_size > 0 && output_buf && output_buf != MAP_FAILED) {
        size_t written = 0;
        while (written < output_size) {
            size_t chunk = output_size - written;
            if (chunk > 256 * 1024 * 1024) chunk = 256 * 1024 * 1024;
            ssize_t w = write(out_fd, output_buf + written, chunk);
            if (w < 0) {
                if (errno == EINTR) continue;
                perror("write"); break;
            }
            written += (size_t)w;
        }
        munmap(output_buf, output_size);
    }
    if (out_fd != STDOUT_FILENO) close(out_fd);

    /* Cleanup — process is exiting, OS reclaims everything */
    munmap((void *)input_map, input_size);
    free(thread_line_counts);
    free(thread_line_offsets);
    free(targs);
    free(threads);
    free(splitters);
    free(bucket_sizes);
    free(bucket_starts);
    for (int t = 0; t < nthreads; t++) {
        free(thread_bucket_counts[t]);
        free(thread_bucket_offsets[t]);
    }
    free(thread_bucket_counts);
    free(thread_bucket_offsets);
    pthread_barrier_destroy(&barrier);

    return 0;
}
