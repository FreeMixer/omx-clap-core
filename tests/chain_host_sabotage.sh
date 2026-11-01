#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The sabotage of the host's chain guards (#104, docs/chain.md): each arm copies the sources into
# build/sabotage-chain-host-<arm>, breaks one guard of src/effects.c or src/main.c there with one sed, builds and runs
# make test-chain-host (or, with PLUGIN_HOSTD set, make test-hostd-chain for the arm only the daemon shows) in that
# copy. The arm holds when the test goes red on the check that names the guard; an arm whose test stays green, goes red
# on something else, or whose sed changed nothing fails the run. Nothing outside build/sabotage-chain-host-* is touched.
#
# CHAIN_SABOTAGE_JOBS: make's parallel jobs in each copy (default 4); PLUGIN_HOSTD: the daemon, for the hostd arm
# (skipped, and said, without it); CHAIN_SABOTAGE_ARMS: the arms and drills to run, by name, space-separated (default
# every one)

set -u

root=$(cd "$(dirname "$0")/.." && pwd)
jobs=${CHAIN_SABOTAGE_JOBS:-4}
failures=0

# an arm or drill CHAIN_SABOTAGE_ARMS names, or every one when it names none
picked() { [ -z "${CHAIN_SABOTAGE_ARMS:-}" ] || [[ " $CHAIN_SABOTAGE_ARMS " == *" $1 "* ]]; }

# arm <name> <file> <sed expression> <the FAIL line's text> [<make target>: test-chain-host, or test-hostd-chain]
arm() {
    local name=$1 file=$2 expr=$3 fail=$4 target=${5:-test-chain-host} dir log
    picked "$name" || return 0
    dir=$root/build/sabotage-chain-host-$name
    log=$dir.log
    rm -rf "$dir"
    mkdir -p "$dir"
    cp -r "$root/src" "$root/lv2" "$root/tests" "$root/data" "$root/Makefile" "$root/VERSION" "$dir/"
    # what the worktree built is not the copy's: everything is built again from the sabotaged sources
    make -s -C "$dir" clean >/dev/null 2>&1
    sed -i -e "$expr" "$dir/$file"
    if cmp -s "$root/$file" "$dir/$file"; then
        echo "FAIL $name: the sed left $file as it was"
        failures=$((failures + 1))
        return
    fi
    if (cd "$dir" && make -j"$jobs" omx-clap-host && LD_LIBRARY_PATH=$dir make "$target" PLUGIN_HOSTD="${PLUGIN_HOSTD:-}") >"$log" 2>&1; then
        echo "FAIL $name: survived, the test stays green with $file broken (log $log)"
        failures=$((failures + 1))
    elif ! grep -q -E "^FAIL .*$fail" "$log"; then
        echo "FAIL $name: red, but not on \"$fail\" (log $log)"
        grep -E "^FAIL" "$log" | head -5 | sed 's/^/     /'
        failures=$((failures + 1))
    else
        echo "ok   $name: red as expected: $(grep -m 1 -E "^FAIL .*$fail" "$log")"
    fi
}

