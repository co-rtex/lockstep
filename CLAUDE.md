# EngineX — persistent Claude Code instructions

## Mission
Build TradeGrid EngineX autonomously as a standalone, technically rigorous C++20 systems engineering project. Read `docs/ENGINEERING_SPEC.md` before modifying or creating implementation files. This specification is the engineering source of truth. Build all **required** releases R1 (core), R2 (durable), and R3 (networked), in that order; R4 is optional and only justified by measured evidence.

## Operating rules
- Act as an owner-level software engineer: investigate, create a brief plan, immediately implement, compile, test, fix, profile, document, and proceed to the next item. Do not end work merely because a draft, plan, phase, or intermediate milestone is done. Make reasonable decisions without seeking approval for ordinary design choices.
- Keep `docs/IMPLEMENTATION_PLAN.md`, `docs/PROGRESS.md`, and `docs/DECISIONS.md` current. Progress must list completed, in progress, blocked, and outstanding acceptance criteria plus exact commands/results. At the end of every major batch, refresh these files.
- Commit LOCALLY after each coherent, verified implementation increment (typically a component, working feature, test suite, or meaningful fix), with specific Git commit messages. Prefer many small, reviewable commits, not giant commits. Run appropriate checks before each commit; avoid committing known-broken intermediate states, secrets, build artifacts, generated binaries, or fabricated results.
- R1 first: working C++ matching engine, shared-rule correctness, cancellation, CLI, tests, benchmark, README and CI. Do not let R2/R3 delay the first independently demonstrable release. Continue immediately through R2 and R3 when R1 acceptance criteria pass.
- Validate with actual commands: clean builds, unit/integration/property tests, sanitizers where supported, fault/recovery tests, network tests, CI configuration and reproducible benchmarks. Do not invent successful runs, numbers, or guarantees.
- Be especially precise about accepted-command sequencing, price-time priority, deterministic replay, crash consistency, acknowledgment ordering, idempotency, bounded queues, network backpressure, and published performance claims.
- Keep this repository independent of https://github.com/co-rtex/tradegrid. Never change or replace its Go engine, its existing workflows, or its contracts. Use the public matching spec/fixtures as behavioral references with attribution. Do not automatically push, open PRs, or modify another repository.
- The primary use is a truthful, high-signal JPMorgan Chicago SWE internship team-matching résumé project. Reuseable systems-engineering depth for quant and Big Tech SWE is the secondary goal. Prioritize substance, tests and verified metrics over unnecessary features and technical buzzwords.

## Decision boundaries
Autonomously modify files and create local commits within this repository. Do not delete unrelated data, run destructive commands outside this repository, access private data or secrets, incur paid cloud/API costs, push to a remote, or make changes to the club's repository without explicit user authorization. If genuine external access, account authentication, elevated OS permissions, billing, or an irreconcilable ambiguity is required, clearly report the blocker and continue with independent work if possible.

## Completion
Only claim the project is finished after the required R1–R3 acceptance criteria in `docs/ENGINEERING_SPEC.md` have been checked against real execution evidence, final clean build/test/recovery/network suites succeed on supported environments, documentation accurately reflects what runs, all changes are committed locally, and `git status` is clean (aside from intentionally ignored build output). Otherwise continue executing the plan or accurately report an unavoidable blocker.
