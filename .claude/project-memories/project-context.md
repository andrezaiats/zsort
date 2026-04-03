---
name: project-context
description: Auto-scanned project stack, structure, and tooling configuration
type: project
source: scan-project
scanned_at: 2026-04-03T00:00:00Z
stale_after_inactive_days: 30
---

### Stack & Versions
- Language: C (GNU C with `_GNU_SOURCE`)
- Compiler: GCC with pthreads
- Build flags: `-O3 -march=native -flto -pthread -Wall -Wextra`
- No external dependencies beyond libc and pthreads
- License: MIT

### Project Structure
- `zsort.c` — single-file implementation (614 lines), the entire sorter
- `Makefile` — builds `zsort` binary from `zsort.c`
- `wolfsort-src/` — vendored copy of wolfsort (comparison/benchmark tool)
- `nsort`, `sorter`, `wolfsort` — prebuilt benchmark comparison binaries
- `arquivo.txt`, `dataset_10gb.txt` — test/benchmark input files
- `gensort-linux-1.5.tar.gz` — gensort tool for generating benchmark data
- `32/`, `64/` — unknown directories (likely gensort platform binaries)

### Build & Run
- `make` — compiles `zsort` from `zsort.c`
- `make clean` — removes the `zsort` binary
- `./zsort input.txt output.txt` — sort input file to output file
- `./zsort input.txt > output.txt` — sort to stdout
- `./zsort input.txt /dev/null` — benchmark mode (no disk write)
- Verification: `LC_ALL=C sort input.txt > ref.txt && cmp out.txt ref.txt`

### Architecture & Design
- Parallel sample sort with 8-byte prefix keys across all CPU cores
- 24-byte record struct: `uint64_t key` (prefix), `const char *ptr`, `uint32_t len`, padding
- Phase 0: `mmap` input with `MAP_PRIVATE`, `MADV_SEQUENTIAL | MADV_HUGEPAGE`
- Phase 1: parallel line discovery — threads scan byte chunks for `\n`, prefix sum assigns offsets
- Phase 2: sample sort partitioning — sample `num_buckets * 64` keys, introsort samples, select splitters, scatter via binary search + histograms; `num_buckets = nthreads * 8`
- Phase 3: per-bucket introsort with work-stealing (atomic counter); Hoare partition, median-of-3 pivot, insertion sort at n<=32, heapsort fallback at depth > 2*log2(n)
- Phase 4: parallel output via `pwrite()` with pre-calculated offsets and software prefetching
- Prefix key optimization: first 8 bytes stored as big-endian `uint64_t` for register-only comparisons (~99% of cases)
- Memory: ~2.5x input size (mmap + 2 record arrays + output buffer)

### Known Limitations & Plans
- Designed for datasets that fit in RAM (in-memory sort only)
- Requires Linux (uses `mmap`, `pwrite`, `MADV_HUGEPAGE`, pthreads barriers)
- No CI/CD pipeline configured

_Re-run `/scan-project` after significant changes to stack, structure, or tooling._