# drill <name> <file> <sed expression> <fault>: one JACK call made to fail for one instance in a copy of the sources,
# and tests/chain_host_e2e.sh's drill CHAIN_HOST_FAULT=<fault> run on it, which must stay GREEN: a line JACK fails part
# way answers the error, frees no instance and runs the chain its read-back names, a refused one changes nothing
drill() {
    local name=$1 file=$2 expr=$3 fault=$4 dir log
    picked "$name" || return 0
    dir=$root/build/drill-chain-host-$name
    log=$dir.log
    rm -rf "$dir"
    mkdir -p "$dir"
    cp -r "$root/src" "$root/lv2" "$root/tests" "$root/data" "$root/Makefile" "$root/VERSION" "$dir/"
    make -s -C "$dir" clean >/dev/null 2>&1
    sed -i -e "$expr" "$dir/$file"
    if cmp -s "$root/$file" "$dir/$file"; then
        echo "FAIL drill $name: the sed left $file as it was"
        failures=$((failures + 1))
        return
    fi
    if (cd "$dir" && make -j"$jobs" omx-clap-host tests/jack_identity tests/jack_latency_probe tests/fake.clap \
            tests/fake_compressor.clap tests/fault-14.clap \
            && LD_LIBRARY_PATH=$dir CHAIN_HOST_FAULT=$fault ./tests/chain_host_e2e.sh) >"$log" 2>&1 \
        && grep -q -E "^ok   .*answers the error" "$log"; then
        echo "ok   drill $name: $(grep -m 1 -E "^ok   .*answers the error" "$log" | sed 's/^ok   //'), every instance kept"
    else
        echo "FAIL drill $name: a line JACK fails part way is not answered as it should be (log $log)"
        grep -E "^FAIL" "$log" | head -5 | sed 's/^/     /'
        failures=$((failures + 1))
    fi
}

# keep <name> <file> <sed expression> <a line the log must show>: a hold put in a copy of the sources, and make
# test-chain-host run on it, which must stay GREEN and show that line
keep() {
    local name=$1 file=$2 expr=$3 ok=$4 dir log
    picked "$name" || return 0
    dir=$root/build/keep-chain-host-$name
    log=$dir.log
    rm -rf "$dir"
    mkdir -p "$dir"
    cp -r "$root/src" "$root/lv2" "$root/tests" "$root/data" "$root/Makefile" "$root/VERSION" "$dir/"
    make -s -C "$dir" clean >/dev/null 2>&1
    sed -i -e "$expr" "$dir/$file"
    if cmp -s "$root/$file" "$dir/$file"; then
        echo "FAIL keep $name: the sed left $file as it was"
        failures=$((failures + 1))
        return
    fi
    if (cd "$dir" && make -j"$jobs" omx-clap-host && LD_LIBRARY_PATH=$dir make test-chain-host) >"$log" 2>&1 \
        && grep -q -E "$ok" "$log"; then
        echo "ok   keep $name: green with the hold: $(grep -m 1 -E "$ok" "$log")"
    else
        echo "FAIL keep $name: red with the hold (log $log)"
        grep -E "^FAIL" "$log" | head -5 | sed 's/^/     /'
        failures=$((failures + 1))
    fi
}

# 1. a member that joins closes its client, so the chain is one node: left open
arm one-node src/effects.c '/^static int chain_join/,/^}/s/^        jack_client_close(effect->jack_client);$/        (void)0;/' \
    'chain 7 10 11 12: one node'

# 2. the head's cycle runs the chain: the head alone instead
arm chain-run src/effects.c 's/(chain = atomic_load_explicit(&effect->chain_run, memory_order_relaxed)))/(chain = NULL))/' \
    'p1 left: the chain against the series'

# 3. the head's ports report the members' sum: the head's own latency instead
arm latency-sum src/effects.c '/^static void chain_latency_changed/,/^}/s/^    atomic_store_explicit(&chain->head->latency_run, frames, memory_order_relaxed);$/    (void)frames;/' \
    'p1: the latency to the last output'

# 4. a member the line leaves out leaves the walk: kept in it
arm left-out src/effects.c '/^int effects_chain/,/^}/s/^        if (i < n)$/        if (1)/' \
    'chain 7 10 12: two nodes'

# 5. a member that leaves gets a client of its own back: the client made for it never taken
arm client-back src/effects.c 's/^        if (client_take(leaving\[j\], \&own\[j\]) != SUCCESS)$/        if (0)/' \
    '(chain 7 10 12: two nodes|pass has its ports again)'

# 6. the head's removal gives the other members a client of their own first: its client closed under them
arm head-remove src/effects.c 's/^        if (effect->chain)$/        if (0)/' \
    'remove 12: pass alone in its own client'

