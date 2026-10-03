# XISO matched rerun after launcher correction

The original September 19 XISO speed percentages were withdrawn because the launcher left the fork Advanced settings to different build defaults. This rerun used the same published Win64 executables, without rebuilding main. The candidate at cd3c30b6 contains the exact PR #135 product head ab518d88 plus the separate Advanced-default changes, which this rerun overrides explicitly. The Windows GUI runner requested Vulkan at 1× scale, VSync Off, shader cache On, Ubershader Prewarm, and shader fastpath On for both roles. The four pre-launch configuration files were byte-identical (SHA-256 9114631eb9acbe4fe4641bc3572b8465b993b4cdbfaa4485976f430c0a245d0e). Both builds logged 32 family-prewarm pipelines ready. Validation and optional host telemetry were Off.

The order was main, candidate, candidate, main. The suite used the same guest XISO/catalog, zero warmup iterations, and per-iteration GPU completion. Each run passed 157/157 records. The 152 leaf tests have 150 framebuffer-hash matches across all four runs; two same-address queued S3TC leaves differed and have no speed percentage in the [per-leaf CSV](2026-09-19-xiso-matched-candidate-main.csv). The [summary JSON](2026-09-19-xiso-matched-candidate-main-summary.json) includes source and executable hashes, settings, CPU time, and focused checks. The prewarm summaries recorded zero later demand hits in all four runs, so this suite does not measure a benefit from those prepared families.

| Run | Host wall s | Summed guest leaf s | Process CPU s | Average logical cores |
| --- | ---: | ---: | ---: | ---: |
| Main 1 | 91.174 | 75.808 | 194.500 | 2.144 |
| Candidate 1 | 92.139 | 76.145 | 197.031 | 2.162 |
| Candidate 2 | 91.130 | 76.164 | 195.547 | 2.153 |
| Main 2 | 92.094 | 76.092 | 195.891 | 2.154 |

Positive candidate speedup means lower candidate latency: 100 × (main latency / candidate latency − 1). The two-run value uses each role's arithmetic mean. For the 150 hash-matched leaves, that value has a descriptive median of **−0.59%** and geometric mean of **−0.36%**; 54 leaves favor candidate, 95 favor main, and one ties. Only 92 leaves have the same direction in both adjacent comparisons. For 51 leaves, the absolute two-run effect exceeds the larger within-role run drift. These counts are a noise audit, not a statistical confidence interval or an overall FPS figure. Summed guest leaf time is about 0.27% longer for candidate; mean host wall time is effectively equal.

Two focused, order-balanced checks probed the apparent differences:

| Leaf | Guest iterations per run | Main 1 / candidate 1 / candidate 2 / main 2 average µs | Candidate speedup, pair 1 / pair 2 | Two-pair result |
| --- | ---: | --- | ---: | ---: |
| Sampler-only texture identity | 80 | 35,435 / 37,693 / 36,656 / 35,481 | −5.99% / −3.21% | **−4.62%** |
| SSE scalar CPU control | 100 | 506,035 / 509,483 / 515,397 / 508,054 | −0.68% / −1.43% | **−1.05%** |

Both focused leaves passed and retained matching framebuffer hashes. The sampler-only result repeats with opposite run order: −4.62% candidate speedup means 4.84% longer candidate latency, using candidate latency as the denominator for the latter. This is a credible **leaf-specific regression signal**. It does not establish a game-frame regression or identify the responsible function. The full-suite CPU control previously appeared about 5.34% faster for candidate; the focused control reverses direction, showing why single leaf percentages from the old grouped run were misleading. The candidate's texture-scale reconciliation on descriptor publication is one source path to profile next, not a proven cause.

The runner still waived unavailable live guest markers. These values are guest-reported microbenchmark durations, not host-correlated frame intervals, displayed FPS, or PR-grade timing proof. The XISO data do **not** support merging PR #135 as an across-the-board improvement. The separate PGR2 and Morrowind comparisons used an explicitly configured launcher and remain attached to their tested revisions. Official upstream was not rerun for this correction; its earlier partial XISO functional result remains 35 PASS, 2 FAIL, and 120 not reached per attempt.
