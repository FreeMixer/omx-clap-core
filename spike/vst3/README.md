# VST3 feasibility spike (omx-clap-core#287), NOT FOR MERGE

Branch `spike/vst3-feasibility`, kept on origin by the operator's ruling of 2026-10-20 (not for merge, not for
deletion by branch hygiene). The report is `docs/adapter-seam-and-vst3.md` on main.

`vst3_probe.c` is plain C11 against Steinberg's generated C API, `vst3_c_api.h` from
https://github.com/steinbergmedia/vst3_c_api at 93a116f ("VST3 SDK 3.8.1", 3777 lines, BSD-3-Clause `LICENSE.txt`).
It needs no C++ compiler and no SDK library. It dlopens one installed `.vst3` bundle and then:

1. calls `ModuleEntry`, `GetPluginFactory`, `IPluginFactory::getFactoryInfo/countClasses/getClassInfo`
2. creates the `IComponent` and its separate `IEditController` and initialises both with the host context
3. connects the two halves through `IConnectionPoint` (directly, without a proxy, because there is one thread)
4. moves the component state to the controller (`getState` → `setComponentState`) through a memory `IBStream`
5. lists the buses (`getBusCount/getBusInfo`) and the parameters (`getParameterCount/getParameterInfo`,
   `getParamStringByValue`)
6. `setBusArrangements(stereo)`, `canProcessSampleSize(32)`, `setupProcessing(realtime, 32-bit, 512, 48000)`, then
   a state round trip (`setState` with the saved bytes, then `getState` again, compared), `activateBus`,
   `setActive(1)`, latency and tail, `setProcessing(1)`
7. processes one block of silence and eight blocks of white noise (−6 dBFS peak) and prints peak and RMS per channel
8. tears everything down in reverse order and calls `ModuleExit`

## Build and run

    git clone https://github.com/steinbergmedia/vst3_c_api && git -C vst3_c_api checkout 93a116f
    make VST3_C_API=$PWD/vst3_c_api
    ./vst3_probe /usr/lib64/vst3/lsp-plugins.vst3                                 # "Compressor Stereo"
    ./vst3_probe /usr/lib64/vst3/lsp-plugins.vst3 "Sidechain Compressor Stereo"

The subject was lsp-plugins-vst3 1.2.29-1.fc44 (Fedora 44 `updates`). Without root, it was unpacked from
`dnf download lsp-plugins-vst3` and the probe ran on the unpacked bundle. The two logs are `run-compressor-stereo.txt`
and `run-sidechain-compressor-stereo.txt`.

## What worked

- The module loaded, and the factory listed 388 classes: 194 audio modules and 194 controllers.
- "Compressor Stereo" has 48 parameters (1 flagged bypass), one stereo main input and one stereo main output, latency
  0 and tail 0. The component state is 517 bytes, and the `setState`/`getState` round trip was byte-identical.
- Silence in gave silence out (peak −inf dBFS). Noise at −6.04 dBFS peak and −10.84 dBFS RMS (left) came out at
  −6.57 dBFS peak and −11.37 dBFS RMS at the default settings.
- "Sidechain Compressor Stereo" has a second input bus, `sidechain_in` (stereo, aux, default active), and 51
  parameters. It processed with only the main bus handed over (`numInputs` 1).
- `setProcessing(1)` returned `kNotImplemented` (3), which the specification allows.

## What needed more than the obvious

1. **The host must implement `IMessage` and `IAttributeList`.** LSP's `IComponent::setState` asks the host context for
   an `IMessage` through `IHostApplication::createInstance` (cid 936F033B…) and crashed when the host refused. The
   SDK's C++ hosting library ships `HostMessage`/`HostAttributeList`. In C they are 147 lines (one heap object holding
   both interfaces and one refcount).
2. **State only after `setupProcessing`.** LSP 1.2.29 divides by its sample rate inside `IComponent::setState`. With
   the rate still 0 that is an integer division by zero (SIGFPE at `lsp-plugins.so+0x3b55e6`, `div %r8`). The probe
   restores state only after `setupProcessing`, so a host must order them that way.
3. **A crashed LSP instance poisons every later one.** LSP keeps a shared-memory catalog,
   `/dev/shm/lsp-catalog-<user>.shm` (with a `.lock` file). The SIGFPE run left it at 0 bytes. Every later instance
   then died of SIGBUS on a background thread at `setupProcessing`, while reading the catalog header (`cmp
   $0x53434154`, "SCAT", at `lsp-plugins.so+0x2ddceb`). Reproduced: `: > /dev/shm/lsp-catalog-$USER.shm` makes the
   probe exit 135. Removing both files makes it exit 0 three times out of three.

Neither of the first two is a C-API limit. They are the obligations every VST3 host carries, and the SDK's C++
`hosting` classes would only have hidden the first one.

## Line count (scc 3.5.0 code lines, by section of `vst3_probe.c`)

| section | lines (file) | code |
|---|---|---:|
| header, includes | 1-32 | 14 |
| `IHostApplication` | 33-51 | 18 |
| `IMessage` + `IAttributeList` | 52-204 | 147 |
| memory `IBStream` | 205-262 | 56 |
| empty `IParameterChanges` + `IEventList` | 263-301 | 36 |
| helpers (UTF-16 to ASCII, levels, bus list) | 302-351 | 46 |
| `main`, the driving sequence | 352-533 | 160 |
| **total** | 533 lines | **477** |

lizard 1.24.1: 467 NLOC in 40 functions, mean CCN 3.4. `main` has CCN 32. The COM objects alone (host, message,
stream, parameter changes, event list) are 257 code lines. That is the part a real adapter would keep, with real
parameter queues and event lists in place of the empty ones.

Time spent: about two hours, including the three plugin quirks.