# 7. one width end to end, checked before anything changes: not checked. A member of another width is still refused by
# the library's own guard (OMX_CLAP_CHAIN_ERR_WIDTH) when it would join, so the host's check shows on a head of
# another width, which the library refuses only once the chain has formed and the member would join, answering the
# same code: the chain formed is what shows
arm width src/effects.c 's/^        if (!member_fits(effect, head))$/        if (0)/' \
    'the refusals formed no chain 8'

# 8. a member with a note port is no refusal (#400): the refusal of the first version taken back
arm note-port src/effects.c 's/^        \/\/ every member.s main input and output as wide as the head.s main output, or a mono member of a stereo walk$/        if (i > 0 \&\& (effect->midi_port || effect->midi_out_port))\n            return ERR_INVALID_OPERATION;\n&/' \
    'chain: chain 9 15 13 -> '

# 8b. and the member gets its note port again in a client of its own: the client made ready for it registers none
arm note-port-back src/effects.c 's/^        \&\& !(own->midi_in = jack_port_register(.*$/        \&\& 0)/' \
    '13 has its note port again'

# 9. a member of another chain: taken into this one as well
arm other-chain src/effects.c 's/if (effect->chain \&\& effect->chain != chain)/if (0)/' \
    'chain 8 15 10 -> '

# 10. the cycle allocates, locks or prints nothing: a print in every member's stage
arm rt-print src/effects.c '/^static void chain_stage/,/^}/s/^    effect_t \*effect = ctx;$/    effect_t *effect = ctx;\n    fprintf(stderr, "stage\\n");/' \
    "the host's chain cycle names an allocator"

# 10b. and nothing the cycle calls does either: an allocation in transport_block, which process_gated calls
arm rt-callee src/effects.c '/^static void transport_block/,/^}/s/^    struct omx_clap_transport rec;$/&\n\n    free(malloc(1));/' \
    "the host's chain cycle names an allocator"

# 10c. and nothing the core runs for it either: an allocation in omx_clap_chain_process, the library's walk the head's
# cycle runs (src/clap_chain.c, which the scan reads with clap_host.c, clap_host.h and hosted_stage.h)
arm rt-core src/clap_chain.c '/^int omx_clap_chain_process/,/^}/s/^    uint32_t s;$/&\n\n    free(malloc(1));/' \
    "the host's chain cycle names an allocator"

# 12. a member's auxiliary ports are on the head's client, under the name it has alone too, and leave with it
arm aux-port src/effects.c 's/^        aux_ports_on_head(effect, head->jack_client);$/        ;/' \
    'the side chain of member 16 is'
arm aux-connect src/effects.c "s/^        if (len \&\& client_borrowed(effect) .*/        if (0)/" \
    'connect effect_10:out_1 effect_16:aux_in_1_1'
arm aux-leave src/effects.c '/^static void chain_extract/,/^}/s/^        aux_ports_off_head(effect, chain->head->jack_client);$/        ;/' \
    'and the head.s client no longer holds it'

# 13. a move takes the line's order: the kept members' order never published, the walk left in the old one
arm move src/effects.c 's/^    if (j < n_kept \&\& (ret = chain_order(chain, kept, n_kept)) != 0)$/    if (0)/' \
    'p5b left: comp moved after gain'

# 14. a member's own bypass reaches it in the chain: a bypass that reaches a chain's head alone
arm member-bypass src/effects.c 's/^    omx_clap_host_bypass(g_effects\[effect_id\]\.clap, value);$/    if (!g_effects[effect_id].chain || g_effects[effect_id].chain->head == \&g_effects[effect_id])\n        omx_clap_host_bypass(g_effects[effect_id].clap, value);/' \
    'p2b left: gain, a member, bypassed in both'

