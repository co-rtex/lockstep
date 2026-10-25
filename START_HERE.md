# EngineX — start autonomous Claude Code development

This starter contains:
- `docs/ENGINEERING_SPEC.md`: full authoritative project specification.
- `CLAUDE.md`: instructions Claude Code automatically reads from the project root.
- `AUTONOMOUS_BUILD_PROMPT.md`: complete implementation and autonomy instructions.

## One-time setup on macOS

1. Unzip this archive into a NEW folder such as `~/Developer/enginex` (not your club's `tradegrid` checkout).
2. Open Terminal in that folder, then run:

   ```sh
   cd ~/Developer/enginex
   git init -b main
   xcode-select -p || xcode-select --install
   cmake --version || echo "Install cmake with: brew install cmake ninja"
   git --version
   claude --version
   claude auth status
   ```

   If CMake or Ninja is missing and you use Homebrew: `brew install cmake ninja`. Finish first-time Claude authentication if needed. If Claude Code is already installed and authenticated, no reinstall is necessary.
3. Start Claude Code:

   ```sh
   claude --permission-mode auto
   ```

   If auto mode is not available on your account, use `claude --permission-mode acceptEdits`, and use `/permissions` to pre-approve needed build/test/git commands as you review them. **Do not enable `--dangerously-skip-permissions` on your normal Mac.**

4. Within Claude Code, use **this one command** to launch the autonomous implementation loop:

   ```text
   /goal Read @AUTONOMOUS_BUILD_PROMPT.md and @docs/ENGINEERING_SPEC.md and follow @CLAUDE.md. Implement every REQUIRED acceptance criterion of Releases R1, R2, and R3; verify a clean build, correctness/replay/recovery/network test suites and honest benchmark/documentation results; make frequent tested LOCAL git commits; preserve the external TradeGrid Go project; maintain progress files; and stop only when all required criteria are demonstrably satisfied or an unavoidable user-action blocker prevents further progress. Begin coding now.
   ```

   `/goal` both sets the continuing completion condition and starts the first work turn. It can restart turns while the goal remains unsatisfied. It cannot override model/usage limits, workspace permissions, context exhaustion, session availability or explicit stop commands.

## Controls

- Check progress: `/goal` (status), `git log --oneline -10`, `cat docs/PROGRESS.md`.
- Stop the persistent completion loop: `/goal clear` or explicitly interrupt the session.
- Resume a prior interactive conversation in the same directory: `claude --continue` (then check `/goal`).
- Local Git commits are allowed; no remote push is authorized by these instructions.

## Why the separation matters

The original TradeGrid Go project has an authoritative exchange and a club development roadmap. This project is an independent experimental C++ build, not a replacement or silent integration. The spec provides compatibility boundaries.
