#!/bin/sh
# Round-trips several inputs through every CPU/GPU combination.
# GPU legs are skipped if no OpenCL GPU is present (use GPZ_TEST_GPU=1 to require them).
set -e
# lets the GPU code path run on a CPU OpenCL runtime (e.g. pocl) when no real GPU exists
[ "${GPZ_TEST_GPU:-0}" = 1 ] || export GPZ_ANY_DEVICE=1
cd "$(dirname "$0")"
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
: > $T/empty
printf 'a' > $T/one
head -c 300000 /dev/urandom > $T/random
head -c 400000 /dev/zero > $T/zeros
for i in $(seq 1 4000); do echo "line $i: the quick brown fox jumps over the lazy dog $((i*7919))"; done > $T/text
cat gpz.c codec.h gpz > $T/mixed
GPU=0
if ./gpz -g c $T/one $T/probe >/dev/null 2>&1; then GPU=1; fi
[ "${GPZ_TEST_GPU:-0}" = 1 ] && [ $GPU = 0 ] && { echo "no GPU available"; exit 1; }
fail=0
for f in empty one random zeros text mixed; do
  for u in "" -u; do
    ./gpz -c $u c $T/$f $T/$f.cpu 2>/dev/null
    modes="c"; [ $GPU = 1 ] && modes="c g"
    for m in $modes; do
      if [ $m = g ]; then
        ./gpz -g $u c $T/$f $T/$f.gpu 2>/dev/null
        cmp -s $T/$f.cpu $T/$f.gpu || { echo "FAIL $f $u: CPU and GPU compressed output differ"; fail=1; }
      fi
      for d in c g; do
        [ $d = g ] && [ $GPU = 0 ] && continue
        rm -f $T/$f.rt
        ./gpz -$d d $T/$f.$([ $m = g ] && echo gpu || echo cpu) $T/$f.rt 2>/dev/null
        cmp -s $T/$f $T/$f.rt || { echo "FAIL $f $u: compress=$m decompress=$d"; fail=1; }
      done
    done
  done
  echo "ok $f ($(wc -c < $T/$f) -> $(wc -c < $T/$f.cpu) bytes)"
done
# corruption must be detected, not crash
./gpz -c c $T/text $T/bad.gpz 2>/dev/null
printf 'X' | dd of=$T/bad.gpz bs=1 seek=5000 conv=notrunc 2>/dev/null
if ./gpz -c d $T/bad.gpz $T/bad.out 2>/dev/null; then echo "FAIL corruption undetected"; fail=1; else echo "ok corruption detected"; fi
[ $GPU = 1 ] && echo "GPU legs: ran" || echo "GPU legs: skipped (no OpenCL GPU)"
exit $fail
