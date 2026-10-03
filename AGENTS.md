# Instructions for Autonomous Agents & AI Assistants

This file provides mandatory instructions and guidelines for autonomous AI agents, coding assistants, and models contributing code or creating pull requests for the xemu project.

---

## PR Descriptions, Decisions, Evidence, and Communication

### The PR body is the canonical current record

The description of every pull request, including drafts and investigation
PRs, must explain the branch as it exists now. A PR description is not a
placeholder, a status ping, or a link that forces reviewers to inspect the
diff or a plan document before they can understand the work.

At creation, and after every material code change, test campaign, or change
in recommendation, edit the PR body in place. It must make all of the
following clear:

- the problem being solved;
- the concrete code or documentation currently present on the branch;
- the important files, subsystems, functions, and behavior changed;
- the mechanism by which the change is expected to work;
- what has and has not been implemented;
- what has and has not been validated;
- known risks, regressions, and remaining work.

A draft or **HOLD** recommendation does not relax these requirements.
"This PR starts an investigation" or "more testing is needed" is not an
adequate description of a branch. Separate completed branch contents from
future work. If a branch contains only a plan document, identify it as a
**plan-only PR**, summarize the actual plan, expected production files and
invariants, and explain why the plan itself is the reviewable deliverable.
Do not open an announcement-only PR that merely restates an issue or future
intent.

### Use this mandatory PR body template

The very top of every PR must use the following structure. Keep the headings
in this order and fill every applicable section. Use `N/A` with a reason
instead of silently deleting a section.

```markdown
## Recommendation: MERGE | HOLD | FAIL

**Summary:** In 2-4 sentences, state the problem, the concrete branch delta,
the observed result or current evidence state, and why this recommendation
applies. Do not summarize only the investigation or future plan.
**Remaining:** `None`, or a bounded list of work and tests required before
reconsideration.

### Problem

Describe the failure, limitation, or opportunity; how it was observed or
reproduced; and the related issue.

### What this PR changes

Describe the current diff, not intended future work:

- files, subsystems, and important functions changed;
- behavior, algorithm, data flow, synchronization, ownership, lifetime, or
  instrumentation added or changed;
- what is implemented on the branch now;
- what is explicitly out of scope or not yet implemented.

### Why this approach

Explain the mechanism, important correctness invariants, and relevant
tradeoffs or rejected alternatives.

### Validation

List exact checks completed and not completed:

- build and CI;
- correctness and regression tests;
- performance measurements, or `Performance: N/A - <reason>`;
- tested parent and candidate commits, settings, workload, renderer, and
  hardware when applicable.

### Results

Include compact, decision-relevant measurements or tables. Say `Not yet
measured` when appropriate; never imply that a count, trace, build, or
passing suite proves a speedup.

### Risks and remaining questions

List known regressions, lifetime/threading/invalidation cases, missing
coverage, and blockers.

### Related work

Use `Fixes`, `Closes`, or `Addresses` for issues as appropriate. Link
dependencies, prior PRs, and superseded work.

> **Agent Declaration**: This pull request was created with assistance from
> [Agent Name / Model Name].
```

The template is a minimum review contract, not permission to pad the body
with generic text. Prefer concrete nouns, file names, functions, measured
values, and explicit state over vague statements such as "improves
performance," "adds support," or "work is ongoing."

### Recommendation meanings

Choose exactly one current recommendation:

- **MERGE**: The tested candidate satisfies the declared correctness,
  performance, and review gates. Required evidence is complete and applicable
  to the current candidate. This is a recommendation, not permission to merge.
- **HOLD**: Required testing is missing, evidence is stale or incomparable,
  results are inconclusive, or useful improvements come with unresolved
  regressions or tradeoffs. Preserve the useful result and provide a bounded
  handoff: what improved, what worsened, and what to investigate next.
- **FAIL**: Completed, adequate evaluation identifies zero useful improvements
  in performance, correctness, or functionality, and no worthwhile direction
  remains for this candidate. This means stop pursuing it, not merely that
  one test or metric failed. Missing evidence, uncertainty, or mixed gains
  and losses are HOLD, not FAIL.

For example, p99 increasing by 10 ms while CPU falls by 10% is **HOLD** with
both measurements and a performance handoff, not FAIL. A correctness repair
is itself an improvement even without a speedup. A failed correctness check
still blocks MERGE; a useful but broken candidate remains HOLD for repair.
Record individual test failures honestly regardless of the PR recommendation.

