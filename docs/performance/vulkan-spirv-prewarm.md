# feature/vulkan-spirv-prewarm

**Status:** Draft — repaired-head Windows build and focused native/sanitizer tests pass; gameplay/profiling retest pending; performance HOLD\
**PR:** https://github.com/Mainkill1/xemu/pull/70\
**Cause diagnostic:** https://github.com/Mainkill1/xemu/pull/68\
**Post-integration ubershader design:** https://github.com/Mainkill1/xemu/pull/71 — blocked until #70 is qualified and merged\

**Stable baseline:** `9f618d6d8c4c446ef023955f3d4de22f661f61a4` / retained product executable SHA-256 `3489fdcc593e942b92a612bf35a98f509ff0907e3370e1e5f45f2972d83fb16b`\
**Previous main:** `e18ba8d6274cf227cc9e5ae1b5684f28ed911a99` / tree `3826f39bafd751596436958da355ca87e5b98c87`\
**Reused previous-main binary:** built from `19944268d97ecd92f2dcd820d6e151107833795b` / tree `e4d305254f64b92d1c374413bda12567874d7c14` / executable SHA-256 `13f61e7655a7b37ea51c282335b7540b48e92dc5980af0877be2e968eb571d9a`. Its only source-tree difference from previous main is the performance PR template; runtime source is equivalent.\
**Historical tested runtime:** `b14bfb745870faae500a1ecb0ff49d2143ba8bd1` / tree `3a79dd692d9e7f1089fa0b138d07fc4269fbd704` / `xemu.exe` SHA-256 `d6c0762fd672932667537b7152bf4068a7c313d2980d4b545e020e852064bc9d`

