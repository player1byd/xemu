# Rejected Branches

This page is the archive index for performance work that is no longer part of the active xemu optimization program.

## Performance-first rule

A performance branch stays active only when it either preserves the accepted performance baseline or demonstrates a useful performance improvement. Correctness-only work, diagnostic instrumentation, superseded integration branches, inconclusive experiments, and candidates with unresolved performance regressions are archived instead of remaining indefinitely open.

Archiving does **not** mean the investigation was worthless or that the code was necessarily incorrect. It means the branch does not satisfy the current performance objective and should not occupy an active PR lane.

## Cleanup status — 2026-09-11

PR cleanup is complete. All 15 rejected PRs listed below are closed and unmerged. Associated dead-end xemu issues #38, #40, #56, #60, and #61 are closed as not planned; #39 was already closed and remains closed. The active exceptions were verified after cleanup: PRs #37, #70, and #71 remain open, and issues #69 and #72 remain open.

Branch packaging and deletion are intentionally still pending. The available GitHub connector cannot create/upload binary ZIP archives or delete remote branch refs. Every `ZIP PENDING` row must be archived and verified by the follow-up Git-capable agent before its branch is deleted.

## Active lanes — do not archive

| Item | Purpose | Status |
| --- | --- | --- |
| PR #37 | Settings → Advance performance toggles | KEEP OPEN |
| PR #70 | Vulkan persistent / prewarmed SPIR-V reuse | KEEP OPEN |
| PR #71 | Vulkan fallback ubershader architecture | KEEP OPEN |
| Issue #69 | SPIR-V lifecycle/resource prerequisite used by PR #70 | KEEP OPEN |
| Issue #72 | Vulkan GPU / video-card selection in Settings and live state | KEEP OPEN |

## Closed / rejected PRs

The `Archive` column intentionally remains `ZIP PENDING` until the branch has been exported, verified, stored under `Closed-PRs/`, and its remote head branch deleted. Closing a PR comes before remote branch deletion, but deletion must never occur before the archive is verified.