A successful build or passing correctness suite is not performance proof.
"No speedup claimed" does not exempt runtime changes from regression testing.
A necessary correctness fix with a measured performance cost must disclose
that cost and remain HOLD until the tradeoff is explicitly accepted.

### Update the body; do not create a progress-comment stream

The PR body is the single source of truth for the current branch. Replace
stale text and results in the body rather than appending a chronological
diary in comments.

Do not post comments merely to say that work started, a commit was pushed,
the body was updated, CI ran, another test completed, a result was
superseded, or more work remains. Do not post one comment per commit, test
run, experiment, or agent iteration. Commit history, check runs, and the
current PR body already provide that information.

A PR comment is appropriate only when it serves a distinct communication
need, such as:

- answering a reviewer in an existing thread;
- requesting a specific external test or action from another person or
  machine;
- recording one consolidated evidence package that cannot be stored or
  linked elsewhere;
- documenting a discrete review decision that subscribers must be notified
  about.

Even then, update the PR body with the current conclusion and link the
comment or artifact from the relevant section. Consolidate related evidence
into one comment per coherent campaign or decision. The description always
wins when an older comment conflicts with current state.

### Keep the description current and readable

Normally target 250-600 words of prose, plus compact tables and necessary
flow diagrams. Completeness and reviewability take priority over the target;
do not remove material findings merely to shorten the body.

Explain what changed, why it should help, what was actually measured, and
what remains unresolved. The reader should be able to understand the
current branch and decision without reading comments, commits, or linked
raw logs first.

### Show significant processing-flow changes

When significantly changing execution order, branching, caching, batching,
threading, ownership, or synchronization, include BEFORE and AFTER diagrams.

Use Mermaid or compact text flow diagrams. Show the changed decisions,
work removed or deferred, and the correctness/invalidation conditions.
Simple arithmetic or constant changes do not require decorative diagrams.

### XISO correctness AND timings are mandatory

For runtime/performance-affecting changes:

1. Identify affected XISO categories and individual test IDs, plus an
   unaffected control. Run focused comparisons during development;
   complete the broader required coverage before recommending MERGE.