# 15. the members' walk on JACK's thread and every change of the members are ordered by g_chain_members. The probe, in
# a copy only: a change that holds the lock (chain_extract, step 7's remove of a member) names a block size for its head
# as the buffer-size callback does, from its own thread, which is a try of a held mutex; the walk must leave that size
# to the control thread (resize_named kept). With the lock gone the walk runs under the change and the size is consumed:
# the host logs a FAIL line and the e2e's last step names it. No timing: the try and the hold are on one thread, so
# the arm turns red on every run, where ThreadSanitizer saw the race only when the stress happened to meet it.
lock_probe='/^static void chain_extract(chain_t \*chain, effect_t \*effect)$/i\
static void lock_probe(chain_t *chain)\
{\
    effect_t *head = atomic_load_explicit(&chain->members[0], memory_order_relaxed);\
\
    if (!head || !head->jack_client || atomic_load_explicit(&head->chain_run, memory_order_relaxed) != chain)\
        return;\
    chain_followers_each(head, jack_get_buffer_size(head->jack_client));\
    if (!atomic_load_explicit(&head->resize_named, memory_order_acquire))\
        fprintf(stderr, "%s%c", "FAIL a block size named while a change held g_chain_members was walked under it", 10);\
}\

/^static void chain_extract(/,/^}/s/^    \(pthread_mutex_lock(&g_chain_members);\|(void)0;\)$/&\n    lock_probe(chain);/'

# the probe alone, the lock kept: green, so the arms below are red on the lock and not on the probe
keep lock-probe src/effects.c "$lock_probe" '^ok   the hosts logged no FAIL line'

# the lock gone at every site
arm lock src/effects.c 's/^\( *\)pthread_mutex_\(un\)\{0,1\}lock(\&g_chain_members);$/\1(void)0;/;s/pthread_mutex_trylock(\&g_chain_members) == 0/1/'"
$lock_probe" 'walked under it'

# 15a. the same, one site: the walk on JACK's thread (chain_followers_each) takes the try as had, every other site kept
lock_site() {
    arm "lock-$1" src/effects.c "/^static [a-z_ ]*\\<$1(/,/^}/{s/^\\( *\\)pthread_mutex_\\(un\\)\\{0,1\\}lock(\\&g_chain_members);\$/\\1(void)0;/;s/pthread_mutex_trylock(\\&g_chain_members) == 0/1/}
$lock_probe" 'walked under it'
}
for site in chain_followers_each; do lock_site "$site"; done

# 16. a client's thread on its instance's audio role from its first cycle, named with no lock: the thread-init
# callback's store into the slot gone, so the plugin runs off the role in its own client (pass says so)
arm slot src/effects.c '/^static void jack_thread_init(void \*arg)$/,/^}$/s/^    atomic_store_explicit(&effect->thread_named, pthread_self(), memory_order_release);$/    (void)0;/' \
    'pass ran on the audio role from its first cycle'

# 16b. and a member on its head's thread: a joining member's role left on its own slot, not its head's, so the head's
# cycle runs it off the role
arm member-slot src/effects.c '/^static int chain_insert(/,/^}$/s/^        omx_clap_host_name_slot(effect->clap, &head->thread_named);$/        (void)0;/' \
    'pass ran on the audio role from its first cycle'

# 17. JACK's thread never waits on g_chain_members: every add holds it from before its client's jack_activate until that
# client's thread-init callback has returned, polling with a sleep, as a resize holds its lock across its poll
# and the plugin's activate. A callback that waits on the lock never returns while it is held, so the add gives
# up after 10 s and answers -202 (the bound only ends a red run; a callback that does not wait returns at once, so the
# outcome does not hang on timing); every add answering, and the whole test green, is the callback returning with the
# lock held
keep role-hold src/effects.c '/^static void jack_thread_init(void \*arg)$/,/^}$/s/^}$/    atomic_store(\&g_role_hold_returns, 1);\n}/;s/^static void jack_thread_init(void \*arg)$/static _Atomic int g_role_hold_returns;\n\n&/;/^int effects_add(/,/^}$/s/^    if (jack_activate(effect->jack_client) != 0)$/    int held_polls = 0;\n\n    pthread_mutex_lock(\&g_chain_members);\n    atomic_store(\&g_role_hold_returns, 0);\n    if (jack_activate(effect->jack_client) == 0)\n        while (!atomic_load(\&g_role_hold_returns) \&\& held_polls++ < 10000)\n            usleep(1000);\n    pthread_mutex_unlock(\&g_chain_members);\n    if (!atomic_load(\&g_role_hold_returns))\n        fprintf(stderr, "role hold: the thread-init callback did not return with the lock held\\n");\n    if (!atomic_load(\&g_role_hold_returns))/' \
    '^chain host: 0 failed$'

