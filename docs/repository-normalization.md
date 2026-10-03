# Repository normalization inventory — September 9, 2026

**Decision: retain existing main as the accepted stable baseline foundation.** The owner explicitly confirmed its known fixes and testing addons. This normalization pins that source as `baseline` and merges documentation only; it does not replace the product tree, merge an experimental release proposal, or delete history.

## Current branch identities

| Branch | Commit | Meaning now |
| --- | --- | --- |
| `upstream` | `75650bd8cd91945f7b79774e2cee0b200ca373ff` | Exact `xemu-project/xemu:master` at the audit snapshot; no local patches. Native behavior at this exact head has not been qualified here. |
| `baseline` | Tag `baseline/cycle-01-start` | Exact cycle-start commit, initially shared with main. Resolved SHA/tree are in the linked [cycle manifest](https://github.com/Mainkill1/xemu/releases/download/archive/2026-09-09/main-before-normalization/cycle-01-start.json). |
| `main` at switchover | Tag `baseline/cycle-01-start` | Same complete commit as baseline; subsequent accepted PRs and patches advance main only. |

The [preservation release](https://github.com/Mainkill1/xemu/releases/tag/archive/2026-09-09/main-before-normalization) retains a complete Git bundle and checksum, with the captured branches, tags and PR-head histories. Existing PR discussions, release assets and external captures are separate from the bundle and remain preserved at their existing locations. No working branch is being removed.

## Archive tags created

All tags below begin with `archive/2026-09-09/`. They are immutable historical anchors, not qualification labels.

| Tag suffix | Commit |
| --- | --- |
| [main-pre-apu-snapshot](https://github.com/Mainkill1/xemu/tree/archive/2026-09-09/main-pre-apu-snapshot) | `586de4f32c11001c0abe072c3530236f1571b372` |
| [main-before-normalization](https://github.com/Mainkill1/xemu/tree/archive/2026-09-09/main-before-normalization) | `bd1fecb93353272dda2a810991e28945de35b665` |
| [integration-stable-next-acceptance](https://github.com/Mainkill1/xemu/tree/archive/2026-09-09/integration-stable-next-acceptance) | `208e4596832a1f949bd63822d5ddd962c4575067` |
| [release-stable-v2-final-unvalidated](https://github.com/Mainkill1/xemu/tree/archive/2026-09-09/release-stable-v2-final-unvalidated) | `dce89f5c12125cfb4a9416a666c84994da0981e2` |
| [pr25-wait-xiso-markers](https://github.com/Mainkill1/xemu/tree/archive/2026-09-09/pr25-wait-xiso-markers) | `a2a444a85f29dab215bbd646ce1a32a690a9fd1b` |
| [pr2-gloffscreen-owned-resources](https://github.com/Mainkill1/xemu/tree/archive/2026-09-09/pr2-gloffscreen-owned-resources) | `d7223fb8a65a20488c60d86cd689dfa2a833f189` |
| [pr18-texture-repair-series](https://github.com/Mainkill1/xemu/tree/archive/2026-09-09/pr18-texture-repair-series) | `2e801ab07c4a8d0a87defd2ba70ddad3b3e1041c` |
| [pr48-ptimer-future-deadlines](https://github.com/Mainkill1/xemu/tree/archive/2026-09-09/pr48-ptimer-future-deadlines) | `cd5af56967f6d4e5c521de3d98bfe64a6872b7f9` |
| [pr59-ptimer-main-unvalidated](https://github.com/Mainkill1/xemu/tree/archive/2026-09-09/pr59-ptimer-main-unvalidated) | `8da17c3e68c525475f55f3e9d1ddda0ad9c11d5b` |

Pre-alignment pin `baseline/cycle-01-bd1fecb9` preserves the original selected product snapshot at `bd1fecb93353272dda2a810991e28945de35b665`. The [owner decision](https://github.com/Mainkill1/xemu/issues/38#issuecomment-5610949330) supersedes earlier pending-selection statements.

The pre-existing `archive/full-speed-e7bf825d` tag preserves `e7bf825d9cf073b38c52f7fba73efd08509913af`. The bundle also retains older archive tags, review parents and closed-PR heads.

The release also provides the [complete branch/PR inventory](https://github.com/Mainkill1/xemu/releases/download/archive/2026-09-09/main-before-normalization/branch-pr-inventory.json) and [90-commit main-content ledger](https://github.com/Mainkill1/xemu/releases/download/archive/2026-09-09/main-before-normalization/main-content-ledger.json), including exact source identities and explicitly unavailable measurement attribution. These archive assets capture the earlier audit: any pending-foundation classification there is superseded by the owner decision recorded here. The original audit is preserved rather than overwritten.

## Important PR and branch dispositions

These are audit dispositions, not automatic changes to GitHub PR state. Historical merges into a blocked release lineage do not establish acceptance into current main.

| Disposition | PRs | Treatment |
| --- | --- | --- |
| Accepted baseline foundation | Entire main at `bd1fecb9` | Existing source, known fixes and testing addons retained by explicit owner decision. |
| Accepted current-main patch | #53 | APU snapshot-load repair; retain its focused correctness evidence and the unavailable old-reference timing comparison. |
| Historical merged release-line work | #47, #50 | Preserve merge history; not accepted into current main by normalization. |
| Candidate | #2, #5–#11, #14–#16, #18, #37 | Preserve actual stacked bases, implementation and evidence; no implicit merge or rebase. |
| Research | #54, #55, #57 | Preserve diagnostic/optimization investigations; no new optimization during normalization. |
| Blocked | #12, #13, #25, #51, #58, #59 | Keep outstanding qualification/performance gates explicit. #51 must not merge as cleanup. |
| Superseded for the current route | #3, #4, #22, #48 | Preserve evidence and predecessor implementations; supersession does not erase valid tests. |
| Historical | #1, #17, #21, #23, #24, #52 | Retain source, descriptions and failed/research results without promoting them to release status. |

All PR numbers in this table refer to [Mainkill1/xemu](https://github.com/Mainkill1/xemu/pulls?q=is%3Apr). `fix/main-apu-snapshot-load` is represented by merged #53. `fix/ptimer-main-qualification` remains blocked under #59. The frozen S integration and release branches diverge from current main and remain preserved. Existing long branch names and review-parent branches are not rewritten to satisfy the new naming convention.

PR #59 has native timer positive/negative controls, but not completed compatibility and fixed-work performance qualification. The legacy-baseline snapshot reload also reproduces the controller overlay tracked in [#61](https://github.com/Mainkill1/xemu/issues/61); this is not evidence that the PTIMER candidate caused it. The full-suite gates are independently tracked in [#60](https://github.com/Mainkill1/xemu/issues/60), [perf-tests #10](https://github.com/Mainkill1/xemu-perf-tests/issues/10) and [perf-tests #11](https://github.com/Mainkill1/xemu-perf-tests/issues/11).

## Baseline decision and historical attribution

The owner clarified that existing main is stable and known good. Full-Speed was an experiment aggregator; Stable was a route to remove accidentally integrated performance branches and restore a baseline. Accordingly, the entire existing main tree at `bd1fecb9` is the accepted initial foundation. Neither historical branch replaces it, and the lack of a GitHub PR association is not grounds to discard accepted baseline code.

The source inventory remains useful: main and pinned upstream have common ancestor `d73326b62199c6dd952ef512947710e1333a49d3`, with 90 main-only and 17 upstream-only commits. The local range contains 63 source/test patches, seven diagnostic commits, 11 documentation commits, seven historical evidence commits, and #53's source/merge commits. These contents are included in the selected baseline. Historical individual performance effects remain unmeasured where no matched comparison exists; acceptance does not invent a speedup or prove every diagnostic has zero cost.

The exact baseline binary is SHA-256 `3489fdcc593e942b92a612bf35a98f509ff0907e3370e1e5f45f2972d83fb16b`, compiled from `c17591d59c270b352b72e648f5ed65e4b2a3e77e`, whose tree is identical to `bd1fecb9`. Reuse that binary and matching results. The [pinned campaign](https://github.com/Mainkill1/xemu-perf-tests/blob/1f5cc9d8629df9a9157d676c39545595e799a37a/docs/evidence/baseline-campaign-20260909/REPORT.md) retains both completed observations and failed/incomplete tests. Its known gaps remain follow-ups; they do not erase the owner's baseline selection or valid older comparisons.

The cycle starts with the exact same commit in main and baseline, including the completed workflow documentation. Immutable tag `baseline/cycle-01-start` preserves that point. The older product snapshot and its binary/results remain the qualification identity; aligning documentation does not change product source. No optimization delta has accumulated through normalization. Documentation changes do not require rebuilding the baseline; their lack of source changes is checked directly. This is an identity statement, not a newly measured zero-percent result.

## Preserved upstream comparison for a future re-baseline

These source comparisons remain useful for a future deliberate upstream adoption. Current main was selected instead; no upstream transplant is part of this normalization.

| Item | Current upstream at `75650bd8` | Consequence |
| --- | --- | --- |
| Accepted APU repair #53 | Absent. Upstream's APU file is byte-identical to #53's pre-fix file (`1ee6dc42529f257de412855f955c8737a6c0df18`); the repair is a 19-line preload change. | An upstream-based accepted tree needs a reviewed carry-forward decision and focused snapshot checks for its resulting binary. There is no source-text conflict in that file. |
| PTIMER rounding and masked scheduling | Upstream already has rounded conversions, a positive minimum host delay, an interrupt-mask scheduling guard, and 20 registered timer tests. Those tests were inspected, not executed by this audit. | Do not describe upstream as the old fork timer or automatically overlay #59. |
| PTIMER fork-specific contract in #59 | Differs in phase-aware arithmetic, stopped-clock/register-reset handling, explicit armed state and VMState version 5; upstream remains at version 4. | Neither source's test evidence qualifies the other source or an untested combination. #59 remains unaccepted. |

Source anchors: [upstream APU preload](https://github.com/xemu-project/xemu/blob/75650bd8cd91945f7b79774e2cee0b200ca373ff/hw/xbox/mcpx/apu/apu.c#L377), [accepted #53 change](https://github.com/Mainkill1/xemu/commit/c17591d59c270b352b72e648f5ed65e4b2a3e77e), [upstream PTIMER](https://github.com/xemu-project/xemu/blob/75650bd8cd91945f7b79774e2cee0b200ca373ff/hw/xbox/nv2a/ptimer.c), and [#59 timer source](https://github.com/Mainkill1/xemu/blob/8da17c3e68c525475f55f3e9d1ddda0ad9c11d5b/hw/xbox/nv2a/ptimer.c).

The accepted APU ordering releases the BQL, takes the APU lock, waits for the worker to become idle, resets while it remains paused, unlocks, and restores the BQL. Its old regression inputs and oracle can be reused; old binary results cannot become a pass for an upstream-derived binary. No source transplant, build or new emulator test was performed for these checks.

## Workflow reference

| Question | Current answer / remaining gate |
| --- | --- |
| What does untouched upstream do? | Source is pinned above. Exact-head native behavior remains unmeasured here. |
| What exact state is the cycle baseline? | Immutable `baseline/cycle-01-start`; resolved commit/tree in the cycle manifest, with retained binary/results pinned in the selection record. |
| What accepted changes are in main? | The complete owner-selected baseline foundation plus normalization documentation; no later optimization merged by normalization. The source ledger inventories that foundation. |
| What did each optimization change from previous main? | Record both source identities and matched results; historical missing comparisons remain explicitly unavailable. |
| What is the cumulative change versus baseline? | No product-source delta at cycle setup. Future accepted patches require matched cumulative measurements; historical per-patch gaps remain explicit. |
| Where does evidence survive branch deletion? | Immutable source tags/bundle, existing PR discussions, releases and pinned [test evidence](https://github.com/Mainkill1/xemu-perf-tests/pull/7); verify all links before any deletion. |

The [workflow document](repository-workflow.md) defines how these gates are maintained. Baseline selection records owner acceptance; the evidence retains each test's actual scope and outcome.
