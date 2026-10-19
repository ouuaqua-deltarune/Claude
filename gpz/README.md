# gpz

A block compressor that runs on the CPU or entirely on a GPU.

```
make            # needs only a C99 compiler (+ -ldl on Linux)
./gpz c file            # -> file.gpz   (GPU if one is found, else CPU)
./gpz d file.gpz        # -> file
./gpz -m c file         # MAX RATIO: context mixing + zip/gzip recompression (CPU, slow)
./gpz -u c file         # fast mode with lazy matching, a little smaller, slower
./gpz -g c file         # require GPU      ./gpz -c c file   # force CPU
./gpz list              # show OpenCL devices
sh test.sh              # CPU/GPU cross round-trip tests
```

## How it works
- **Format (GPZ1):** 20-byte header (magic, block size, length, CRC-32), a 4-byte
  table entry per block, then the blocks. Blocks are coded independently
  (default 64 KiB); a block that does not shrink is stored raw.
- **Codec:** LZ77 (4-way hash buckets, optional lazy matching) + an LZMA-style
  adaptive binary range coder. Integer-only.
- **GPU:** `codec.h` is compiled both as C and as OpenCL C, so the CPU and GPU
  paths give **byte-identical** output (checked by `test.sh`). One work-item
  handles one block, for both compression and decompression, so the GPU does
  the whole job; the host only moves bytes.
- **AMD / NVIDIA / Intel:** via OpenCL, loaded with `dlopen` at run time. No
  vendor SDK is needed to build, and the same binary runs without a GPU
  (it falls back to the CPU).
- **Old architectures:** C99, no SIMD, no unaligned access, byte-wise
  little-endian I/O, 32-bit arithmetic apart from the range coder's 64-bit
  `low` (emulated on 32-bit CPUs). Kernels use only OpenCL 1.0 features.

## Max-ratio mode (`-m`, format GPZ2)
A context-mixing compressor (same family as lpaq/paq): orders 0-6 and 8, word,
sparse, column and match models, each with fast+slow counters, blended by two
gated mixers and three SSE stages, then a binary arithmetic coder. Integer-only,
so it is deterministic across machines. `d` detects GPZ1/GPZ2 automatically.

| input | original | gpz -m | xz -9 | gzip -9 |
|---|---|---|---|---|
| source code (55 KB) | 54,718 | **13,440** | 15,616 | 16,898 |
| license texts (303 KB) | 303,076 | **34,889** | 43,748 | - |
| binaries (1.5 MB) | 1,500,000 | **535,341** | 606,224 | 728,998 |
| gzip file from Python (165 KB) | 164,564 | **29,127** | 125,896 | - |

**Undeflate:** gzip members and zip entries are inflated, and if zlib (level
1-9, memLevel 8/9) can regenerate the *exact original bytes*, the stream is
stored as raw data + settings, which CM compresses far better. Restoring is
byte-identical and checked by CRC. Streams that cannot be reproduced stay as-is.
- Works for files made with zlib (Python, Java, git, most libraries, many zip
  tools). **Does not work** for GNU `gzip`, 7-Zip and some Info-ZIP entries, which
  use their own deflate encoder; those are stored unchanged (about 100%). Handling
  them needs a preflate-style model of the encoder, which is not implemented.
- Restoring needs a zlib that produces the same bytes as the one used to
  compress. Mainstream zlib 1.2.x/1.3.x do; zlib-ng and some vendor builds may
  not. If they differ, gpz stops with an error instead of writing wrong data,
  but the archive then can only be opened on a matching zlib. Use `-n` to avoid
  this dependency.

Cost: about 0.25 MB/s, CPU only (context mixing is sequential, so it does not
map onto the GPU), roughly 150 MB of RAM. It is for archiving, not speed.

## GPU parallelism and memory
Fast mode runs one GPU thread per block and sizes its batches from the device's
real memory, so it works on any OpenCL GPU, and is aimed at GPUs with 4 GB or
more of VRAM / unified memory:
- **Dedicated GPU:** uses up to 70% of VRAM for working buffers.
- **Unified memory (Apple Silicon, integrated):** uses a quarter of the shared RAM
  (at most 4 GiB), since the OS needs the rest.
- **Allocation failures:** it halves the batch until the driver accepts it.
- **Watchdog-safe:** work is split into short launches whose size adapts to how
  long each takes, so slow GPUs do not trip the driver's timeout.
- **Fallback:** if the GPU fails at any point, the remaining work finishes on the
  CPU with identical output (use `-g` to make that an error instead).
- Smaller GPUs (< 4 GB) work too, just with smaller batches.

`-v` prints the device memory, the budget used, launches and kernel time;
`gpz list` shows each device, its memory type and max buffer. For more threads on
small files use `-b 14` (16 KB blocks): more parallelism, text compresses ~20% worse.

CUDA: NVIDIA GPUs are driven through their OpenCL driver (same cores). A native
CUDA backend is not included; I could not build or test one.
Max-ratio mode is CPU-only: each model needs ~150 MB, which cannot be
replicated across thousands of GPU threads.

## Honest limits
- **Fast mode (default) is not "ultra small":** roughly gzip -9 sized on text.
  Use `-m` for the best ratio.
- **Only tested on a CPU OpenCL runtime (pocl) and plain CPU.** I had no real
  AMD or NVIDIA GPU, so GPU speed is unmeasured. One work-item per 64 KiB
  block is serial per block, so a GPU may be *slower* than the CPU on small
  inputs; it needs many blocks (large files) to pay off.
- The whole file is held in memory; inputs are limited to 4 GiB (less on 32-bit).
- Real GPUs are selected by default; `GPZ_ANY_DEVICE=1` also allows CPU OpenCL
  devices (used by the tests).
- Build uses `sed` to embed the kernel source; some compilers (e.g. MSVC)
  limit string literal length, so Windows builds may need a different embed step.