# the drills: a line JACK (or the library) fails part way. Each must stay green: refused with nothing changed, or
# answered with every instance kept and the chain its read-back names
drill client src/effects.c '/^static jack_client_t \*open_jack_client/,/^}/s/^    size_t name_limit;$/&\n    static int opened[MAX_INSTANCES];\n\n    if (instance == 11 \&\& opened[instance]++)\n        return NULL;/' client
drill stop src/effects.c '/^static int member_stop/,/^}/s/^    if ((ret = jack_deactivate(effect->jack_client)) != 0)$/    if ((ret = effect->instance == 13 ? -1 : jack_deactivate(effect->jack_client)) != 0)/' stop
drill activate src/effects.c '/^static int client_take/,/^}/s/^    if (jack_activate(effect->jack_client) != 0)$/    if (effect->instance == 11 || jack_activate(effect->jack_client) != 0)/' activate
# the library refuses a move's order: the line answers it, the walk in the order it had
drill order src/effects.c '/^static int chain_order/,/^}/s/^    ret = omx_clap_chain_order(chain->walk, ids, n);$/    ret = n ? OMX_CLAP_CHAIN_ERR_ARG : omx_clap_chain_order(chain->walk, ids, n);/' order
# another head: a member named anew not stopped; a member left out not activated after the new head's client was; the
# old head, named as a member, not stopped once the others are in their clients
drill rehead-stop src/effects.c '/^static int member_stop/,/^}/s/^    if ((ret = jack_deactivate(effect->jack_client)) != 0)$/    if ((ret = effect->instance == 13 ? -1 : jack_deactivate(effect->jack_client)) != 0)/' rehead-stop
drill rehead-activate src/effects.c '/^static int client_take/,/^}/s/^    if (jack_activate(effect->jack_client) != 0)$/    if (effect->instance == 11 || jack_activate(effect->jack_client) != 0)/' rehead-activate
drill rehead-head-stop src/effects.c '/^static int member_stop/,/^}/s/^    if ((ret = jack_deactivate(effect->jack_client)) != 0)$/    if ((ret = effect->instance == 10 ? -1 : jack_deactivate(effect->jack_client)) != 0)/' rehead-head-stop
# a member named anew, linked in its own client and stopped, then a later step fails: it runs again with its links; a
# second member named anew not stopped, and, another head, a member left out not activated
drill stop-links src/effects.c '/^static int member_stop/,/^}/s/^    if ((ret = jack_deactivate(effect->jack_client)) != 0)$/    if ((ret = effect->instance == 14 ? -1 : jack_deactivate(effect->jack_client)) != 0)/' stop-links
drill rehead-activate-links src/effects.c '/^static int client_take/,/^}/s/^    if (jack_activate(effect->jack_client) != 0)$/    if (effect->instance == 11 || jack_activate(effect->jack_client) != 0)/' rehead-activate-links

# 11. behind the daemon the chain is one node because this host answers the line: refused instead
if [ -n "${PLUGIN_HOSTD:-}" ]; then
    arm hostd-line src/main.c 's/            resp = effects_chain(id, instances, n);/            resp = ERR_INVALID_OPERATION;/' \
        'chain 5 is one node' test-hostd-chain
else
    echo "skip hostd-line: no PLUGIN_HOSTD"
fi

if [ "$failures" -ne 0 ]; then
    echo "FAIL chain host sabotage: $failures arm(s) did not hold"
    exit 1
fi
echo "ok   chain host sabotage: every guard seen red"
