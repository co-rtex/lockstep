# EngineX — Autonomous Build Execution Prompt

You are the principal software engineer responsible for delivering **TradeGrid EngineX** from an empty repository to a complete, production-quality *educational simulator* (not real trading software).

**Read `docs/ENGINEERING_SPEC.md` in full before designing implementation details; read `CLAUDE.md` for persistent operating rules.** The specification is the product and technical contract. Build its required Releases 1, 2, and 3 entirely from scratch. R4 is optional only if useful after R1–R3 are solid.

## Execution instructions — start immediately

1. Examine the repository, available compiler, build tools, operating system, Git state and specification. Identify any missing dependencies; use available tools and reasonable standard dependencies without asking routine questions. Avoid expensive, destructive or privileged actions.
2. Create and maintain `docs/IMPLEMENTATION_PLAN.md` with a sequence of small, verifiable tasks mapped to each specification acceptance criterion; `docs/PROGRESS.md` with exact task/check/test status; and `docs/DECISIONS.md` containing nontrivial architectural choices and tradeoffs. **Planning is an initial step, not the final output: immediately begin implementing Release 1.** Do not enter a plan-only state that requires approval to start writing code.
3. Implement the project end to end: C++20 deterministic matching and cancellation, bounded integer-based order semantics, efficient price-level indexing, CLI and fixtures, independently reproducible benchmarks, then command journaling, deterministic replay, idempotency, corruption/crash recovery, then concurrent ingress and sequenced TCP market-data publishing with backpressure and resync, exactly as the spec defines.
4. After each logically coherent increment, compile and run focused tests, correct failures, inspect the diff, and make a **local Git commit** with a descriptive conventional message (`feat:`, `fix:`, `test:`, `docs:`, `perf:`, `refactor:`). Commit frequently: approximately one commit per finished component or bounded feature rather than one giant final commit. Make no knowingly broken commits; never stage credentials, generated binaries or unrelated files.
5. After each release, run its complete acceptance checks, update documentation, record verified measurements and results, commit the milestone, and **continue directly into the next release**. Do not stop at R1 merely because the initial résumé-ready version is complete. Continue through the entire required specification and final polish.
6. Treat every test failure, compiler warning, sanitizer report, flaky test, incorrect benchmark, or broken build as a task to diagnose and fix, not a reason to conclude. Perform at least one end-to-end final pass including fresh build, matching correctness, replay/restart/corruption, network and slow-client failure testing, documented benchmark workloads, CI setup and README command verification.
7. Apply engineering judgment independently when the specification permits alternatives. Prefer straightforward, verifiable designs over added frameworks. Preserve deterministic matching order; keep network and durable I/O off the matching hot path while implementing correct documented acknowledgment/durability semantics. Never claim exactly-once distributed delivery, production-grade durability, or low latency without corresponding real evidence.
8. Continuously maintain a clear, accurate project state that a fresh Claude Code session could resume: plan, progress file, decisions, current passing commands and next task. If you are nearing context limits, refresh these files before continuing. Use subagents for isolated review/tests if they genuinely help and cannot create conflicting edits.

## Autonomy and non-stop execution

**Do not stop after writing the plan. Do not stop after a prototype, partial release, checkpoint, successful test run or status report. Make ordinary engineering decisions yourself, implement them, test them, commit them, and immediately continue to the next incomplete specification item.** Continue until (A) the full required R1–R3 project is implemented, debugged, tested, documented, benchmarked and polished with all acceptance criteria verified; (B) I explicitly tell you to stop or clear the goal; or (C) a genuine external blocker, access restriction or tool/session limit prevents further progress.

Do not ask me to approve your design, choose between comparable libraries, or give permission for routine local edits and tests. If a choice affects durability/correctness, decide carefully, document the reasoning, write tests and continue. If blocked on one optional feature, work on other required tasks. If a required feature cannot be verified, **say it is incomplete; do not declare the project finished**.

Keep all changes local to this new EngineX repository. Do **not** modify the original TradeGrid Go code or club repository; do **not** push to GitHub or publish without my explicit permission. Never delete unrelated files, access secrets, or incur charges. Local Git commits are explicitly authorized.

## Finish criteria and report

Conclude only when the required release criteria are met and evidenced by real test/build output, or when genuine blocked status is unavoidable. In the final report provide: completed release checklist; exact commands and results; verified benchmark numbers with machine/compiler/workload details; known limitations; Git commit summary; and precise truthful résumé bullets for actually completed features. Ensure `git status` is clean.

**Start now: inspect the repository and full specification, create the plan, and begin writing Release 1 implementation without waiting for another user message.**
