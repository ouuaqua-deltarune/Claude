# gpz

A block compressor that runs on the CPU or entirely on a GPU.

```
make            # needs only a C99 compiler (+ -ldl on Linux)
./gpz c file            # -> file.gpz   (GPU if one is found, else CPU)
./gpz d file.gpz        # -> file
./gpz -u c file         # "ultra": lazy matching, a little smaller, slower
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

## Honest limits
- **Not "ultra small".** On text it is roughly gzip -9 sized and well behind
  `xz -9` (which uses optimal parsing, repeat-match codes and richer contexts).
  Larger blocks did not help; the match finder is the limit. This is a
  working, fast-to-parallelise base, not a record-setting ratio.
- **Only tested on a CPU OpenCL runtime (pocl) and plain CPU.** I had no real
  AMD or NVIDIA GPU, so GPU speed is unmeasured. One work-item per 64 KiB
  block is serial per block, so a GPU may be *slower* than the CPU on small
  inputs; it needs many blocks (large files) to pay off.
- The whole file is held in memory; inputs are limited to 4 GiB (less on 32-bit).
- Real GPUs are selected by default; `GPZ_ANY_DEVICE=1` also allows CPU OpenCL
  devices (used by the tests).
- Build uses `sed` to embed the kernel source; some compilers (e.g. MSVC)
  limit string literal length, so Windows builds may need a different embed step.
