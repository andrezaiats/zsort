# zsort ⚡

**zsort** is a high-performance, parallel, in-memory sorter for modern Linux systems. It is designed to be a faster drop-in replacement for `LC_ALL=C sort` for datasets that fit in RAM, optimizing heavily for cache locality and instruction pipelining.

![License](https://img.shields.io/badge/license-MIT-blue.svg)
![Platform](https://img.shields.io/badge/platform-Linux-lightgrey.svg)
![Build](https://img.shields.io/badge/build-passing-brightgreen.svg)

## 🚀 Performance Benchmarks

Benchmarks were performed on an **88-core / 330 GB RAM** Linux machine.
**zsort** outperforms both the standard GNU `sort` and the commercial/industry-standard `nsort`.

### 🏆 10 GB Dataset (Gensort Format)
*Input: 100M lines, 100-byte records (generated via `gensort -a`).*

| Sorter | Time (Wall) | Throughput | vs zsort |
| :--- | :--- | :--- | :--- |
| **zsort** | **3.30s** | **~3.03 GB/s** | **--** |
| nsort | 9.52s | ~1.05 GB/s | 2.9x slower |
| GNU sort (parallel) | 55.4s | ~0.18 GB/s | 16.8x slower |
| GNU sort (default) | 62.1s | ~0.16 GB/s | 18.8x slower |

### ⚡ 100 MB Dataset (Short Strings)
*Input: 10M lines, fixed 10-char alphanumeric strings.*

| Sorter | Time (Wall) | vs zsort |
| :--- | :--- | :--- |
| **zsort** | **0.22s** | **--** |
| nsort | 0.87s | 4.0x slower |
| GNU sort (parallel) | 4.18s | 19x slower |

> *All benchmarks run with `LC_ALL=C`, outputting to `/dev/null` to measure pure sorting throughput without disk write bottlenecks.*

---

## 🧠 Why is it so fast?

zsort achieves this performance by minimizing CPU stalls and maximizing memory bandwidth:

1.  **Prefix Key Optimization:** Instead of dereferencing string pointers for every comparison (which causes cache misses), zsort stores the first 8 bytes of each line as a `uint64_t` directly in the sorting structure.
    * *Result:* 99% of comparisons happen in CPU registers.
    * *Benefit:* Eliminates pointer chasing and branch mispredictions.
2.  **Zero-Copy & Hugepages:** Uses `mmap` with `MADV_HUGEPAGE` to reduce TLB misses and avoid copying data from kernel to user space during the read phase.
3.  **Parallel Write:** Uses `pwrite()` with pre-calculated offsets to allow all threads to blast data to disk simultaneously, saturating I/O bandwidth without file descriptor locking.
4.  **Cache-Friendly Layout:** Data structures are aligned to cache lines to prevent false sharing between cores.

## 🛠️ Build & Installation

Requires GCC (with pthreads support) and Make.

```bash
git clone https://github.com/yourusername/zsort.git
cd zsort
make
```

## 💻 Usage

zsort reads from a file and writes to a file or stdout.

```bash
# Sort to a file
./zsort input.txt output.txt

# Sort to stdout
./zsort input.txt > output.txt

# Benchmark (sort-only, no disk I/O)
./zsort input.txt /dev/null
```

### Thread Count

By default, zsort uses one thread per **physical core** (detected from the
CPU sibling topology). The bandwidth-bound phases — the output gather and the
partition scatter — do not benefit from running a second thread per core via
hyperthreading; oversubscribing logical CPUs adds memory-system contention and
is measurably slower. You can override the default with the `ZSORT_THREADS`
environment variable:

```bash
# Force a specific thread count (e.g. to use all logical CPUs, or fewer)
ZSORT_THREADS=88 ./zsort input.txt output.txt
```

If the topology cannot be read, zsort falls back to the online CPU count.

## 📋 Input Format

- Any text file with `\n`-delimited lines
- Variable-length lines supported
- Binary content supported
- Files with or without trailing newline handled correctly

## 🔬 Algorithm

Parallel sample sort with 8-byte prefix keys across all available CPU cores.

### Record Layout (24 bytes)

```c
typedef struct {
    uint64_t key;    // first 8 bytes as big-endian uint64 (prefix compare)
    const char *ptr; // pointer to line start in mmap'd input
    uint32_t len;    // line length excluding newline
    uint32_t _pad;
} line_t;
```

### Phases

0. **mmap input** — `MAP_PRIVATE` with `MADV_SEQUENTIAL | MADV_HUGEPAGE`.

1. **Parallel line discovery** — Each thread scans its byte chunk for `\n`,
   counts lines, then a prefix sum assigns write offsets, and each thread
   builds `line_t` records into a shared array.

2. **Sample sort partitioning** — Thread 0 picks `num_buckets * 64` evenly-spaced
   samples, sorts them with introsort, and selects `num_buckets - 1` splitters.
   All threads classify their lines via binary search into per-thread histograms,
   a prefix sum computes global bucket boundaries, and all threads scatter records
   into a second array. `num_buckets = nthreads * 8` for good load balancing.

3. **Per-bucket introsort (work-stealing)** — An atomic counter distributes
   buckets. Each thread sorts in-place using introsort: Hoare partition with
   median-of-3 pivot, insertion sort at n <= 32, heapsort fallback at depth
   > 2*log2(n). All comparisons inlined (no function pointer overhead).

4. **Parallel output** — Each thread computes its output byte range via prefix
   sum, then copies sorted lines into an output buffer with software prefetching.

### Memory Usage (10 GB input, 100M lines)

| Allocation     | Size    |
|----------------|---------|
| Input (mmap)   | 10 GB   |
| Record array A | 2.4 GB  |
| Record array B | 2.4 GB  |
| Output buffer  | 10 GB   |
| **Total**      | ~25 GB  |

## ✅ Verification

```bash
# Small file
./zsort arquivo.txt out.txt
LC_ALL=C sort arquivo.txt > ref.txt
cmp out.txt ref.txt && echo "CORRECT"

# 10 GB file
./zsort dataset_10gb.txt out.txt
LC_ALL=C sort dataset_10gb.txt > ref.txt
cmp out.txt ref.txt && echo "CORRECT"
```

## 📄 License

MIT — see [LICENSE](LICENSE) for details.