2. Retrieve the actual per-test timing measurements from the runner's
   result/comparison reports after the campaign completes. Do not stop
   at status, passed counts, or comparison-eligible counts. Use the
   [XISO campaign](https://github.com/Mainkill1/Xemu-Test-Runner/blob/main/docs/XISO-CAMPAIGNS.md)
   and [result/comparison](https://github.com/Mainkill1/Xemu-Test-Runner/blob/main/docs/HASH-RESULTS.md)
   documentation for the maintained workflow.

3. Report baseline time, candidate time, absolute difference, percentage
   improvement, and correctness for the relevant timed tests. Separate
   OpenGL and Vulkan. Include affected cases, controls, and material
   regressions, not only favorable rows. Use a compact table:
   `Test ID | Backend | Before | After | Time difference | Improvement % | Correctness`.

4. Preserve the full per-test comparison in linked evidence. A total
   suite duration or "160/160 passed" cannot replace individual timings.
   Expand truncated or paged reports before claiming complete coverage.
   Structural parent groups are not independent timing samples.

5. Use matched test/catalog revisions, fixed-work settings, host,
   renderer, build configuration, and cache conditions. Record repetitions,
   statistic, units, and comparison eligibility. Use balanced A/B runs;
   do not call small differences improvements without assessing variation.

6. Missing required timings mean HOLD. Failed, incomplete, or incomparable
   runs remain visible and cannot establish a speedup. If a test has no
   valid timing contract, mark it NOT COMPARABLE and explain the alternative
   measurement; never invent timing data or silently omit the gap.

Label suite wall time, individual test work time, frame time, and guest
cadence accurately. They are different measurements. When changing
emulated clocks or timers, guest-reported time alone cannot prove host
performance improvement.

Use "Improvement %" consistently: positive is better, negative is worse.
For time/cost: `100 * (baseline - candidate) / baseline`.
For throughput: `100 * (candidate - baseline) / baseline`.
A zero baseline has no meaningful percentage.

### Bind evidence to the actual candidate

Identify the exact parent/reference and tested candidate commits.
Retain executable hashes, runner revision, XISO/catalog identity,
settings, and run IDs in the linked manifest.

Keep the fixed campaign baseline unchanged. Distinguish candidate-versus-
parent results from cumulative results against the fixed baseline.

After a new push, reassess evidence applicability. Older measurements
are not automatically current-head proof; verified source/tree equivalence
may be documented rather than rerunning unchanged product code.

Documentation-only changes may state `Performance: N/A` with a reason.
Runtime changes require measurements even when their purpose is correctness.

### Keep evidence with the owning emulator PR

The xemu PR body contains the current decision, concrete implementation
summary, compact result tables, and links to detailed evidence. Raw per-test
tables, logs, manifests, and captures belong in durable linked evidence
storage.

When a GitHub comment is the only practical way to attach or preserve an
evidence package, post one consolidated evidence comment for that coherent
campaign, label it clearly, and link it from the PR body. Do not use comments
as a running laboratory notebook.

Never create evidence-only PRs or evidence-storage commits in
Xemu-Test-Runner or xemu-perf-tests. Those repositories receive actual
runner or test changes only. Linking a genuine dependency PR is allowed.

Before recommending MERGE, verify: current decision; applicable CI;
before/after correctness; required XISO timings; representative workload
and resource checks; disclosed regressions; accessible evidence; and
flow diagrams where needed.

---

## 1. Strict Adherence to Project Standards

All contributions must strictly comply with the guidelines defined in [CONTRIBUTING.md](CONTRIBUTING.md).

---

## 2. Mandatory Agent Declarations

When submitting code or opening a pull request generated with or assisted by an agent:

1. **PR Description Declaration**:
   - The pull request description **must** include an explicit declaration stating which AI agent and model were used to generate or assist with the change.
   - Example:
     ```markdown
     > **Agent Declaration**: This pull request was created with assistance from [Agent Name / Model Name].
     ```

---

## 3. Scoping & Granularity Guidelines

To produce high-quality, easily reviewable pull requests, agents must observe the following constraints:

- **Single Responsibility**: Each pull request must address exactly one bug fix, hardware improvement, or specific feature. Never bundle multiple independent bug fixes, features, or cleanups into a single commit or pull request. Break independent changes into separate, logically sequenced PRs.
- **Minimal Change**: Touch only the files and lines necessary to accomplish the stated task. Do not refactor surrounding functions or reorganize include headers unless explicitly requested.
- **Verify Against Upstream**: Always ensure the branch is rebased on the latest upstream `master` and that changes do not stomp on or duplicate existing open PRs.

---

## 4. Verification & Testing

- **Compilation**: Verify that all modified files compile without warnings or errors.
- **Emulation Accuracy**: Do not hallucinate register definitions, bitfields, or hardware behaviors. Cross-reference existing implementations under `hw/xbox/` or verified hardware documentation.
- **Test Coverage & Parity**: Whenever altering hardware emulation (NV2A, APU/DSP, MCPX, memory controller, etc.), provide or suggest a test XBE that can be run on both bare-metal Xbox hardware and xemu to validate behavior.

---

## 5. Agent Pre-Submission Checklist

Before finalizing any commit or pull request, ensure:

- [ ] Commit message uses `<subsystem>: <short description>` followed by a detailed explanatory body.
- [ ] `clang-format` is applied to new files, and existing code style is respected.
- [ ] No unrelated formatting or refactoring changes are included.
- [ ] Existing open pull requests have been searched to avoid duplicating work.
- [ ] The PR body follows the mandatory template from the first draft and explains the current branch delta, important files/functions, mechanism, scope, and present validation state.
- [ ] A draft or HOLD PR clearly separates what is implemented now from future work; a plan-only PR is labeled and justified as such.
- [ ] The PR starts with one MERGE/HOLD/FAIL recommendation, a concrete result summary, and bounded remaining work; mixed benefits and regressions are HOLD, not FAIL.
- [ ] The pull request description includes the agent/model declaration.
- [ ] The PR body was updated in place instead of posting routine progress comments; any necessary evidence comment is consolidated and linked from the body.
- [ ] Required XISO per-test before/after timings and correctness are reported, including controls, material regressions, and the full linked comparison; pass/fail counts alone are insufficient.
- [ ] Evidence identifies the tested candidate, reference, runner, workload/settings, and run IDs; missing or stale required evidence prevents MERGE.
- [ ] Significant processing-flow changes have BEFORE/AFTER diagrams.
- [ ] The description summarizes the current head; raw evidence stays with the owning xemu PR or durable linked storage, not in evidence-only test-runner/test-suite PRs or commits.
