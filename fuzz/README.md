# Fuzzing the readers

The host reads bytes from disk that people and other programs write: preset files, the state bodies they carry, and
the `manifest.ttl` of every bundle on the LV2 path. Each reader that parses those bytes itself has a libFuzzer harness
here, built with clang under AddressSanitizer and UndefinedBehaviorSanitizer (`-fno-sanitize-recover=all`) and run in
CI on every push and pull request (`.github/workflows/fuzz.yml`).

A harness is one C file defining `LLVMFuzzerTestOneInput`. It links its own file, `fuzz/alloc.c` and the reader's
source, and nothing else: no plugin is loaded, nothing is opened with `dlopen`, and no harness reads a clock, the
environment, a file or the network, or writes a file. Properties are checked with `if (!cond) abort();` in a
`noinline` function `check_<property>`, never `assert()`, and every outcome an input can choose returns 0. An input
larger than the reader itself would read is refused.

## The harnesses

| Harness | Reader | Input |
| --- | --- | --- |
| `fuzz_preset` | `pj_parse` of `src/preset_file.c`, the preset file (docs/presets.md), and the writer `pj_write` over what it accepts | the file, whole, up to `PRESET_FILE_MAX_BYTES` |
| `fuzz_state` | `preset_base64_decode` of `src/preset_file.c`, the state body of a preset (`state.bytes`), and `preset_base64_encode` back | the text, up to its first NUL |
| `fuzz_manifest` | `lv2_locate_manifest_names` of `src/lv2_locate.c`, which bundle may hold a plugin URI | one byte k, k bytes of URI, then the manifest |

What has no harness, and why:

- **Descriptor JSON and qualification records.** omx-clap-scan writes the descriptor JSON (`src/scan.c`); nothing in
  this tree reads one back, and no qualification record is read here. `pj_parse` is the one JSON reader of the tree.
- **The LV2 bridge's Turtle.** Every ttl the host and the adapter read for a plugin, its ports, presets and state goes
  through lilv (`lv2/core/lv2_world.c`), which is not this tree's parser. The one Turtle the tree reads itself is the
  manifest text `lv2_locate` scans before it opens anything, which `fuzz_manifest` drives.
- **A state body saved to a state file.** `clap_state_load` (`src/effects.c`) hands its bytes to the plugin through the
  core's bounded stream; the parser of those bytes is the plugin. The adapter's state is Turtle that lilv reads.

### The properties

Each harness checks the same five, one `check_` function each:

- **no_overread**: the answer is the one the input's own bytes give. The reader is handed the input three times with
  different bytes past it (two digits and a NUL for the preset file; `AA`, `BA` and a NUL past a state text's NUL;
  a name character and a NUL past the manifest's and the URI's) and must give one answer, and the same record whenever
  two of them accept; a reader that reads past the input sees the difference. The call that follows is handed a copy of
  exactly the input, so a read past it is also AddressSanitizer's.
- **as_written**: the answer is the one the input's grammar gives, read by the harness itself and never by the reader
  under test. Each harness's header writes its grammar down: the preset file's JSON (its depth, its `\u` escapes, its
  lax numbers), the state body's base64 (whitespace skipped, padding allowed), and `src/lv2_locate.h`'s rule for when a
  manifest names a URI. The reader must refuse what the grammar refuses and accept what it reads, and to the same
  record: every string's bytes and every number's text and value for a preset, every byte of a state body. A reader and
  a writer that share one mistake, an escape read and written back the same wrong way or a base64 letter both get wrong,
  make a round trip that holds; this property does not. A limit of a grammar is the harness's own, never the reader's:
  arrays and objects of a preset nest 32 deep at most (`JSON_DEPTH_MAX` of `fuzz/fuzz_preset.c`, which
  `PJ_ALLOC_BOUND` uses too), so a reader whose `PRESET_JSON_DEPTH_MAX` moves refuses `deepest` or reads
  `refuse-too-deep`; a manifest is read for 16 declarations of a namespace the URI starts with at most (`PREFIXES_MAX`
  of `fuzz/fuzz_manifest.c`), so a reader whose `LV2_LOCATE_PREFIXES_MAX` moves names the URI in `prefixes-most-read`
  or turns down `prefixes-past-most-read`. `fuzz/gen-corpus.c` writes those seeds at the same two limits.
- **reject_clean**: a refusal leaves no partial state. A refused preset leaves the tree zeroed; a refused state text
  writes no length; every refusal gives back every byte the reader took. The input is read once more with one of the
  allocations its reading took failing (which one, its bytes choose), as when memory runs out: the reader refuses and
  holds nothing, and so does the writer, pretty and compact, or the encoder. The seed `compact-second-allocation` is a
  compact text past the writer's first 4096 bytes whose bytes pick the second allocation to fail.
