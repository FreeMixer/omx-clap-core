# The CLAP thread pool

`clap.thread-pool` lets a plugin split one `process()` into tasks: inside `process()` it calls the host's
`request_exec(num_tasks)`, and the host calls the plugin's `exec(plugin, task_index)` once for every index from 0 to
`num_tasks - 1` before `request_exec` returns. A plugin that splits its voices into tasks then uses as many cores as
the host gives it. This page states how libomx-clap-core runs those tasks, when it spreads them across cores, and what
was measured (#395).

**What it gains, and for which plugins.** The pool spreads voices only for a plugin that splits them into tasks. Surge
XT 1.4.0 does not offer `clap.thread-pool` (no `CLAP_EXT_THREAD_POOL` in its extension list, no `request_exec` in its
source), and none of the 489 plugin descriptors on the Raspberry Pi 5 of the measurement mentions the extension. For
every other plugin the cores are used by placement: one worker per instrument or chain, pinned across the cores
(plugin-hostd#226, not yet built). That, not the pool, is what the Pi gains from today. The pool is the host behaviour
the extension specifies, and the gate by qualification stands for the plugins that use it.

## Two paths

**Serial.** Every task runs on the calling audio thread, one after the other, before `request_exec` returns. The
extension allows it, it is real-time safe, and it is what an instance gets unless the pool is granted to it.

**Pooled.** The process holds one pool of helper threads, shared by every instance granted it:

- **Size.** N is the number of cores in the worker's CPU affinity mask when the pool is made (`sched_getaffinity`, so
  a worker that plugin-hostd placed on cores gets exactly those); the pool has N - 1 helpers (at most
  `OMX_CLAP_THREAD_POOL_HELPERS_MAX`), and the audio thread is the N-th worker. A mask of one core gives no pool.
- **Placement.** Each helper is pinned to one core of the mask, every core but the mask's first, which is left to the
  audio thread. A helper is named `omx-pool-<core>` (ps, `/proc/<pid>/task/*/comm`).
- **Priority.** A helper runs at the audio thread's scheduling: `SCHED_FIFO` at the audio thread's real-time priority
  (omx-clap-host reads it from JACK, `jack_client_real_time_priority`), or the normal class when the audio thread is not
  real-time. When the priority cannot be set (no `CAP_SYS_NICE`, an `RLIMIT_RTPRIO` below it) no pool is made and the
  instance runs serial, with one log line naming why: `thread pool: serial: SCHED_FIFO <p> refused (<errno>)`.
- **Lifetime.** The pool is made by the first activate of an instance granted it and ended by the last deactivate; its
  threads are created and joined on the control thread only. Nothing is allocated after the activate.
- **Parking.** Between requests a helper spins for a few microseconds, then sleeps on a futex. A request wakes the
  sleepers with one `FUTEX_WAKE`, only when some helper is asleep.
- **Handing out.** The request is published as one 64-bit word: a generation, the task count and the next index. The
  helpers and the calling audio thread take indices from it with one compare-and-swap each, so a task is taken
  exactly once and no lock is held anywhere. The audio thread takes tasks too: a helper that has not woken yet is never
  waited for, the audio thread runs the tasks it would have taken.
- **Completion.** `request_exec` returns when every task has finished. The audio thread spins on the completion count
  for the length of one block (`max_block / rate`), then sleeps on the completion futex, which the helper that finishes
  the last task wakes. A wait that runs past the block is a **late** event: it is counted in the process's pool
  (`omx_clap_host_thread_pool`) and omx-clap-host adds it to the `xruns <count>` line it writes on its feedback port,
  so plugin-hostd attributes it as it does an xrun. A task a helper has already started is still waited for: returning
  before it ends would hand `process()` back to the plugin while its own task still runs, which the extension forbids.
  Tasks nobody has started are never waited for.
- **Exclusion.** One request holds the pool at a time. A second audio thread that asks while another request holds it
  runs its tasks serial. A request made from inside a task, on a helper or on the audio thread, is refused, as before.
- **The role.** Inside a task, a helper answers `clap.thread-check`'s `is_audio_thread` true for the instance whose task
  it runs: the helpers are audio threads of that instance for the length of the task.
- **Limits.** A request of one task, or of more than 65535, runs serial.

## The gate: per plugin, by its qualification

The pool is never a global switch. A plugin gets helper threads only when its qualification measured it real-time safe
(`hosting.rt-safety.*` clean, with the interposer watching the helper threads too) and faster with the pool at that
quantum; plugin-hostd carries that verdict in the plugin's pin as `pool: cores|off`. An unmeasured plugin, or one with
violations, runs serial.

In the library the gate is one call per instance, made while it is inactive:
`omx_clap_host_thread_pool_use(in, use, rt_priority)`. An instance never told to use the pool runs serial.

omx-clap-host takes the verdict on its command line, `--thread-pool=qualified|off|on`:

- `qualified` (the default): an instance gets the pool only when the host was told its qualification grants it. The
  host has no per-instance verdict input yet, so `qualified` runs every instance serial until plugin-hostd hands the
  verdict over.
- `on`: every instance that implements `clap.thread-pool` gets the pool. plugin-hostd passes it to the worker of a
  plugin whose pin says `pool: cores`; a pinned plugin runs in a worker of its own, so the worker's switch is that
  plugin's verdict.
- `off`: every instance runs serial.

## Measured

Raspberry Pi 5 (4 cores, governor `powersave`, kernel 6.18 PREEMPT), idle (no stress run), 2026-10-29. Surge XT offers
no `clap.thread-pool`, and no plugin on the device does, so this is **the fixture plugin**, `tests/fake_pool.clap`: 16
tasks a block, each rendering 8, 24 or 48 sine voices over the block. The bench (`tests/thread_pool_bench.c`) is the
audio thread at SCHED_FIFO 70 (jackd's `-P 70`) on core 0, the pool's three helpers on cores 1 to 3 at the same
priority, each block started at its period for 10 s; the branch was built natively on the Pi with its gcc. Per-block
cost of `omx_clap_host_run` in microseconds, 48000 Hz:

| voices/task | frames | budget | off mean | off p99 | on mean | on p99 | speed-up | helper busy |
|---|---|---|---|---|---|---|---|---|
| 8  | 128 | 2667  | 263.6  | 277.1  | 81.7   | 86.8   | 3.2x | 2.5% each |
| 8  | 256 | 5333  | 527.9  | 534.8  | 155.9  | 164.6  | 3.4x | 2.5% each |
| 8  | 512 | 10667 | 1035.3 | 1050.2 | 289.5  | 296.0  | 3.6x | 2.4% each |
| 24 | 128 | 2667  | 780.6  | 793.2  | 212.9  | 221.1  | 3.7x | 7.4% each |
| 24 | 256 | 5333  | 1583.4 | 1591.1 | 418.7  | 425.0  | 3.8x | 7.4% each |
| 24 | 512 | 10667 | 3242.2 | 3269.9 | 845.6  | 858.0  | 3.8x | 7.6% each |
| 48 | 128 | 2667  | 1660.9 | 1683.1 | 432.3  | 439.0  | 3.8x | 15.6% each |
| 48 | 256 | 5333  | 3335.5 | 3362.7 | 857.9  | 863.2  | 3.9x | 15.6% each |
| 48 | 512 | 10667 | 6603.3 | 6649.5 | 1686.6 | 1699.2 | 3.9x | 15.4% each |

No block went over its budget and no wait was late in any run. "Helper busy" is each helper's time inside tasks over
the run's wall time. The 512-frame load of 48 voices, 6.6 ms of the 10.7 ms block serial, takes 1.7 ms on the pool.
The raw per-block costs, the summary and the device are in `docs/evidence/thread-pool-pi5-2026-10-29/`
(`measure.sh` reruns it).

Not measured: a real plugin that requests tasks (none on the device does), the pool under JACK's own thread rather
than the bench's, voices until the first xrun, and 96 kHz. The fixture's tasks are equal in size and touch no shared
state, which is the pool's best case.
