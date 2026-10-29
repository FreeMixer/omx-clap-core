#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# builds the core of this branch, the pool fixture, its test and its bench natively, nothing installed
set -eu
cd "$(dirname "$0")"
F="-O3 -g -std=gnu99 -fPIC -fvisibility=hidden -D_GNU_SOURCE -pthread -ffp-contract=off -Wall -Wextra -Iinc -Isrc -DOMX_CLAP_VERSION=\"0.23.2\" -DOMX_CLAP_VERSION_NUM=2302u"
gcc $F -shared -Wl,-soname,libomx-clap-core.so.1 -Wl,--version-script=src/omx-clap-core.map -o libomx-clap-core.so.1 \
    src/clap_host.c src/idle_hold.c src/clap_chain.c src/thread_pool.c -ldl -lm
gcc $F -shared -o fake_pool.clap tests/fake_pool.c -lm
gcc $F -o thread_pool_bench tests/thread_pool_bench.c -L. -l:libomx-clap-core.so.1 -Wl,-rpath,"$PWD" -ldl -lm
gcc $F -o thread_pool_test tests/thread_pool_test.c -L. -l:libomx-clap-core.so.1 -Wl,-rpath,"$PWD" -ldl -lm
echo built
