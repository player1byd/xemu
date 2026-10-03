# XISO per-leaf audit: 2026-09-19 Windows runs

The official upstream v0.8.136 release, published fork main at ef1a7fc4, and the PR #135 candidate at cd3c30b6 each had two XISO attempts. The raw values below are guest average milliseconds per leaf, not frame intervals or displayed FPS.

**Performance comparison withdrawn.** The actual XISO launcher set display options but omitted the fork Advanced controls. Candidate used its Prewarm and shader-fastpath defaults On; main used both defaults Off. The saved config was rewritten after exit and omitted default-valued settings, concealing the mismatch. The earlier percentage speedups and aggregate faster/slower counts are invalid as a matched performance comparison.

The runner also waived unavailable live guest markers. Official upstream stopped early after Vulkan device loss, and several leaves varied materially between repeated runs. The linked CSV retains raw timings and functional outcomes, with speedup fields cleared. A separate [explicitly configured, order-balanced fork rerun](2026-09-19-xiso-matched-rerun.md) now provides a diagnostic comparison; it does not rehabilitate the percentages withdrawn from this original dataset.

## Functional coverage

| Pair | Passing leaves with matching functional hashes | Other outcomes |
| --- | ---: | --- |
| Fork main / official upstream | 29 | 117 not reached, 2 failed, 4 passing hash mismatches |
| Candidate / fork main | 150 | 2 hash mismatches |

## Raw timing: 29 fork-main and official-upstream hash-matched leaves

| XISO leaf | Official runs 1 / 2 (ms) | Main runs 1 / 2 (ms) | Performance comparison |
| --- | ---: | ---: | --- |
| `busy_pfifo.pfifo_saturation` | 67.774 / 65.593 | 105.122 / 100.687 | withdrawn |
| `busy_pfifo.pgraph_pattern_polling` | 8261.209 / 8395.558 | 4427.825 / 4582.678 | withdrawn |
| `cpu_floating_point.sse_scalar` | 512.472 / 510.971 | 518.075 / 521.383 | withdrawn |
| `cpu_floating_point.x87_scalar` | 755.892 / 758.102 | 780.180 / 787.358 | withdrawn |
| `cpu_translation_blocks.direct_loop` | 10.076 / 10.093 | 9.787 / 9.851 | withdrawn |
| `cpu_translation_blocks.indirect_dispatch` | 25.162 / 24.877 | 39.352 / 39.362 | withdrawn |
| `cpu_translation_blocks.indirect_dispatch_stress` | 497.811 / 494.841 | 786.101 / 787.302 | withdrawn |
| `fill_rate.solid` | 1.674 / 1.601 | 2.324 / 1.400 | withdrawn |
| `fill_rate.textured` | 1.986 / 1.858 | 2.365 / 1.276 | withdrawn |
| `game_load.cross_title_hotpath.blend_constant_reuse` | 102.941 / 100.940 | 116.136 / 116.406 | withdrawn |
| `game_load.cross_title_hotpath.gpu_wait_control` | 2.111 / 1.896 | 1.294 / 1.253 | withdrawn |
| `game_load.cross_title_hotpath.pgr2_lagspot_inline_elements` | 132.705 / 133.087 | 105.336 / 104.051 | withdrawn |
| `game_load.cross_title_hotpath.pgr2_small_draws` | 107.781 / 107.623 | 115.080 / 114.861 | withdrawn |
| `game_load.cross_title_hotpath.pipeline_state_churn` | 50.964 / 51.597 | 67.027 / 66.948 | withdrawn |
| `game_load.cross_title_hotpath.queued_vertex_cpu_writes` | 62.397 / 53.893 | 66.782 / 64.883 | withdrawn |
| `game_load.cross_title_hotpath.s3tc_streaming_fenced_draws` | 7058.658 / 6643.692 | 1148.507 / 1157.278 | withdrawn |
| `game_load.cross_title_hotpath.scaled_surface_pressure` | 352.345 / 398.352 | 348.297 / 343.837 | withdrawn |
| `game_load.cross_title_hotpath.surface_reuse` | 5.711 / 5.569 | 6.481 / 5.918 | withdrawn |
| `game_load.cross_title_hotpath.texture_binding_reuse` | 183.075 / 183.635 | 154.190 / 154.506 | withdrawn |
| `game_load.cross_title_hotpath.texture_update_reuse` | 3.517 / 3.521 | 2.920 / 2.778 | withdrawn |
| `game_load.long_unlocked_scene.alpha_overdraw` | 29.689 / 29.483 | 33.106 / 33.600 | withdrawn |
| `game_load.long_unlocked_scene.combined` | 59.921 / 60.630 | 70.551 / 71.340 | withdrawn |
| `game_load.long_unlocked_scene.cpu` | 15.988 / 16.238 | 23.811 / 23.789 | withdrawn |
| `game_load.long_unlocked_scene.full_system` | 59.635 / 59.047 | 70.837 / 71.242 | withdrawn |
| `game_load.long_unlocked_scene.pfifo` | 6.163 / 6.209 | 4.930 / 4.957 | withdrawn |
| `game_load.long_unlocked_scene.streaming_surface_reuse` | 8.883 / 8.611 | 9.007 / 8.856 | withdrawn |
| `game_load.s3tc_sync_factor.dxt1_dirty_once_redraw` | 15.770 / 16.029 | 15.720 / 15.544 | withdrawn |
| `game_load.s3tc_sync_factor.rgba8_dirty_once_redraw` | 16.201 / 16.133 | 16.250 / 16.408 | withdrawn |
| `game_load.s3tc_sync_factor.rgba8_ring_payload_generations` | 49.413 / 49.577 | 49.105 / 49.314 | withdrawn |

The two upstream functional failures were `game_load.s3tc_sync_factor.dxt1_same_address_wait` and `game_load.s3tc_sync_factor.rgba8_same_address_wait`. Four additional passing leaves had different framebuffer hashes and are excluded from the hash-matched timing table:

- `game_load.s3tc_sync_factor.bc2_native_eligible`
- `game_load.s3tc_sync_factor.dxt1_ring_payload_generations`
- `game_load.s3tc_sync_factor.dxt1_same_address_queued`
- `game_load.s3tc_sync_factor.rgba8_same_address_queued`

The other 117 leaves were not reached by official upstream. The CSV gives every leaf its two raw values where available and its functional comparison status. No percentage speedup from these runs should be used for release or PR acceptance.