| PR | Branch | Associated issue(s) | Disposition | Performance reason | Successor / retained value | Archive |
| --- | --- | --- | --- | --- | --- | --- |
| #14 | `fix/eng-2026-523-cumulative-s-native-gl-s3tc-border-mips-u393db700` | — | REJECT | Repeated PGR2 Vulkan maximum regression (-12.59% / -10.23%) and Morrowind OpenGL maximum regression (-2.34% / -5.46%). | Correctness evidence retained; shader/pipeline optimization moved elsewhere. | ZIP PENDING |
| #15 | `fix/eng-2026-523-cumulative-s-vulkan-bordered-cubemap-copy-ueda1ccde` | — | REJECT | Correctness repair did not establish a current performance win; historical Vulkan Morrowind cadence/tails were unfavorable, including p99 +9.89% raw time. | Historical cubemap correctness evidence only. | ZIP PENDING |
| #16 | `fix/eng-2026-523-texture-dma-contract-ue7eb6770` | — | REJECT | Correctness-only contract validation with no current performance improvement; historical PGR2 tails include unfavorable movement. | Preserve descriptor-contract findings as reference. | ZIP PENDING |
| #18 | `fix/eng-2026-523-linear-texture-source-span` | — | REJECT | Correctness-only bounds repair; no current performance gain and historical OpenGL Morrowind cadence remained -2.42%. | Preserve guarded source-span tests as reference. | ZIP PENDING |
| #25 | `validate/eng-2026-523-wait-xiso-markers` | — / folded into #37 policy | SUPERSEDED | Historical CPU use improved materially, but this is an old S-stack branch and current-cycle integration was never qualified. | **Retain the useful Windows wait toggle/result in PR #37.** | ZIP PENDING |
| #48 | `fix/ptimer-future-deadlines` | #39, #40 | REJECT / SUPERSEDED | Correctness-focused PTIMER branch had no demonstrated performance improvement and no current-main qualification. | Findings were carried into #59; both PTIMER branches are now archived. | ZIP PENDING |
| #51 | `release/eng-2026-523-stable-v2-final` | #38 | REJECT / SUPERSEDED | Historical release tree regressed Morrowind cadence about 3.4% Vulkan and 3.2% OpenGL, with unfavorable tails. | Preserve historical release/evidence only. | ZIP PENDING |
| #54 | `research/main-cause-counters` | — | RESEARCH COMPLETE | Instrumentation identified STI/shadow return frequency but cannot be a production performance candidate. | Finding motivated #55. | ZIP PENDING |
| #55 | `research/sti-shadow-entry` | #56 | REJECT | Vulkan result was effectively neutral/mixed; OpenGL had only one favorable non-repeatable observation; no host-efficiency proof. | Preserve STI/shadow hypothesis only. | ZIP PENDING |
| #57 | `research/vk-wait-attribution` | xemu-perf-tests #8 | RESEARCH COMPLETE | Diagnostic instrumentation identified descriptor exhaustion but makes no production speed claim. | Finding motivated #58. | ZIP PENDING |
| #58 | `research/vk-descriptor-capacity` | #40 | REJECT | One non-interleaved pair did not establish benefit; Vulkan p95 moved unfavorably and resource/driver cost remained unknown. | Preserve descriptor-capacity hypothesis only. | ZIP PENDING |
| #59 | `fix/ptimer-main-qualification` | #39, #40, #61 | REJECT | Repeated paired measurements remained mixed/adverse: Vulkan median p99 -2.249%, OpenGL maximum median -4.201%, with multiple >2% adverse pairs. | PTIMER correctness evidence remains historical; performance lane is closed. | ZIP PENDING |
| #67 | `research/pr14-tail-counters` | — | RESEARCH COMPLETE | Diagnostic-only attribution; no performance candidate and no speed claim. | Finding led to shader/pipeline investigations. | ZIP PENDING |
| #68 | `feature/vulkan-shader-binding-cache` | — | RESEARCH COMPLETE | Shader-miss classification completed; instrumentation is not a merge candidate and makes no speed claim. | **Behavioral work continues only in kept PR #70.** | ZIP PENDING |
| #74 | `fix/pgraph-report-bounds-progress` | #60 | REJECT | Correctness repair makes no speed claim and failed the performance-neutral gate; matched PGR2 OpenGL maximum was 88.478 ms vs 43.501 ms previous main. | Preserve report-query findings/tests as historical evidence. | ZIP PENDING |

## Issue disposition

Issues owned exclusively by rejected branches are closed as `not planned`. Issues that feed an active lane stay open.

| Issue | Disposition | Reason |
| --- | --- | --- |
| #38 | CLOSED / NOT PLANNED | Historical S-release integration lane rejected/superseded. |
| #39 | CLOSED | PTIMER repair lanes #48/#59 archived; issue was already closed before this cleanup. |
| #40 | CLOSED / NOT PLANNED | PTIMER/descriptor experiments using this performance gate are archived. |
| #56 | CLOSED / NOT PLANNED | STI-shadow candidate #55 rejected. |
| #60 | CLOSED / NOT PLANNED | Report-bounds candidate #74 archived after correctness/performance investigation. |
| #61 | CLOSED / NOT PLANNED | Compatibility gate belonged to rejected PTIMER #59. |
| #69 | KEEP OPEN | Required by active SPIR-V #70 lifecycle/resource work. |
| #72 | KEEP OPEN | User-requested GPU/video-card selection feature. |

## Archive contract

For every `ZIP PENDING` row, preserve enough material to reconstruct and inspect the branch independently before deleting its remote ref. The archive should contain the exact PR metadata, head/base SHAs, a full source snapshot, a Git bundle preserving the branch history, a base-to-head patch series, a short commit log, and SHA-256 checksums. Store archives under `evidence/wiki-xiso-per-test/Closed-PRs/` (or the GitHub Wiki `Closed-PRs/` directory if the wiki repository is available to the archiving agent).

Only after the archive is pushed and independently verified should the remote branch be deleted. Never delete `main`, `baseline`, branches backing PR #37/#70/#71, or any branch needed by active issue #69/#72.

## Returning a rejected idea

Do not reopen an archived historical PR just because the idea becomes interesting again. Start from the then-current `main`, create a focused branch, explain the measured bottleneck, and require a candidate-vs-previous-main and candidate-vs-fixed-baseline result. The old archive is evidence and design history, not an integration base.