- **round_trip**: render and parse again gives the same record, and the render is the format's. A preset written back
  compact and pretty is, byte for byte, the text the harness renders of the tree in the writer's format, and reads back
  to the same tree, member order and every number's text included; a rewrite that sets `modifiedAt` keeps every other
  member in its place. A state body encodes to the encoding the harness writes of it, `(len + 2) / 3 * 4` characters
  that decode to it, and survives a whole preset written and read back. A URI written as a manifest, whole and at every
  split as `@prefix`, `PREFIX` and the empty prefix, is named; the empty text names nothing; and the same spellings with
  one more name character after the URI's end get the answer the rule gives.
- **bounded_alloc**: one input's allocation is bounded by its size. The preset reader holds at most 256 bytes for
  each byte of file and the first slots of the 32 containers that may be open; the decoder `n / 4 * 3 + 3` bytes for
  `n` characters; the manifest reader the URI's length and three.

`fuzz/alloc.h` is forced on the reader's objects, never on the harness: the reader's `malloc`, `calloc`, `realloc`,
`strdup` and `free` go to `fuzz/alloc.c`, which counts what it holds, the most it held or asked to hold, and can fail
one allocation. It notes when the C library returns NULL on a request of at most `fuzz_alloc_most()` bytes; fuzz_preset
sets that to `PJ_ALLOC_BOUND` of the input, and ends the call when memory runs out. A request past it is left to the
checks. A block's size sits in a head AddressSanitizer is told is poisoned, so a block still ends where the reader's
bytes end.

## The seed corpus

`fuzz/corpus/<harness>/` is written by `fuzz/gen-corpus.c` (`make fuzz-corpus`) from what the tree holds: the preset
example of `docs/presets.md`, the JSON of `data/`, the Turtle of every LV2 fixture bundle (`lv2/tests/fixtures/`), the
RFC 4648 vectors, the spellings `tests/lv2_locate_test.c` reads, and presets and state bodies made by the reader's own
writer and encoder. Beside them it writes one input for each refusal a reader makes. Nothing is fetched, directories
are read in name order, and the corpus is committed, so CI and a tarball fuzz from the same seeds. `make
fuzz-corpus-check` is red when the committed corpus is not what the generator writes.

## The coverage witness

`make fuzz-coverage` builds each harness for `llvm-cov` without sanitizers, runs it once over its corpus (`-runs=0`) and
prints the line coverage of the reader functions it drives and every line of them no seed reached (`fuzz/coverage.sh`
lists the functions). `fuzz/coverage-floor.txt` holds the lines each harness covers today, and the target is red when
one falls below it. Every `abort()` of a harness, and the statement that guards it, must be reached by a seed too
(`fuzz/checks.awk`): a check no input reaches is a property nobody tests.

A line counts as covered when any seed runs it, so a branch on a line other seeds reach is one the floor cannot see.
`fuzz/reach.txt` names such a branch and the seed written for it; the target runs that seed alone and is red, naming
the branch by file, line and column, when the seed is gone or does not take the branch's true side. It holds the
manifest's bound: `prefixes-past-most-read` alone must take `++read_again > LV2_LOCATE_PREFIXES_MAX`, a line every
seed with a matching declaration reaches. An empty `fuzz/reach.txt` (every line blank or a comment) proves no branch
and is red for it, the same way an empty coverage report is; an entry naming a harness the run does not know is red
too, never skipped. `make fuzz-coverage-self` (`fuzz/coverage.sh --self-test`) sabotages each of those two ways and
checks the target turns red, without needing a build.

| Harness | Reader lines | Covered | Lines no input reaches |
| --- | ---: | ---: | --- |
| preset | 394 | 387 (98.22%) | `parse_string`'s two `goto bad` once its first scan has found the closing quote; `pj_free(NULL)`; `pj_set`'s allocation failure, a builder only a harness calls |
| state | 60 | 60 (100%) | |
| manifest | 107 | 107 (100%) | |

## The records

Every `abort()` of `fuzz/` is a check, and its comment names a bug it catches: a one-line change of the reader and the
seed that shows it, in the form OSS Verify reads (docs/build-contract.md of oss-verify, "The checks of a fuzz target"):

```
/* The tree is the one the file spells.
 * mutant: src/preset_file.c:151 `'\n'` -> `'\r'`
 * input: escapes */
if (rc == 0 && want != 0)
    abort();
```

