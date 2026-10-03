# xemu

This fork uses the existing stable main tree as its accepted baseline foundation, including its known fixes and testing addons.

## Current measured comparison — 2026-10-02

**A:** upstream v0.8.136 **with matched guest-flip telemetry**. **B:** tested fork main revision `ee5ce48`, Vulkan 1×, ABBA then BAAB: **48 attempts**, four per build/game/host. The full report and raw artifacts live on the separate [evidence branch](https://github.com/Mainkill1/xemu/tree/evidence/fresh-start-performance-20261002); the report links pin its immutable archive commit. Values are descriptive observations from the existing runner, not a universal speedup or isolated PR attribution.

| Rig / scene | Guest flips/s A → B | Cadence change | xemu CPU core % A → B | Device GPU % A → B | p99 ms A → B |
| --- | ---: | ---: | ---: | ---: | ---: |
| Windows / PGR2 parked start | 14.66 → 30.00 | +104.67% | 316.7 → 321.2 | 37.7 → 43.5 | 83.18 → 34.00 |
| Windows / Conker bar menu | 29.18 → 30.00 | +2.80% | 252.6 → 248.2 | 52.2 → 56.1 | 50.06 → 33.75 |
| Windows / DOAXBV island menu | 60.00 → 60.00 | 0.00% | 280.6 → 247.4 | 39.5 → 45.0 | 17.49 → 17.04 |
| Steam Deck / PGR2 parked start | 7.38 → 7.15 | -3.18% | 299.8 → 403.5 | 30.2 → 57.8 | 242.72 → 335.92 |
| Steam Deck / Conker bar menu | 9.12 → 9.59 | +5.17% | 210.2 → 185.3 | 28.4 → 60.7 | 140.70 → 161.21 |
| Steam Deck / DOAXBV island menu | 42.74 → 47.95 | +12.19% | 189.6 → 196.3 | 42.5 → 59.2 | 37.13 → 26.51 |

<details>
<summary>Mean, worst maximum, p95 and p99 frame intervals</summary>

| Rig / scene | Build | Flips/s | Mean ms | Worst max ms | p95 ms | p99 ms | CPU core % | GPU device % |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Windows / PGR2 parked start | A | 14.66 | 68.60 | 101.00 | 80.46 | 83.18 | 316.7 | 37.7 |
| Windows / PGR2 parked start | B | 30.00 | 33.33 | 34.66 | 33.61 | 34.00 | 321.2 | 43.5 |
| Windows / Conker bar menu | A | 29.18 | 34.41 | 53.34 | 49.68 | 50.06 | 252.6 | 52.2 |
| Windows / Conker bar menu | B | 30.00 | 33.33 | 34.72 | 33.57 | 33.75 | 248.2 | 56.1 |
| Windows / DOAXBV island menu | A | 60.00 | 16.67 | 24.69 | 16.87 | 17.49 | 280.6 | 39.5 |
| Windows / DOAXBV island menu | B | 60.00 | 16.67 | 17.67 | 16.91 | 17.04 | 247.4 | 45.0 |
| Steam Deck / PGR2 parked start | A | 7.38 | 137.31 | 333.80 | 189.23 | 242.72 | 299.8 | 30.2 |
| Steam Deck / PGR2 parked start | B | 7.15 | 133.63 | 618.87 | 270.16 | 335.92 | 403.5 | 57.8 |
| Steam Deck / Conker bar menu | A | 9.12 | 110.36 | 213.83 | 132.85 | 140.70 | 210.2 | 28.4 |
| Steam Deck / Conker bar menu | B | 9.59 | 105.05 | 208.60 | 137.04 | 161.21 | 185.3 | 60.7 |
| Steam Deck / DOAXBV island menu | A | 42.74 | 23.51 | 191.20 | 30.23 | 37.13 | 189.6 | 42.5 |
| Steam Deck / DOAXBV island menu | B | 47.95 | 20.68 | 100.01 | 24.85 | 26.51 | 196.3 | 59.2 |

Mean/p95/p99 are medians of four per-attempt estimates. Worst maximum is the highest of the four per-attempt maxima, not a pooled percentile.

</details>

CPU 100% is one logical core. GPU is whole-device usage. Guest flips/s are not host-presented FPS. Frame mean/max/p95/p99, all changes over 1%, exact hardware/build/settings, all attempts and scene/cache limits are in the [full comparison](https://github.com/Mainkill1/xemu/blob/7014438cdcac1bb1889b250b5c757cc808ec8c37/docs/performance/2026-10-02-fresh-start-comparison.md).

| Rig | CPU / memory | GPU / driver |
| --- | --- | --- |
| Windows 10 | Ryzen 9 6900HX, 16 logical processors, 16 GB RAM | RTX 3070 Ti Laptop 8 GiB, NVIDIA 581.95 |
| Steam Deck / SteamOS | AMD Custom APU 0405, 8 logical processors, 16 GB RAM | RADV VANGOGH, Mesa 25.3.0 git 59b552c765 |

PGR2 remains parked with its unchanged input sequence. Conker uses the separately qualified two-A bar-menu startup revision; DOAXBV uses 90 s/A/5 s/A. The Deck starts PGR2/DOAXBV during transitions; scene animation and guest time are not identical across rigs. Windows driver cache is uncontrolled, and power/thermal state is not pinned. Deck PGR2 used 34.6% more CPU and had a 38.4% worse p99; Deck Conker also had a 14.6% worse p99 despite higher throughput. These regressions need investigation. [Evidence and all 48 run identities](https://github.com/Mainkill1/xemu/blob/7014438cdcac1bb1889b250b5c757cc808ec8c37/evidence/fresh-start-performance-2026-10-02/campaign.json).

<details>
<summary>Historical September 19 Windows comparison and XISO corrections</summary>

The Windows comparison below used the published `main` product build at `ef1a7fc4` and the [official upstream v0.8.136 release](https://github.com/xemu-project/xemu/releases/tag/v0.8.136). Values are two-run means from the same guest display-write trace source. Cadence is guest display writes per second; p95/p99 are intervals between those writes, **not displayed FPS**. Both roles used Vulkan at 1× scale with VSync off; the fork test profile explicitly enabled its Advanced controls and selected Ubershader Prewarm.

| Workload | Official upstream: cadence; p95 / p99 | Fork `main`: cadence; p95 / p99 |
| --- | ---: | ---: |
| PGR2 full start, in-race | 14.23/s; 83.03 / 91.09 ms | **29.99/s; 39.35 / 42.56 ms** |
| PGR2 snapshot | Incompatible fork VMState | 22.50/s; 52.11 / 56.80 ms |
| Morrowind snapshot | Incompatible fork VMState | 29.06/s; 42.41 / 46.54 ms |

In that PGR2 race, fork `main` had **110.8% higher guest-write cadence** and 52.6%/53.3% lower p95/p99 intervals than official upstream. This is a measured advantage in one scene, not a universal speedup; the scripted pre-race window did **not** establish a loading-time improvement. The fork also passed **157/157 XISO records twice**. Official upstream completed 35 PASS and 2 FAIL records in each attempt, then hit Vulkan device loss with 120 records unrun, so a full-suite timing comparison is unavailable.

**XISO timing correction:** the September 19 per-leaf speed percentages have been withdrawn. The XISO launcher did not explicitly set the fork's Advanced controls: the candidate used its Prewarm/Fastpath-On defaults, while the published main build used Off defaults. Live guest markers were also unavailable. The [XISO audit](docs/performance/2026-09-19-xiso-leaf-results.md) retains the raw leaf timings and functional outcomes, but those runs do not qualify a matched performance comparison. The PGR2 results above used a separate explicitly configured launcher.

The [matched XISO rerun](docs/performance/2026-09-19-xiso-matched-rerun.md) explicitly enabled Prewarm and shader fastpath on both existing fork executables in main–candidate–candidate–main order. All four runs passed 157/157 records. Among 150 framebuffer-hash-matched leaves, 54 favored the candidate, 95 favored main, and one tied; median candidate speedup was −0.59%. A repeated sampler-only texture leaf showed −4.62% candidate speedup, equivalent to 4.84% longer candidate latency. These are guest microbenchmark timings with live-marker correlation unavailable, so they are diagnostic rather than gameplay FPS or PR-grade timing proof. They do not show an across-the-board #135 improvement.

A separate third PGR2 full-start pass sampled resource usage at approximately 1 Hz. In-race xemu CPU averaged **3.08 core equivalents** on fork `main` versus **2.96** upstream. Whole-GPU utilization averaged **38.4%** versus **37.3%**; that device-wide counter is not specific to xemu and does not identify the performance bottleneck. The [per-run results and limitations](docs/performance/2026-09-19-official-upstream-comparison.md) include the separate profile, XISO timing definitions, and the draft #135 candidate comparison.

</details>

| Branch | Role |
| --- | --- |
| `upstream` | Exact mirror of `xemu-project/xemu:master` |
| `baseline` | Fixed cycle 01 reference at tag `baseline/cycle-01-start` |
| `main` | Accepted foundation plus changes admitted through PRs |
| `feature/*`, `fix/*`, `research/*` | Focused working branches from main |

At switchover, `main` and `baseline` point to the exact same commit. After that, accepted PRs and patches advance `main`; `baseline` advances only through a deliberate, infrequent re-baseline.

Read the [repository workflow](docs/repository-workflow.md), [baseline/results identity](docs/performance/baseline-selection.json), and [preservation inventory](docs/repository-normalization.md). Each accepted optimization is compared with both previous main and the fixed baseline. The [baseline release](https://github.com/Mainkill1/xemu/releases/tag/baseline-bd1fecb9-20260909) retains the existing binary and diagnostic symbols.

Full-Speed historically gathered performance experiments; Stable was intended to remove accidentally integrated performance work and recover a baseline. Those histories and their evidence remain preserved. Their names and earlier review descriptions do not override the owner's selection of the current main tree.

<details>
<summary>Historical Full-Speed report from baseline bd1fecb9 (preserved verbatim)</summary>

# Full Speed

`Full-Speed` is the consolidated research-integration branch for the xemu
changes reviewed after the original Full-Speed baseline (`6234f3606f`). It is
not a production or upstream-ready branch. Forgejo issues #37 and #38 found
correctness defects and validation gaps in both the rollup and its source
branches. Source branches remain available as experiment history; production
candidates must be extracted onto a clean target with their own tests.

This corrective branch is based on `Full-Speed` commit `111deac13f` and fixes
the immediately confirmed rollup defects: complete PTIMER reconciliation and
defined zero-ratio handling, all-CPU callback-retirement ordering, removal of
the unsafe host-load timer control, and removal of the persistent NVIDIA
maximum-power policy. The remaining `Hold` entries below still require the
listed differential or rendered-output gates.

## Included branches

| Branch | Status in Full-Speed | Change |
| --- | --- | --- |
| `feature/eng-2026-520-perf-etw-event-telemetry` | Diagnostic only | Adds opt-in NV2A retail-capture telemetry. Its event naming, disabled-path overhead, sampling, and placeholder fields must be corrected before it is trusted as proof. |
| `feature/eng-2026-525-vk-wait-telemetry-u524a4d5b` | Superseded by v2 | Introduced per-reason Vulkan submit/wait attribution. Its functional changes are retained through the v2 branch. |
| `feature/eng-2026-525-vk-wait-telemetry-v2` | Included | Reduces telemetry overhead by timing the first occurrences and periodic hot occurrences of each Vulkan wait reason. |
| `feature/eng-2026-523-vk-vertex-upload-staging` | Hold | Stages ordered vertex-RAM updates and removes measured waits, but capacity pressure, overlap, reset, and rendered-output validation remain required. |
| `feature/eng-2026-523-vk-cpu-hotpath-attribution` | Included | Attributes Vulkan draw-preparation CPU work in the opt-in telemetry output. |
| `feature/eng-2026-523-win-qpc-clock-fastpath` | Included | Avoids an unnecessary 128-bit divide in exact Windows QPC scaling. |
| `feature/eng-2026-523-win-nvidia-prefer-max-performance-u90829d4e` | Removed | Persistently changed driver power policy without a separate opt-in or rollback and could prevent the older profile settings from being saved. |
| `feature/eng-2026-523-tcg-active-mmu-dirty-reset-ud9e1e15a` | Hold | Skips empty MMU modes while resetting code-dirty state; requires isolated randomized equivalence coverage across active/victim modes and resize/flush transitions. |
| `feature/eng-2026-523-tcg-x86-static-state-tb-lookup-u3121717e` | Hold | Contains several independent TCG and memory fast paths. Each source change must be isolated and differential-tested before promotion. The cached callback lifetime defect is fixed on this corrective branch. |
| `feature/eng-2026-523-vk-native-bc-on-vertex-staging-u8a09a4ea` | Hold | Native BC layout arithmetic is covered, but native-versus-decoded Vulkan upload/sample output still needs comparison. |
| `xemu-pr-staging: feature/eng-2026-336-stage-local-vulkan-uniforms-v3-uec89c153` | Hold | Stage-local uniform updates need table-driven dependency coverage plus UBO-byte and rendered-output comparison against the always-update path. |
| `fix/eng-2026-523-vk-report-dma-ownership` | Included | Captures the report DMA object when a GL, Vulkan, or null-renderer query is queued, preventing a later context switch from writing the completed report through the wrong DMA mapping. |
| `feature/eng-2026-523-vk-texture-pipeline-fastpath-uaad84ed1` | Included | Keeps texture image/sampler changes on the descriptor path instead of forcing a Vulkan pipeline lookup; shader-affecting texture state still invalidates through `ShaderState`. The recorded Morrowind capture reduced pipeline lookups by 79.9%, and the fixed-work A/B comparison improved average throughput by 1.079%. |
| `fix/eng-2026-523-nv2a-ptimer-overdue-catchup-u76925787` | Corrected here | Coalesces missed epochs. This corrective branch uses one transition for callback, post-load, interrupt-read, and interrupt-enable paths, asserts exact future epochs, and treats a zero ratio as a stopped clock without changing the raw registers. |

## One-variable review and A/B branches

Do not use this combined corrective branch to attribute a performance change.
Use the isolated branch that owns the relevant behavior and compare it with
the exact parent recorded below. Small improvements remain valid findings, but
they must keep their own confidence level and correctness evidence.

| Concern | Isolated branch/head | Exact control | Purpose |
| --- | --- | --- | --- |
| PTIMER reconciliation and zero ratios | `fix/eng-2026-523-ptimer-reconciliation-zero-ratio-u3f16e05d` (`b332c16d6a`) | `Full-Speed` (`111deac13f`) | Correctness isolation; measure timing only as a separately proven lead. |
| Cached callback retirement | `fix/eng-2026-523-callback-retirement-order-u16a212b2` (behavior `82749ab7a6`) | `Full-Speed` (`111deac13f`) | From exclusive removal, queue every CPU's flush, the synchronization barrier, then deferred free. |
| Unsafe host-load option removal | `fix/eng-2026-523-remove-unsafe-host-load-ube3040c6` (`4140a916fb`) | `Full-Speed` (`111deac13f`), legacy option disabled | Establish the safe baseline without global timer changes. |
| Host-load feature replacement | `research/eng-2026-523-host-load-deadline-redesign-ufaef5c6e` (`676c7f3c84`) | Safe-removal branch (`4140a916fb`) | Preserve the valuable feature as a presentation-deadline-only opt-in design. |
| NVIDIA persistent PSTATE policy | `fix/eng-2026-523-remove-nvidia-power-policy-u83f2a080` (`ede650cf47`) | `Full-Speed` (`111deac13f`) | Keep driver state outside implicit emulator startup behavior. |
| Dirty TLB range helper inlining | `feature/eng-2026-523-tcg-dirty-range-inline-u1bfe153e` (`34791cc209`; behavior `4bcf7a95c4`) | `Full-Speed` (`111deac13f`) | Clean extraction of historical research commit `9fd812fb64`; independently reproduce its small CPU-path result before promotion. |
| Tiny-draw empty-TLB metadata access | `research/eng-2026-523-vk-tiny-draw-reuse-attribution-u42bc13ac` (`091dc945fd`; behavior `45fbf9e10c`) | Its behavior commit's immediate parent | Safety fix inside research history; extract again before performance promotion. |
| Batched-submit dependencies | `fix/eng-2026-523-vk-batched-submit-safety-u70dcad5c` (`2991e35eea`; behavior `84975084de`) | Rejected batched branch at `d13650db` | Synchronization repair only; neutral/slower result remains rejected. |

Telemetry semantics, each TCG/MMU optimization, vertex staging, texture
pipeline classification, native BC output, uniform invalidation, and
equivalent texture-scale suppression remain separate extraction tasks. They
must not be grouped into a new performance branch merely because they share
this investigation number. Each extracted branch must state A/B/C source
commits, release/debug builds, perf-lab XISO coverage, the relevant retail
fresh-boot or snapshot workload, and its hypothesis-specific metric.

### Corrective-branch validation to date

The corrected behavior head `390a00e694` builds successfully in both the
pinned release/LTO configuration and the assertion-enabled debug configuration
documented below. In each build,
`tests/unit/test-xbox-nv2a-ptimer.exe --tap -k` passed all 9 tests under Wine,
including callback, post-load, interrupt-read, interrupt-enable, exact
multi-epoch, and zero-ratio cases.

The first release XISO attempt against behavior head `2ed201fe4f` failed with
Windows `0xC0000005`. Exact release RVA `0x10d7bd` resolves to
`do_mem_access_callback_remove_by_ref()`: the initial repair freed the callback
in the caller before its asynchronous removal executed. Corrective commit
`390a00e694` now starts the all-CPU flush from inside the exclusive removal
callback, then queues the source-CPU synchronization barrier and deferred free.
This also prevents a remote flush from racing ahead of removal. The failed run
is retained as evidence.

The corrected Windows release/LTO executable SHA-256 is
`70b3a1236f770d52b62c363b59cc19b747797037a12941da23bbfbd32b5fac00`;
the debug executable SHA-256 is
`fd17490bf0ad2abc1ae82ea03d81326c3dbbbf12b758b885394cb6bf1d223dff`.
Both passed the complete 147-record perf-lab XISO with functional hashes,
Vulkan validation enabled, and zero VUIDs. The release and debug summary
SHA-256 values are respectively
`f463b5d74f7665245d71416fc6a396c5f773cadc8130a4886efc887ab9c05540`
and
`550b22e3b769a2cb84aa7435f4067ee89bb5f033d5a217a1b73478376e7b9282`.
Evidence is retained under
`evidence/eng523-branch-review/corrected-xiso-ab441f7/` in the lab working set.
These passes do not replace the purpose-built multi-vCPU callback churn test
or fresh-boot retail gates.

## Reproducing the Windows builds

No private compiler, patched SDK, or hidden branch-specific flag is required.
The authoritative upstream job is [`.github/workflows/build-windows.yml`](.github/workflows/build-windows.yml).
The lab reproduced its x86-64 release path on a Debian 13 x86-64 Docker host
(Docker 29.7.2); those host versions are descriptive, while the immutable
container below defines the compiler environment.

### Source identity

Use a clean checkout of the exact 40-character commit under test:

```bash
git clone --recurse-submodules <published-xemu-repository> xemu-full-speed
cd xemu-full-speed
git fetch --all --tags
git checkout <full-40-character-commit>
git submodule update --init --recursive
git status --porcelain=v1
git rev-parse HEAD
```

The status command must print nothing before the build. A binary produced by
copying changed files into another checkout is diagnostic only and is not
release-eligible.

### Pinned upstream toolchain

| Component | Exact value |
| --- | --- |
| Container tag | `ghcr.io/xemu-project/xemu-win64-toolchain-gcc:sha-2881edd` |
| Container digest | `sha256:09fdc183a88b493bf3a98d0d00b03aca4d5a23e60cc08228d7752d3c3295e8b2` |
| Base image | Ubuntu 24.04 |
| MXE source | `https://github.com/mxe/mxe.git` at `d9441093aa48e376aa6e49bcc7118ccc2b683a1e` |
| Target | `x86_64-w64-mingw32.static` |
| Compiler | GCC 16.1.0 |
| Linker | GNU binutils 2.46.0.20260210 |
| Meson selected by `build.sh` | 1.9.0 |
| Ninja | 1.13.2 |
| Python | 3.12.3 |

Pull by digest so a mutable tag cannot change the build environment:

```bash
docker pull \
  ghcr.io/xemu-project/xemu-win64-toolchain-gcc@sha256:09fdc183a88b493bf3a98d0d00b03aca4d5a23e60cc08228d7752d3c3295e8b2
```

### Release/LTO build used by the lab

Run from the clean source root. The cache affects build time only. `curl` is
installed because the pinned image does not include it and xemu's DSP fallback
downloads a versioned input while configuring.

```bash
mkdir -p .build-cache/ccache .build-cache/lto

docker run --rm \
  -e CROSSPREFIX=x86_64-w64-mingw32.static- \
  -e CROSSAR=x86_64-w64-mingw32.static-gcc-ar \
  -e CCACHE_DIR=/xemu-cache/ccache \
  -e CCACHE_MAXSIZE=512M \
  -e LTO_CACHE_DIR=/xemu-cache/lto \
  -v "$PWD:/src" \
  -v "$PWD/.build-cache:/xemu-cache" \
  -w /src \
  ghcr.io/xemu-project/xemu-win64-toolchain-gcc@sha256:09fdc183a88b493bf3a98d0d00b03aca4d5a23e60cc08228d7752d3c3295e8b2 \
  bash -lc 'apt-get update && apt-get install -qy curl && \
    ./build.sh -j32 -p win64-cross \
      --extra-cflags="-flto-incremental=/xemu-cache/lto -flto-partition=cache" \
      -Db_lto=true -Dx86_version=3'
```

This is the x86-64 release-equivalent path used for shared performance
binaries. The official workflow chooses the runner's concurrency instead of
the lab's `-j32`; that changes build duration, not generated semantics.

### Debug/assertion build used by the lab

Use a separate clean checkout or build directory with the same container:

```bash
./build.sh -j32 -p win64-cross --debug -Db_lto=false -Dx86_version=3
```

The official Windows workflow uses `--debug` for its debug matrix entry. The
lab also states `-Db_lto=false` and `-Dx86_version=3` explicitly so the
assertion build's configuration is unambiguous. Debug timings are not compared
against release/LTO timings.

`build/qemu-system-i386w.exe` is the unbundled executable. `build.sh` produces
`dist/xemu.exe` and `dist/LICENSE.txt`. Preserve `build.log`, the clean-source
result, the full command, and the exact source commit.

### Windows symbols and packaging

The upstream job uses public `cv2pdb` 0.52 from
`https://github.com/rainers/cv2pdb/releases/download/v0.52/cv2pdb-0.52.zip`.
The lab's `cv2pdb64.exe` SHA-256 is
`93b9033f24a9d671544c885bea29f825920199fb929cff9f1ae877b141f49184`.
Before conversion, retain the DWARF executable and create the sorted symbol
map with the matching container tool:

```bash
x86_64-w64-mingw32.static-nm -n \
  build/qemu-system-i386w.exe > xemu.map
```

Then run on Windows:

```powershell
Copy-Item xemu.exe xemu-dwarf.exe
cv2pdb64.exe xemu.exe
Get-FileHash -Algorithm SHA256 xemu.exe,xemu-dwarf.exe,xemu.pdb,xemu.map
```

Distribute the executable together with `LICENSE.txt`, matching PDB, original
DWARF executable, map, build log, source commit, build identity, and SHA-256
manifest. Never symbolize an address with artifacts from another executable.

### Required validation identity

Each result bundle must identify the emulator commit and hashes, renderer,
VSync state, output resolution, GPU/driver, Windows build, snapshot or fresh
boot path, and perf-lab XISO/catalog hashes. The current broad correctness gate
contains 147 records. PGR2 fresh-boot automation uses a 10-second BIOS delay,
then `A-3,A-10,A-2,A-2,F-2,A-2,A-2,A-2,A-2,A-2,A-7`; snapshot and fresh-boot
runs are different test modes and must not be presented as interchangeable.
Behavior changes are tested in both release and debug builds. The removed
host-load option must remain unavailable/off in normal runs.

## Reviewed research branches

These branches remain open and are recorded here for traceability. They were
not merged wholesale because they are measurements, experiments, or contain
explicitly reverted experiments rather than an additional validated production
change.

| Branch | Result |
| --- | --- |
| `research/eng-2026-523-always-stage-vertex-layout-safe-ue4c43cdc` | Evaluated staging vertex RAM without layout changes; the validated production staging work is represented by the vertex-upload branch above. |
| `research/eng-2026-523-sparse-uniform-layout-safe-u6299d05f` | Investigated safely skipping clean uniform rows; retained as research pending an independently validated landing. |
| `research/eng-2026-523-tcg-tb-lookup-attribution-u065f47f7` | Collected indirect TB-lookup evidence that informed the later TCG fast paths. |
| `research/eng-2026-523-vk-stalled-gpu-timestamps-uf1f85872` | Added diagnostic Vulkan timing experiments and records several reverted candidates; it is intentionally not used as a production rollup branch. |
| `feature/eng-2026-523-vk-aux-submit-fence-u70f4e0ef` | Replaces a queue-wide idle wait with an auxiliary-command-buffer fence. It passed focused validation, but its recorded end-to-end A/B result was neutral to slightly slower; it remains a candidate, not a rollup change. |
| `feature/eng-2026-523-vk-batched-aux-main-submit-u48cab4ee` | Batches the auxiliary and draw command buffers into one ordered submit. Its measured A/B result was neutral/slower, so it remains research. |
| `research/eng-2026-523-vk-report-boundary-submit-u688e8e71` | Records report-boundary submission experiments and remains open pending a reproducible end-to-end improvement. |
| `research/eng-2026-523-vk-tiny-draw-reuse-attribution-u42bc13ac` | Contains further tiny-draw and cache-attribution experiments; it is deliberately not merged wholesale. |

## Removed host-policy experiments

The `Reduce host CPU usage when VSync is off` option is not available on this
corrective branch. Its implementation coupled a display preference to global
QEMU timer precision and added an unconditional 1 ms display-loop delay. The
observed host-load reduction remains a valuable lead tracked by issue #37, but
it must return only as an explicit toggle based on a real presentation/event
deadline and must not change guest timer semantics. Do not use older builds
with this option enabled for correctness or performance comparison.

The NVIDIA maximum-performance P-state addition is also removed. The existing
NVIDIA OpenGL profile setup remains unchanged; xemu does not silently add a
new persistent maximum-power policy on this branch.

## Staging audit

The `xemu-pr-staging` production candidates were compared against this
rollup. The transient-buffer growth, changed-uniform-row, versioned-vertex,
dynamic-blend, and texture-dirty branches already have later validated
successors here. ENG-2026-336 was the remaining distinct production change;
its seven focused unit tests passed on the Linux dev box under Wine before it
was reconciled into this branch. Staging source branches remain open.

## Validation focus

Validate this rollup with the same snapshot and emulator configuration used by
the baseline. Compare guest-event median, p95, p99, and maximum duration;
Vulkan finish/wait time; ordered texture-upload and vertex-staging counters;
native-versus-decoded BC upload counters; and TCG CPU time. Keep the source
branches available for A/B comparison rather than closing or deleting them.
For the new work, also run `test-xbox-nv2a-ptimer`, verify report-query writes
against the DMA object active at queue time, and compare Vulkan pipeline
lookups plus draw-preparation CPU time on a texture-heavy saved-state capture.

</details>
