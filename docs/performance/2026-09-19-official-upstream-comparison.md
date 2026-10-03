# Windows comparison: official upstream, current main, and #135 candidate

On 2026-09-19, one Windows test rig ran two primary attempts of each applicable workload, all candidate attempts first, followed by the [official xemu v0.8.136 ZIP](https://github.com/xemu-project/xemu/releases/download/v0.8.136/xemu-win-x86_64-release.zip), then the already-published [current-main release](https://github.com/Mainkill1/xemu/releases/tag/xemu-upstream-f9b14039-main-ef1a7fc4-win64-20260919). Main was **not rebuilt**. The candidate combines rebased #135 texture-uniform changes and a separate Advanced-defaults change. **Correction:** the XISO launcher did not explicitly configure those controls, so its candidate/main timing comparison used different defaults. The PGR2 and Morrowind launchers did configure both fork builds identically.

| Role | Source commit | EXE SHA-256 |
| --- | --- | --- |
| Official upstream v0.8.136 | `fc24584ce88f0915ad7f04775bb7712c2e3f49ee` | `7da537938ea2ac09f894186ba793c9ae51dff37c95903b0002273b8d363818b7` |
| Current main release | `ef1a7fc4b62b033d4364f56c7c1822981935232a` | `f9fbc940e33994698ad43ea9c7ecf42790c0b3c53763c08f120a73fc3c485ffa` |
| #135 + defaults candidate | `cd3c30b6025d4ca8d8bf114d689892873fc40a71` | `f673bb0eb250e1e15ca9768a11a1ad5a06ae8f12d611cae9f7de0167edea0170` |

The rig used an AMD Ryzen 9 6900HX, NVIDIA RTX 3070 Ti Laptop GPU, NVIDIA driver 581.95, Vulkan at 1× surface scale, and VSync off. Validation, optional Vulkan telemetry, and Hybrid tracing were off. The PGR2 and Morrowind launchers explicitly enabled all fork Advanced controls, selected Ubershader **Prewarm**, and enabled shader caching. The XISO launcher only set display options; its candidate used Prewarm and shader fastpath defaults On, while main used both defaults Off. Official upstream has no corresponding fork controls. The same game media and input script were used within each workload. These are two samples per role, so small differences are descriptive, not statistical proof.

For PGR2 and Morrowind, **cadence** means guest NV2A display-write events per second; p95/p99 are intervals between those events. PGR2 values were recalculated from the *same raw QEMU trace event in every role*. The harness had mislabeled different sources as the same frame log; its original cross-role p95/p99 values are not used here. Guest writes are a progress proxy, **not displayed FPS**. The PGR2 pre-race window contains scripted menu waits, so it is not a pure loading-duration measurement.

## PGR2 full start

The 30-second in-race window completed in all six primary runs. Current main averaged **29.99 writes/s**, p95 **39.35 ms**, p99 **42.56 ms**; official upstream averaged **14.23 writes/s**, p95 **83.03 ms**, p99 **91.09 ms**. On this rig and scene, current main's cadence was 110.8% higher and its p95/p99 intervals were 52.6%/53.3% lower than official upstream. The #135 candidate was effectively level with current main in this window.

| Role/run | In-race writes/s | In-race p95 ms | In-race p99 ms | Pre-race writes/s | Pre-race p95 ms | Pre-race p99 ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Candidate 1 | 30.035 | 39.437 | 42.082 | 43.368 | 37.305 | 46.188 |
| Candidate 2 | 29.982 | 39.155 | 41.703 | 42.871 | 37.555 | 49.436 |
| Official upstream 1 | 13.961 | 84.773 | 96.487 | 42.667 | 36.209 | 46.044 |
| Official upstream 2 | 14.496 | 81.290 | 85.689 | 42.498 | 35.954 | 50.077 |
| Current main 1 | 30.018 | 38.965 | 42.349 | 43.534 | 37.110 | 43.497 |
| Current main 2 | 29.970 | 39.744 | 42.780 | 43.539 | 36.416 | 42.823 |

The approximately 50-second pre-race window was mostly menu/navigation and fixed scripted waits. Its cadence and tails do **not** establish that main loads faster; the candidate's pre-race p95 was slightly worse than main's.

### Separate CPU/GPU resource profile

After the two-run primary matrix, a third full-start pass per role sampled the xemu process and NVIDIA device at approximately 1 Hz. These are **resource-profile passes, not additional primary timing samples**. The in-race window yielded 28–29 samples per role.

| Role | Samples | xemu CPU, mean core equivalents | xemu CPU, mean of 16 logical CPUs | Whole-GPU mean utilization | Whole-GPU mean power |
| --- | ---: | ---: | ---: | ---: | ---: |
| Candidate | 28 | 3.029 | 18.93% | 38.75% | 31.16 W |
| Current main | 29 | 3.080 | 19.25% | 38.38% | 31.15 W |
| Official upstream | 28 | 2.955 | 18.47% | 37.32% | 32.17 W |

CPU is process-specific and includes all xemu threads; it does not identify a hot thread. The NVIDIA figures are for the **whole GPU**, not xemu alone, and may include unrelated system work. Near-equal aggregate CPU/GPU utilization alongside different guest cadence does not identify the bottleneck. The machine-readable release results include medians, maxima, working-set samples, and the exact metric scope; raw host logs remain private.

## PGR2 snapshot

Both candidate and main completed twice. Official v0.8.136 failed to load the fork snapshot in both attempts: VMState subsection `mcpx-apu/dsp-state/dma/dma_read_count` is missing. Therefore there is no valid official-upstream snapshot performance comparison.

| Role/run | Writes/s | p95 ms | p99 ms |
| --- | ---: | ---: | ---: |
| Candidate 1 | 22.853 | 51.183 | 58.225 |
| Candidate 2 | 22.775 | 51.657 | 55.585 |
| Current main 1 | 22.452 | 51.929 | 56.503 |
| Current main 2 | 22.549 | 52.282 | 57.096 |

Candidate mean cadence rose about 1.4%, while mean p99 was about 0.2% worse. This is mixed, not an across-the-board improvement.

## Morrowind snapshot

Both candidate and current main completed twice and passed the final-image check. The official release could not load the fork snapshot because of the same missing VMState subsection; after the deterministic PGR2 failures, only one Morrowind official attempt was made. There is no comparable official timing result.

| Role/run | Writes/s | p95 ms | p99 ms |
| --- | ---: | ---: | ---: |
| Candidate 1 | 29.099 | 42.038 | 47.051 |
| Candidate 2 | 28.749 | 42.643 | 47.823 |
| Current main 1 | 29.118 | 42.347 | 46.192 |
| Current main 2 | 29.001 | 42.480 | 46.893 |

Candidate mean cadence was about 0.47% lower, p95 essentially level, and p99 about 1.9% worse than main. Thus #135 does not meet the previously requested merge condition of an improvement across workloads. It remains draft while this result is investigated or consciously accepted.

## XISO suite: functional results; timing comparison withdrawn

The fork's candidate and current main each passed **157/157** records in both runs. Official v0.8.136 completed **35 PASS, 2 FAIL, 120 not reached** in each attempt, then hit `VK_ERROR_DEVICE_LOST` before suite completion. The two failures were `game_load.s3tc_sync_factor.dxt1_same_address_wait` and `game_load.s3tc_sync_factor.rgba8_same_address_wait`, both a source-precision tile readback mismatch. The last completed record was `game_load.s3tc_sync_factor.bc2_native_eligible`. This is a repeatable functional difference; a partial upstream run cannot yield a comparable full-suite timing result.

| Role/run | Records | Host wall s | Guest iterations/s | p95 leaf mean ms | p99 leaf mean ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| Candidate 1 | 157/157 PASS | 91.630 | 23.639 | 287.265 | 959.765 |
| Candidate 2 | 157/157 PASS | 91.146 | 23.511 | 287.093 | 961.677 |
| Current main 1 | 157/157 PASS | 91.548 | 23.628 | 285.048 | 963.680 |
| Current main 2 | 157/157 PASS | 91.112 | 23.505 | 285.768 | 968.619 |
| Official 1 | 35 PASS, 2 FAIL, 120 not reached | — | — | — | — |
| Official 2 | 35 PASS, 2 FAIL, 120 not reached | — | — | — | — |

XISO p95/p99 here are percentiles across 152 leaf-test **guest-average** latencies, not frame-time percentiles. Cadence is guest test iterations divided by summed guest-test duration. Live guest timing markers were unavailable. Because the two fork executables used different Advanced defaults, these recorded figures must not be used to accept or reject #135's performance.

The [per-leaf audit](2026-09-19-xiso-leaf-results.md) and [complete 152-leaf CSV](2026-09-19-xiso-leaf-comparison.csv) retain raw guest timings, record outcomes, and functional-hash match status. Official upstream reached 35 passing leaves; 2 failed, 4 other passing leaves had different functional hashes, and 117 were not reached. The earlier leaf speed percentages have been withdrawn because the fork settings were unmatched.

That [matched fork rerun](2026-09-19-xiso-matched-rerun.md) is now available. It used the same published executables with identical requested Advanced settings in main–candidate–candidate–main order. Its guest timing result remains diagnostic because live guest markers were unavailable; it does not show an across-the-board #135 improvement. Official upstream was not rerun in that matched fork comparison.

The [candidate prerelease](https://github.com/Mainkill1/xemu/releases/tag/xemu-upstream-f9b14039-main-ef1a7fc4-pr135-cd3c30b6-20260919) retains the exact stripped executable, separate symbols, source identity, and machine-readable aggregate results. Raw traces, screenshots, game data, and private host paths are deliberately excluded.