A stop after an allocation of the harness's own fails has one line `not a property: <reason>` instead. `make
fuzz-records` (`fuzz/records.sh`) proves each record alone: the harness, plain and native with `fuzz/standalone.c` for
its driver, exits 0 on the seed as the tree is; with the change made and the harness built again it exits 134 on the
seed, at that check's own `abort()`. A check with neither a record nor a `not a property` line, or with lines not of
that form, makes `make fuzz-records` and `make fuzz-coverage` red (`fuzz/checks.awk`): a check that names no bug it
catches is a property nobody showed can fail. A record's line is the line of `src/` at the commit it is read at, so a
change to the reader moves the records it shifts.

## The mutants

`make fuzz-sabotage` proves each property can find a bug over the whole corpus. Each
`fuzz/sabotage/<harness>-<property>.patch` is a one-line bug in the reader, one for each property of each harness but
as_written, which OSS Verify reads as the breakage patches of `mutants: fuzz/sabotage`; above its `diff --git` line it
names the check it breaks and the seed that catches it (`Breaks the check of fuzz/fuzz_<harness>.c:<line> (...).` and
`Caught by the seed input "<seed>" of fuzz_<harness>.`). The target applies each, rebuilds, and runs the harness over
every seed of its corpus, one at a time, without fuzzing. A mutant is found when the seed its header names aborts in
that property's own `check_` function, and every seed that fails does so by an `abort()` in a `check_` function: one
bug the harness reads in two ways may stop a seed in another property first, but a seed that crashes anywhere else, a
sanitizer report included, makes it red. The source is restored with `git checkout --` on
every exit path. `make fuzz-records` proves the header's line too: the patch applied alone, the named seed aborts at
that check's own `abort()`.

| Mutant | The bug | Named seed |
| --- | --- | --- |
| preset-no-overread | a number's leading-zero test peeks one byte past the input | `zero-alone` |
| preset-reject-clean | a member that fails is not counted, so not freed with its container | `refuse-array-cut` |
| preset-round-trip | an escaped character is written again raw after its escape | `escapes` |
| preset-bounded-alloc | a container's first slots are 512, not 8 | `many-small-containers` |
| state-no-overread | whitespace skips the character after it too | `trailing-newline` |
| state-reject-clean | the decoder keeps its buffer on a refusal | `refuse-letter` |
| state-round-trip | the encoder writes a letter where one byte's pad goes | `rfc4648-f` |
| state-bounded-alloc | the decoder's buffer is three bytes a character | `rfc4648-f` |
| manifest-no-overread | a prefixed name's end is read one byte late | `name-at-end` |
| manifest-reject-clean | the URI's copy is never freed | `whole` |
| manifest-round-trip | a SPARQL `PREFIX` is read one byte late | `whole` |
| manifest-bounded-alloc | the URI's copy is twice its size | `whole` |

## What the harnesses found

`fuzz_manifest` found `lv2_locate_manifest_names` reading the whole text again for each declaration of a namespace
the URI starts with: a manifest that repeats one costs its size squared (a 4 MiB one, 10.7 s at -O3 per lookup).
Past `LV2_LOCATE_PREFIXES_MAX` such declarations a manifest is now a candidate unread, which the caller opens to know;
`tests/lv2_locate_test.c` holds it by counting the readings (`lv2_locate_manifest_readings`): 16 for a manifest of
1024 such declarations, where the reader before read it 1024 times. The seeds `prefixes-most-read` and
`prefixes-past-most-read` stand either side of the bound. The same reader compared a declared prefix at each prefixed
name and looked for the local name again one byte after each place it found it: a long prefix whose letters come again
before many prefixed names cost the prefix's length at each, and a local name that repeats itself, in a run of it, its
own length at each byte (4 MiB, 10.8 s and 1.01 s at -O3 per lookup). The same test counts the bytes compared
(`*compared`), at most twice the text's length for one reading of each shape. The seeds `long-prefix` and
`repeated-local` are small manifests of those shapes.

## Running

```
make fuzz                    # build every harness into build/fuzz/
make fuzz-run FUZZ_TIME=60   # fuzz each for 60 s; red on any crash
make fuzz-sabotage           # every mutant found, each in the property its header names
make fuzz-records            # every check's own record, alone, makes that check abort on its seed
make fuzz-coverage           # the coverage witness and its floors
make fuzz-coverage-self      # the seed floor's own guards, sabotaged, must turn it red; needs no build
make fuzz-static             # each harness linked fully static with a plain driver, over its corpus
make fuzz-corpus             # write fuzz/corpus/ again from the fixtures
```

They need clang with libFuzzer (`clang compiler-rt` on Fedora), `llvm-cov` and `llvm-profdata` (`llvm`) for the
witness, git for the mutants, and the static libc (`glibc-static`) for `fuzz-static`. The ordinary build is never
touched: everything lands in `build/fuzz/`.

AddressSanitizer does not support a fully static executable, and libFuzzer's own runtime faults at startup under
`-static`. `make fuzz-static` links the same harness sources, `fuzz/alloc.c` and the reader with `fuzz/standalone.c`,
a driver that runs each file it is given through `LLVMFuzzerTestOneInput` once, into an executable with no library
beyond libc and libm: the build a verifier that brings its own driver takes.

## Reproducing a finding

`make fuzz-run` writes the input that crashed a harness to `fuzz/findings/<harness>-crash-<sha1>` (or `leak-`,
`timeout-`, `oom-`) and the harness's log beside it; CI uploads the directory as the `fuzz-findings` artifact. To
replay it:

```
make fuzz
build/fuzz/fuzz_<harness> fuzz/findings/<harness>-crash-<sha1>
```

Once the bug is fixed, have `fuzz/gen-corpus.c` write the input under a name that says what it is, so every later run
replays it first.