**Tested repair build source:** `b111a9612911c9c170d661974852a46f3ad336b8` (tree `ae2cf13138c75cb2a63d5357561ad2800db92670`); **repair source:** `01028d6db68c454e502f89b93383797e7a367553`, including lifecycle fix `ee00a1d21c`. **Repaired executable SHA-256:** `e28c88fc44c5454f93d6af0a46010b562ba50dd2bb8ab77d8208514785d22ae6`. [Build, symbols and focused test receipts](https://github.com/Mainkill1/xemu-perf-tests/blob/251663c919f047d41129767c395d70c8a0fd5fdd/docs/evidence/pr70-spirv-prewarm-20260910/review-repair/README.md). All gameplay and performance tables below describe the older `b14bfb74` executable. They do not qualify the repaired head.

**Evidence:** [Dedicated PR70 evidence PR](https://github.com/Mainkill1/xemu-perf-tests/pull/25) and [historical run manifest](https://github.com/Mainkill1/xemu-perf-tests/blob/e669ec45b41722cab93d5e7fd061c2012d84993d/docs/evidence/pr70-spirv-prewarm-20260910/full-qualification/manifest.json).

## Review repairs and current gates

| Review item | Current disposition | Remaining validation |
| --- | --- | --- |
| Partial uniform cleanup | One guarded cleanup routine handles missing metadata and partial names | Native failed-construction/recovery path |
| Successful destruction / #69 | Same routine now frees names, metadata, and backing storage for uniforms and push constants | Native module-cache eviction and renderer recreation |
| Structural cache tests | Valid enclosing checksums; positive control and transactional rejection; coverage reaches deeper guards | Native cache fallback and lifecycle matrix |
| Focused checks | Six ownership cases and 13 cache groups pass under ASan/UBSan/LSan; exact Windows build and both native units pass | Full-emulator gameplay and lifecycle retest |
| Evidence isolation | PR70 evidence copied byte-identically into its own branch/PR; source history retained | Keep all subsequent PR70 results on that branch |
| Performance | Prior full-start tail movements remain unresolved | Paired repetitions with glslang, reflection, module creation, and pipeline creation timed separately |

These tests exercise the production cleanup helper and cache implementation. They are not full-emulator allocation-failure injection, GPU/gameplay qualification, or a measured lifecycle performance improvement. Issue #69 remains open until the broader lifecycle gate is satisfied.

---

## Summary

**Historical result:** The exact `b14bfb74` Windows executable passed the focused native admission, 157-record XISO runs on OpenGL and Vulkan cold/warm with the documented inherited non-pass cases, and candidate gameplay admission for PGR2 and Morrowind. Every accepted warm Vulkan cell loaded its cold cache, reported zero misses/rejections/fallbacks, queued no new bytes, and left the cache file byte-identical within that profile. A completed exploratory 120-second user-driven PGR2 race expanded the cold cache to 221 records and then served 484 warm hits with zero misses.

Performance qualification is still **on HOLD**. Across two automated PGR2 full-start Vulkan captures, candidate warm p95, p99, and maximum intervals regressed **-5.833%**, **-14.241%**, and **-18.744%** against the mean of two previous-main controls. Cold p95, p99, and maximum also regressed **-2.432%**, **-11.432%**, and **-17.136%**. These fixed-route captures are variable and do not establish cache causality, but they exceed the >2% hold threshold and cannot be discarded.

**Headline:** The persistent cache demonstrably converts known-source warm launches to in-memory SPIR-V hits: automated PGR2 full start reported `338/0` and `341/0` hits/misses in two runs, the broader manual race `484/0`, PGR2 snapshot `141/0`, Morrowind snapshot `49/0`, and full XISO `71/0`. This proves reuse; it does not yet prove an end-to-end performance improvement or eliminate other stall sources.

**Next:** Run the repaired-head gameplay matrix, then reproduce or attribute the controlled PGR2 full-start tail regression and finish the pending native UI lifecycle/failure and complete resource gates. No matching 60-second Morrowind control exists in the retained evidence; the available 20-second controls remain historical context, and a same-duration comparison remains pending. The completed manual race is exploratory broader-route shader coverage: user driving is less repeatable, and one previous-main → cold → warm sequence cannot clear the controlled HOLD or prove coverage of the whole map.

---

# Investigation

## Why This Patch Exists

The completed PR #68 diagnostic recorded 141 Vulkan stage-module misses and 463.419 ms of glslang work during one PGR2 snapshot run. The primary hitch at guest frame 673 contained two vertex and two fragment misses. All four were first-seen generated sources in that process and consumed 18.217 ms in glslang.

Twenty other misses regenerated stage/source identities already seen under a different state key, costing another 68.200 ms in the PR68 diagnostic. PR70 cold admission recorded 121 misses and 20 hits, consistent with avoiding those same-process duplicate-source compilations. Warm admission recorded zero misses and 140 hits. Thus the artifact store provides both same-process deduplication and cross-run reuse. The 68.200 ms is a historical attribution, not a directly measured PR70 saving; the 140/141 lookup difference also means the runs are not identical event streams.

## Patch Hypothesis

Load a bounded SPIR-V artifact index during Vulkan renderer initialization. On a graphics shader-module cache miss, generate the exact GLSL as before and look up a record identified by stage, exact source bytes, shader-generator/cache ABI, target environment, and effective compiler/debug options. A validated hit supplies SPIR-V from memory. A miss uses glslang and queues an immutable artifact for one producer-quiesced atomic write outside gameplay.

The candidate retains synchronous GLSL generation, Vulkan module creation, and reflection. In the diagnostic primary frame those regions took 0.797 ms, 0.043 ms, and 1.269 ms respectively, compared with 18.217 ms in glslang. The expected warm-run benefit is removal of that compilation work for exact artifact hits.

Risks include corrupt or stale input, mismatched stage/compiler identity, unbounded storage, draw-path file I/O, partial Vulkan/reflection state, destructive live-toggle behavior, shutdown data loss, and persistent-cache resource growth.

---

# Processing Flow

## Current / Before

On every process-first shader source, a graphics shader-module state-key miss generates exact GLSL, runs glslang synchronously, creates the Vulkan module, reflects uniforms, and then resumes the blocked draw.

## Candidate / After

Eligible Vulkan renderer initialization reads and validates the bounded cache once. A graphics shader-module state-key miss still generates exact GLSL, then checks the in-memory artifact index. An exact hit reuses validated SPIR-V; a miss runs glslang and queues the artifact. Both paths share Vulkan module creation and reflection. Dirty cache state is written atomically after producer mutations quiesce.

### Processing Difference

| Area | Current | Candidate | Expected Effect |
| --- | --- | --- | --- |
| Draw-path compilation | glslang on every process-first source | In-memory lookup on a previously compiled exact source, within or across runs | Remove measured compiler stall |
| File I/O | None | Initialization read and shutdown write | No module-miss/gameplay-path file I/O |
| GPU work | Create a Vulkan module from fresh SPIR-V | Create a Vulkan module from accepted cached or fresh SPIR-V through one path | Equivalent device-local construction |
| Memory/disk | In-process modules only | Bounded renderer-local artifact index and one bounded file | Controlled persistence cost |
| Failure | Compile on the current path | Reject invalid cache data and compile through the same path | Preserve rendering fallback |

---

# Code Changes

- Add one little-endian cache under the settings-owned `cache/vulkan` directory. The filename identifies the selected Vulkan/SPIR-V target (`spirv-v1-vk11-spv13.bin`, `spirv-v1-vk12-spv15.bin`, or `spirv-v1-vk13-spv16.bin`) and keeps debug-shader artifacts in a separate `-debug` file, while the file identity also includes the cache ABI, generator-policy revision, glslang semantic version/flavor, every effective compiler-policy flag, shader stage, and exact GLSL bytes. A bounded hash index accelerates lookup; full source bytes remain authoritative.
- Independently cap the cache at 4,096 records, 1 MiB per source, 1 MiB per SPIR-V module, 16 MiB aggregate source, 32 MiB aggregate SPIR-V, and 64 MiB for the complete file. At capacity, evict the least-recently used artifacts so a later title can still populate the cache. Validate SPIR-V length, word alignment, magic, target version, allocation bound, instruction framing, graphics stage, entry point, descriptor/member counts, and checked layout arithmetic before an entry becomes usable.
- Read and transactionally validate once during eligible Vulkan renderer initialization. Graphics module misses generate GLSL as before and perform only an in-memory artifact lookup.
- Route cached and freshly compiled SPIR-V through one fallible Vulkan-module/reflection constructor. A cached construction failure removes the entry, unwinds partial state, compiles normally, and queues the successful artifact.
- Serialize the replacement in memory and publish through a unique temporary file plus atomic rename only after producer mutations quiesce. Write/rename failure preserves the old cache and remains retryable. Renderer switch/finalize synchronously flushes dirty state before destruction.
- Gate lookup, add, and writeback on the live shader-cache setting. Turning the setting off stops cache activity immediately. A renderer created while caching is disabled remains ineligible until renderer reinitialization, preventing an unseeded store from replacing an unseen existing cache.
- Emit one bounded aggregate summary after producer quiescence. Shader-module misses perform no synchronous logging.

**Main code path:** `hw/xbox/nv2a/pgraph/vk/glsl.c`, `shaders.c`, `spirv-prewarm.c`, and Vulkan renderer initialization/shutdown.

**Implementation commit:** `b14bfb745870faae500a1ecb0ff49d2143ba8bd1`

---

# Profiling

## Baseline Bottleneck

| Measurement | Stable Baseline | Previous Main Diagnostic | Current Candidate |
| --- | ---: | ---: | ---: |
| PGR2 primary-frame glslang | Not recorded | 18.217 ms | No direct region timing in qualification build |
| PGR2 diagnostic-run glslang | Not recorded | 463.419 ms | No direct region timing in qualification build |
| First stage/source identities | Not recorded | 121 | Persisted as reusable artifacts in exercised snapshot route |
| Different-key/same-source compiles | Not recorded | 20 | Exact source identity permits reuse |

## Exact-Head Warm Reuse

| Workload | Cache file | Warm hits | Warm misses | Rejects | Fallbacks | Queued bytes | Warm file result |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| PGR2 full start, run 1 | 4,839,817 B | 338 | 0 | 0 | 0 | 0 | Byte-identical |
| PGR2 full start, run 2 | 4,839,817 B | 341 | 0 | 0 | 0 | 0 | Byte-identical |
| PGR2 exploratory manual race | 5,418,200 B | 484 | 0 | 0 | 0 | 0 | Byte-identical |
| PGR2 snapshot | 3,042,552 B | 141 | 0 | 0 | 0 | 0 | Byte-identical |
| Morrowind snapshot | 1,307,654 B | 49 | 0 | 0 | 0 | 0 | Byte-identical |
| Full XISO | 1,146,671 B | 71 | 0 | 0 | 0 | 0 | Byte-identical |

**Bottleneck status:** The exact-head aggregate counters prove that accepted warm hits bypassed cache misses for the exercised source sets. They do not time glslang directly and do not establish an end-to-end frame-time win.

**New limiting path:** GLSL generation, hash/lookup, Vulkan module creation, reflection, and other renderer/game timing remain synchronous or unchanged.

---

# Performance Results

## Improvement Convention

Every percentage below is **Improvement %**: positive is favorable and negative is unfavorable.

| Raw + | Raw metric direction | Improvement % |
| --- | --- | --- |
| `+good` | Higher is better, such as FPS | `100 × (candidate / reference - 1)` |
| `+bad` | Lower is better, such as frame interval | `100 × (reference - candidate) / reference` |
| `N/A` | Context only or zero reference | No percentage claim |

The accepted automated package does not contain a same-session stable-baseline cell. For matching 60-second PGR2 protocols only, the stable columns reuse published fixed-baseline values as **historical context** from `ptimer-main-qualification/pgr2-fresh-baseline` and `ptimer-main-qualification/pgr2-snapshot-baseline`. Previous-main comparisons use controls from the accepted automated campaign. The 120-second manual campaign has no fixed-baseline reference, and no 20-second Morrowind result is compared with the current 60-second capture.

## PGR2 Full Start — Automated 60-Second Fixed Route

The reference is the arithmetic mean of previous-main controls B3 and B4. Candidate values are means of two independent cold or warm captures. Averaging the repeated percentile observations is used only to summarize this small campaign; the raw ranges are retained below.

| Phase | Metric | Raw + | Stable | Previous-main mean | Candidate mean | Improvement vs Stable | Improvement vs Previous Main |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: |
| Cold | FPS | `+good` | 30.000 | 30.000 | 29.928 | -0.241% historical | **-0.241%** |
| Cold | average interval | `+bad` | 33.333 ms | 33.333 ms | 33.408 ms | -0.223% historical | **-0.223%** |
| Cold | p95 interval | `+bad` | 33.412 ms | 33.431 ms | 34.244 ms | **-2.489% historical** | **-2.432%** |
| Cold | p99 interval | `+bad` | 34.497 ms | 34.211 ms | 38.122 ms | **-10.508% historical** | **-11.432%** |
| Cold | maximum interval | `+bad` | N/A | 36.347 ms | 42.576 ms | N/A | **-17.136%** |
| Cold | ≥75 ms stalls | `N/A` | N/A | 0 | 0 | N/A | N/A — both zero |
| Warm | FPS | `+good` | 30.000 | 30.000 | 29.864 | -0.454% historical | **-0.454%** |
| Warm | average interval | `+bad` | 33.333 ms | 33.333 ms | 33.473 ms | -0.420% historical | **-0.420%** |
| Warm | p95 interval | `+bad` | 33.412 ms | 33.431 ms | 35.381 ms | **-5.892% historical** | **-5.833%** |
| Warm | p99 interval | `+bad` | 34.497 ms | 34.211 ms | 39.083 ms | **-13.294% historical** | **-14.241%** |
| Warm | maximum interval | `+bad` | N/A | 36.347 ms | 43.160 ms | N/A | **-18.744%** |
| Warm | ≥75 ms stalls | `N/A` | N/A | 0 | 0 | N/A | N/A — both zero |

Raw repeat ranges:

| Phase | p95 | p99 | Maximum |
| --- | ---: | ---: | ---: |
| Previous main B3/B4 | 33.425–33.436 ms | 34.174–34.248 ms | 34.368–38.326 ms |
| Candidate cold 1/2 | 33.652–34.835 ms | 34.341–41.903 ms | 35.063–50.088 ms |
| Candidate warm 1/2 | 33.403–37.358 ms | 34.257–43.909 ms | 35.691–50.629 ms |

**Result:** **HOLD.** Both cold and warm means cross the >2% unfavorable threshold in p95, p99, and maximum interval. The large between-run variance prevents a causal attribution to SPIR-V reuse, but it does not permit a performance-pass claim.

The published stable values above are historical 60-second context and have no maximum/stall samples. They do not replace same-session controls.

## PGR2 Exploratory Manual Race — 120 Seconds

This was one user-driven sequence in the order previous main → candidate cold → candidate warm. Measurement began after the final automated race-confirmation input and a fixed seven-second delay; there was no guest-event race-start detector. It exercised a broader route and produced 221 cached shader records, 25 more than either 196-record automated full-start cold profile. Driving line, speed, traffic, and track position differed across cells, so its favorable sample is exploratory coverage rather than a controlled performance qualification or proof that the whole map is covered. Its only direct performance reference is the 120-second previous-main cell from this sequence; the published 60-second fixed baseline is `N/A`.

| Phase | Metric | Raw + | Previous main | Candidate | Improvement vs Previous Main |
| --- | --- | --- | ---: | ---: | ---: |
| Cold | FPS | `+good` | 29.155 | 29.463 | +1.054% |
| Cold | average interval | `+bad` | 34.262 ms | 33.944 ms | +0.928% |
| Cold | p95 interval | `+bad` | 41.817 ms | 39.741 ms | +4.964% |
| Cold | p99 interval | `+bad` | 49.569 ms | 46.010 ms | +7.180% |
| Cold | maximum interval | `+bad` | 182.864 ms | 74.022 ms | +59.521% |
| Cold | ≥75 ms stalls | `+bad` | 1 | 0 | +100.000% |
| Warm | FPS | `+good` | 29.155 | 29.600 | +1.525% |
| Warm | average interval | `+bad` | 34.262 ms | 33.776 ms | +1.420% |
| Warm | p95 interval | `+bad` | 41.817 ms | 37.773 ms | +9.671% |
| Warm | p99 interval | `+bad` | 49.569 ms | 46.694 ms | +5.800% |
| Warm | maximum interval | `+bad` | 182.864 ms | 76.365 ms | +58.239% |
| Warm | ≥75 ms stalls | `+bad` | 1 | 1 | +0.000% |

The warm cell loaded the exact 5,418,200-byte cold cache and reported 484 hits, zero misses, zero rejections, zero fallbacks, and no queued bytes. It nevertheless recorded a 76.365 ms maximum and one ≥75 ms stall. Reuse works, but other stalls remain; this untraced sample cannot attribute them to the driver, scheduler, or another subsystem. The favorable manual comparison does not erase the controlled automated full-start HOLD.

## PGR2 Snapshot — Automated Bracketed Route

The reference is the mean of previous-main B1 and B2 surrounding the candidate cold/warm cells.

| Phase | Metric | Raw + | Stable | Previous-main mean | Candidate | Improvement vs Stable | Improvement vs Previous Main |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: |
| Cold | FPS | `+good` | 29.012 | 28.905 | 28.889 | -0.425% historical | -0.055% |
| Cold | average interval | `+bad` | 34.484 ms | 34.627 ms | 34.588 ms | -0.303% historical | +0.113% |
| Cold | p95 interval | `+bad` | 40.910 ms | 41.668 ms | 41.358 ms | -1.095% historical | +0.744% |
| Cold | p99 interval | `+bad` | 45.186 ms | 46.253 ms | 45.182 ms | +0.009% historical | +2.316% |
| Cold | maximum interval | `+bad` | N/A | 68.522 ms | 69.208 ms | N/A | -1.001% |
| Cold | ≥75 ms stalls | `N/A` | N/A | 0 | 0 | N/A | N/A — both zero |
| Warm | FPS | `+good` | 29.012 | 28.905 | 28.904 | -0.374% historical | -0.003% |
| Warm | average interval | `+bad` | 34.484 ms | 34.627 ms | 34.591 ms | -0.312% historical | +0.104% |
| Warm | p95 interval | `+bad` | 40.910 ms | 41.668 ms | 41.361 ms | -1.102% historical | +0.737% |
| Warm | p99 interval | `+bad` | 45.186 ms | 46.253 ms | 47.149 ms | **-4.344% historical** | **-1.937%** |
| Warm | maximum interval | `+bad` | N/A | 68.522 ms | 58.829 ms | N/A | +14.146% |
| Warm | ≥75 ms stalls | `N/A` | N/A | 0 | 0 | N/A | N/A — both zero |

**Result:** Against the same-campaign previous-main bracket, results are mixed and within the 2% unfavorable band; cold p99 improved +2.316%, warm maximum improved +14.146%, and warm p99 moved -1.937%. Against the reused historical stable reference, warm p99 is -4.344%. The historical comparison lacks same-session control and maximum/stall data, but the unfavorable result is retained as context. This single bracket does not override the full-start HOLD.

## Morrowind Snapshot — Candidate Coverage

The accepted package contains candidate OpenGL, Vulkan cold, and Vulkan warm 60-second cells, but no matching previous-main or stable comparator. The retained PR11/PR14 Morrowind controls are 20 seconds and therefore protocol-incompatible. No retained matching 60-second control exists, and no additional capture was authorized. These values establish snapshot coverage and cache behavior only.

| Phase | Renderer | FPS proxy | Average | p95 | p99 | Maximum | ≥75 ms stalls |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Candidate | OpenGL | 33.619 | 29.745 ms | 36.461 ms | 41.635 ms | 47.183 ms | 0 |
| Candidate cold | Vulkan | 24.353 | 41.063 ms | 47.940 ms | 54.050 ms | 61.224 ms | 0 |
| Candidate warm | Vulkan | 24.168 | 41.376 ms | 47.762 ms | 53.696 ms | 61.169 ms | 0 |

Warm versus cold is diagnostic context only: FPS proxy **-0.756%**, average **-0.762%**, p95 **+0.371%**, p99 **+0.655%**, and maximum **+0.090%** under positive-good semantics. Cross-renderer comparison is not valid, and no baseline qualification is claimed.

## Candidate OpenGL Context

The accepted package has candidate-only OpenGL cells. Matching 60-second PGR2 fixed-baseline values are reused as historical context; there is no same-session previous-main control. The retained Morrowind reference is 20 seconds and remains excluded from qualification.

| Workload | Metric | Raw + | Historical stable | Candidate | Improvement |
| --- | --- | --- | ---: | ---: | ---: |
| PGR2 full start | FPS | `+good` | 29.991 | 30.000 | +0.030% |
| PGR2 full start | average interval | `+bad` | 33.343 ms | 33.333 ms | +0.028% |
| PGR2 full start | p95 interval | `+bad` | 33.822 ms | 33.381 ms | +1.304% |
| PGR2 full start | p99 interval | `+bad` | 36.059 ms | 34.316 ms | +4.834% |
| PGR2 full start | maximum interval | `N/A` | N/A | 36.763 ms | N/A |
| PGR2 full start | ≥75 ms stalls | `N/A` | 0 | 0 | N/A — both zero |
| PGR2 snapshot | FPS | `+good` | 28.519 | 28.481 | -0.134% |
| PGR2 snapshot | average interval | `+bad` | 35.122 ms | 35.117 ms | +0.015% |
| PGR2 snapshot | p95 interval | `+bad` | 42.308 ms | 42.686 ms | -0.893% |
| PGR2 snapshot | p99 interval | `+bad` | 46.086 ms | 46.169 ms | -0.180% |
| PGR2 snapshot | maximum interval | `N/A` | N/A | 60.825 ms | N/A |
| PGR2 snapshot | ≥75 ms stalls | `N/A` | 0 | 0 | N/A — both zero |

All reported historical PGR2 OpenGL comparisons are within 2% or favorable. They remain historical context because the fixed baseline was reused, not rerun.

---

# XISO Results

**Scope:** Full 157-record correctness catalog\
**Candidate executable:** SHA-256 `d6c0762fd672932667537b7152bf4068a7c313d2980d4b545e020e852064bc9d`\
**Test source:** commit `61012b4e702fbb46a02d813e71f2159a109a1c29`, tree `f5499b106caba6edf79ab3f7445a46503708121c`, nxdk `73c95900965a16be3a3e34b8d4d5d41bc18498be`, catalog `sha256:a0d41d33c1f5da2ba60db7102b094847df048d489ee2a7d7c72d9c6d0048a86e`\
**Image SHA-256:** `6b2161f1b4abab94648f3fa0ca8da893092bab1b63d7e139a2358eb0319d05ac`

| Cell | Records | Functional hash | Cubemap oracle | Vulkan validation | Cache result | Receipt status |
| --- | ---: | --- | --- | --- | --- | --- |
| Candidate OpenGL | 157 | PASS | Expected OpenGL FAIL, framebuffer `0a68f0aa371576a5` | Disabled | N/A | PASS with admitted expected non-pass records |
| Candidate Vulkan cold | 157 | PASS | PASS, framebuffer `be0f2013c1997ca5` | Active; 0 unique VUIDs | Published 61 records / 1,146,671 B | PASS with admitted expected non-pass record |
| Candidate Vulkan warm | 157 | PASS | PASS, framebuffer `be0f2013c1997ca5` | Active; 0 unique VUIDs | 71 hits / 0 misses; exact file unchanged | PASS with admitted expected non-pass record |

The runner returned exit code `1` for all three cells because the admitted catalog still records expected non-pass outcomes. OpenGL recorded `report_query.dma_range_guard` plus the known expected `texture_cubemap_fallback.unbordered_subblock_dxt1` failure. Vulkan cold/warm recorded only `report_query.dma_range_guard`; their cubemap oracle passed. The overall exact-head receipt status is `passed`, and every cell cleanup passed.

The retained previous-main same-suite receipt used the same 157-record test revision. It records the same inherited query-range failure, zero Vulkan VUIDs, and the expected renderer-specific cubemap result. This supplies a correctness comparator; its duration data remains context-only.

XISO durations are correctness context only. The ordinary Release binary lacked live markers, so the generated duration tables do not support a PR-grade performance comparison and are excluded from the performance decision.

---

# Resource Results

| Workload | Records | Source + SPIR-V payload | File bytes | Cold write | Warm loaded bytes | Warm write |
| --- | ---: | ---: | ---: | --- | ---: | --- |
| PGR2 full start, run 1 | 196 | 4,833,466 B | 4,839,817 B | Published | 4,839,817 B | Clean |
| PGR2 full start, run 2 | 196 | 4,833,466 B | 4,839,817 B | Published | 4,839,817 B | Clean |
| PGR2 exploratory manual race | 221 | 5,411,049 B | 5,418,200 B | Published | 5,418,200 B | Clean |
| PGR2 snapshot | 121 | 3,038,601 B | 3,042,552 B | Published | 3,042,552 B | Clean |
| Morrowind snapshot | 48 | 1,306,039 B | 1,307,654 B | Published | 1,307,654 B | Clean |
| Full XISO | 61 | 1,144,640 B | 1,146,671 B | Published | 1,146,671 B | Clean |

The manual receipt includes process CPU and memory samples. Positive is favorable; all resource metrics are `+bad`.

| Manual 120-second resource metric | Previous main | Candidate cold | Cold improvement | Candidate warm | Warm improvement |
| --- | ---: | ---: | ---: | ---: | ---: |
| Samples | 207 | 208 | N/A | 208 | N/A |
| Normalized host CPU | 21.920% | 22.185% | -1.208% | 22.015% | -0.429% |
| Working-set average | 1,011.860 MiB | 1,011.753 MiB | +0.011% | 997.964 MiB | +1.373% |
| Working-set peak | 1,027.652 MiB | 1,022.934 MiB | +0.459% | 1,026.125 MiB | +0.149% |
| Private bytes average | 2,968.419 MiB | 2,972.078 MiB | -0.123% | 2,969.173 MiB | -0.025% |
| Private bytes peak | 2,979.383 MiB | 2,980.160 MiB | -0.026% | 2,993.980 MiB | -0.490% |

These one-cell resource observations are exploratory and route-sensitive. GPU telemetry was `null` in the original campaign receipt. A separately recovered sampler fragment provides the following raw context:

| Manual cell | Sampler coverage | GPU average / peak | VRAM average / peak | Power average / peak | Temperature average / peak |
| --- | ---: | ---: | ---: | ---: | ---: |
| Previous main | 83.359% | 29.985% / 51% | 882.015 / 885 MiB | 22.270 / 24.52 W | 47.455 / 49 °C |
| Candidate cold | 83.183% | 33.273% / 51% | 882.015 / 885 MiB | 22.151 / 26.14 W | 49.449 / 50 °C |
| Candidate warm | 83.250% | 30.955% / 53% | 882.015 / 885 MiB | 22.809 / 24.19 W | 50.460 / 51 °C |

These rows cover only about 83% of each 120-second window. The runner force-stops the sampler; lost buffered output is a suspected mechanism that still needs confirmation. GPU utilization, VRAM, power, and temperature are partial context and unavailable as completed qualification metrics; no Improvement % is assigned. Startup cache-read time and shutdown cache-write time are also unavailable.

The manual cold cache SHA-256 was `a20d080c3e26ebc0384b93fd7910c4a4b64a3fcd99aacf2ae4d0c0f4e329f07f`, and warm preserved it exactly. The two independent automated PGR2 full-start profiles produced files with the same byte count and record/payload totals but different SHA-256 values. Each paired warm run preserved its own cold file exactly. Cross-run serialization determinism is therefore not established.

---

# Correctness and Lifecycle

| Check | OpenGL | Vulkan |
| --- | --- | --- |
| Focused cache-format/failure test | Not applicable | PASS in exact Windows build |
| Full 157-record XISO functional hashes | PASS | PASS cold and warm |
| Previous-main same-suite XISO comparator | PASS | PASS; same inherited query-range failure and 0 VUIDs |
| Full XISO cubemap oracle | Known expected OpenGL failure | PASS cold and warm |
| Validation errors | Validation disabled | 0 unique VUIDs cold and warm |
| PGR2 gameplay admission | PASS candidate full/snapshot cells | PASS cold/warm full and snapshot cells |
| Morrowind snapshot admission | PASS | PASS cold and warm |
| Warm cache use | Not applicable | Zero misses/rejections/fallbacks in every accepted warm cell |
| Normal close and private-HDD cleanup | PASS | PASS |
| 120-second exploratory manual race | Previous-main cell admitted | Cold/warm admitted; 221 records, warm `484/0`; one warm ≥75 ms stall remains |
| Native toggle off/on/reinitialize behavior | Not applicable | Pending explicit runtime exercise |
| Renderer-switch dirty flush | Not applicable | Pending explicit runtime exercise |
| Write/rename failure preservation and retry | Not applicable | Focused/pure coverage exists; native runtime exercise pending |
| Multiple normal shutdown cycles | PASS | PASS |
| Shutdown-pause/resume writeback | Not applicable | Pending explicit runtime exercise |
| RAM/VRAM and renderer-switch resource qualification | Pending | Pending; [Mainkill1/xemu#69](https://github.com/Mainkill1/xemu/issues/69) remains a gate |
| Visual/gameplay admission | PASS for accepted cells | PASS for accepted cells; proprietary images excluded from portable package |

All accepted retail cells closed normally, deleted their writable HDD clone, and left the immutable seed unchanged. Final cleanup reported no xemu, PresentMon, WPR/WPA/xperf, owned trace session, or private HDD. No WPR trace was started. Cold publication, exact warm loading, byte-identical clean shutdown, truncated-cache rejection/replacement, multiple normal shutdown cycles, and OpenGL sessions producing no SPIR-V file were exercised.

---

# Known Limitations and Unresolved Results

- GLSL generation, Vulkan module creation, and reflection remain synchronous. This candidate targets glslang compilation only.
- A renderer initialized while shader caching is disabled remains ineligible. Enabling the setting requires renderer reinitialization before cache use can begin; this preserves an unseen existing cache and keeps file I/O out of module misses.
- Concurrent xemu processes use unique temporary files, but their in-memory record sets are not merged. The last clean shutdown wins.
- The available validation is structural SPIRV-Reflect validation, not a complete `spirv-val` pass.
- The inherited successful-layout allocation leak remains tracked by [Mainkill1/xemu#69](https://github.com/Mainkill1/xemu/issues/69). Eviction and renderer-switch resource qualification still require resolution or explicit acceptance of that separate gate.
- The accepted PGR2 full-start fixed-route data is unfavorable and variable. The warm-cache hit result does not explain or dismiss the p95/p99/maximum regressions.
- The Morrowind snapshot has candidate OpenGL/cold/warm 60-second coverage but no matching previous-main or stable comparison. The retained 20-second results are historical context only and are not substituted; no same-duration capture was authorized.
- The exploratory manual race is a single less-repeatable driving sequence. Its favorable sample does not establish a controlled speedup, cover the whole map, or clear the automated HOLD. The miss-free warm run still contained a 76.365 ms maximum and one ≥75 ms stall without trace attribution.
- XISO timing is context-only because the ordinary Release run used a live-marker waiver.
- Cross-run cache-file byte determinism is not demonstrated, although every warm seed was unchanged within its own profile.

---

# Validation Status

| Gate | Status |
| --- | --- |
| Exact `b14bfb74` Windows executable identity | PASS |
| Focused native cold/warm/corrupt admission | PASS |
| Full XISO OpenGL correctness | PASS with documented expected non-pass records |
| Full XISO Vulkan cold/warm correctness and validation | PASS; 0 unique VUIDs |
| PGR2 snapshot automated comparison | COMPLETE; same-campaign comparison mixed; historical stable warm p99 is -4.344% context |
| Morrowind snapshot OpenGL/Vulkan cold/warm coverage | COMPLETE; no matching 60-second comparator exists; 20-second history is context only |
| PGR2 full-start automated comparison | **HOLD — repeated tail metrics exceed -2%** |
| 120-second user-driven full-race campaign | COMPLETE as exploratory coverage; favorable single sample does not clear HOLD |
| Native lifecycle/failure matrix | PARTIAL; accepted shutdown/cache cycles pass, three UI paths and native write-failure retry remain pending |
| Resource comparison | PARTIAL; exploratory CPU/process memory and incomplete ~83% GPU sampling only |
| Visual/manual admission | Active-scene checks passed for accepted cells; images remain private. This does not independently qualify every frame. |

A single valid unexplained regression over 2%, including p95, p99, maximum, or stalls, keeps the candidate on hold. This branch has not met the qualification gate.

---

# Tradeoffs and Decision

**Result:** Keep the PR as a draft and retain the **PGR2 full-start HOLD**.

The implementation has passed substantial exact-head correctness coverage and the persisted warm path is active, bounded, and miss-free in the accepted workloads. The controlled automated full-start performance evidence remains unfavorable. The broader manual route is favorable but less repeatable and still contains a miss-free warm stall. Several native lifecycle/resource gates remain open. No merge or performance-improvement claim is supported yet.

---

# Evidence

- [Complete shader-identity diagnostic](https://github.com/Mainkill1/xemu-perf-tests/blob/e77436eb3f7084bd8e406e07ede7ec5f318d1e66/docs/evidence/pr68-shader-identity-20260910/classification/REPORT.md)
- [Focused PR70 admission](https://github.com/Mainkill1/xemu-perf-tests/blob/59f5b5171daf8c42dd394b295ada4c66fbc2c7ab/docs/evidence/pr70-spirv-prewarm-20260910/focused-admission/REPORT.md)
- [Full automated/manual report](https://github.com/Mainkill1/xemu-perf-tests/blob/303c381f6e7891101996f95f7e2c009b3abc559b/docs/evidence/pr70-spirv-prewarm-20260910/full-qualification/REPORT.md)
- [Machine-readable comparisons and result rows](https://github.com/Mainkill1/xemu-perf-tests/blob/303c381f6e7891101996f95f7e2c009b3abc559b/docs/evidence/pr70-spirv-prewarm-20260910/full-qualification/automated)
- [Manual race procedure and results](https://github.com/Mainkill1/xemu-perf-tests/blob/303c381f6e7891101996f95f7e2c009b3abc559b/docs/evidence/pr70-spirv-prewarm-20260910/full-qualification/manual-race/RECIPE.md)
- [Evidence checksums](https://github.com/Mainkill1/xemu-perf-tests/blob/303c381f6e7891101996f95f7e2c009b3abc559b/docs/evidence/pr70-spirv-prewarm-20260910/full-qualification/SHA256SUMS.txt)

---

## Final Summary

Exact-head tests prove bounded persistent SPIR-V reuse works across warm launches and retains normal compilation fallback in the exercised corrupt-cache case. Full XISO correctness passed on OpenGL and Vulkan cold/warm, and Morrowind snapshot coverage is complete for candidate OpenGL/Vulkan.

The candidate is not performance-qualified. Repeated controlled PGR2 full-start tail measurements are materially worse than previous main. The favorable exploratory 120-second race expands shader coverage but cannot clear that result, and its miss-free warm cell still records one ≥75 ms stall. Three native UI lifecycle paths, native write-failure retry, complete GPU/resource evidence, [Mainkill1/xemu#69](https://github.com/Mainkill1/xemu/issues/69) disposition, and a same-duration Morrowind comparator remain open. PR #70 remains draft on HOLD.
