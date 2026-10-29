#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# pool off against pool on at 48000 Hz, 128, 256 and 512 frames: 10 s of blocks each, raw per-block costs in raw/
set -eu
cd "$(dirname "$0")"
prio=${PRIO:-70}
tasks=${TASKS:-16}
mkdir -p raw
echo "# mode frames tasks voices blocks budget_us mean_us p50_us p99_us max_us over late helper_busy_pct..."
for voices in ${VOICES:-24}; do
  for frames in 128 256 512; do
    blocks=$((480000 / frames))
    for mode in off on; do
      ./thread_pool_bench "$PWD/fake_pool.clap" 48000 "$frames" "$tasks" "$voices" "$blocks" "$mode" "$prio" "raw/$mode-$frames-t$tasks-v$voices.txt"
    done
  done
done
