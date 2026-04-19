# SLM-OS

A bare-metal operating system for running Small Language Model inference on
edge hardware. Written in C and Rust, targets ARM64 and x86-64, runs on
QEMU, Raspberry Pi 5, NVIDIA Jetson Orin Nano, and bare-metal x86-64.

CS capstone project, Sonoma State University Computer Science program.

---

## Supported Platforms

| Platform | Target ID | Status |
|---|---|---|
| QEMU (ARM64, `virt` machine) | `QEMU_VIRT` | Primary development target |
| QEMU / bare-metal x86-64 | `X86_64` | Full feature parity with ARM64 |
| Raspberry Pi 5 (BCM2712) | `RASPI5` | Boots to interactive shell over UART |
| NVIDIA Jetson Orin Nano | `JETSON_ORIN_NANO` | Boots via kexec at NS EL2 + VHE |

Per-feature, per-platform status: [`docs/specs/README.md`](docs/specs/README.md).

---

## Quick Start

Environment setup is documented in [`docs/setup.md`](docs/setup.md) (Ubuntu).

```bash
make kernel                   # Build the kernel (QEMU_VIRT by default)
make run                      # Build and boot in QEMU
make test                     # Build the test kernel and run the suite
```

All three commands accept `PLATFORM=<target>` to switch platforms. Full
build, run, and debug workflow: [`docs/getting-started.md`](docs/getting-started.md).

---

## Repository Layout

```
kernel/          C kernel (boot, scheduler, memory, drivers, shell, GPU, USB, net)
runtime/         Rust runtime (model loading, ONNX inference, allocator, FFI glue)
cmake/           Cross-compile toolchain files
scripts/         Demo scripts (Lua) and lab helpers
host-tools/      Host-side utilities (GSP harness, firmware extractors)
docs/            Documentation (see below)
.claude/         Claude Code slash commands, subagents, and settings
CMakeLists.txt   C kernel build
Makefile         Top-level orchestration
```

---

## Development Workflow

### CI

GitHub Actions is **not** the CI pipeline for this project. CI is run
locally via a Claude Code slash command:

```
/ci
```

This runs `.claude/commands/ci.md` — a clean-build + QEMU test sweep across
ARM64 and x86-64 targets. Use before every merge to `main`.

### Code Review

Branch review is also a slash command, tailored for embedded / bare-metal
code quality criteria (no libc, no POSIX, kernel stacks fixed at 16 KB,
memory-safety review for use-after-free in task/IPC teardown, etc.):

```
/slmos-review
```

Runs `.claude/commands/slmos-review.md` against the current branch's diff
to `main`. Named `slmos-review` rather than the shorter `review` so it
doesn't mask Claude Code's built-in `/review` command. See the command
file for the full critical / high / medium / low review taxonomy.

### Issue Tracking

GitHub Issues via `gh` CLI. See [`CLAUDE.md`](CLAUDE.md) §"Issue Tracking"
for labels, priority scheme, and the file-an-issue-rather-than-a-TODO
convention.

---

## Documentation

Core references:

- [`docs/architecture.md`](docs/architecture.md) — system architecture
- [`docs/specs/`](docs/specs/) — per-capability × per-platform fact sheets
- [`docs/setup.md`](docs/setup.md) — dev environment setup (Ubuntu)
- [`docs/getting-started.md`](docs/getting-started.md) — building, running, debugging
- [`docs/shell.md`](docs/shell.md) — shell command reference
- [`docs/boot-sequence.md`](docs/boot-sequence.md) — per-platform boot path
- [`docs/scheduler.md`](docs/scheduler.md) — scheduler design (incl. AI policies)
- [`docs/eviction.md`](docs/eviction.md) — page eviction (CACHEUS ensemble)

Project-wide and subsystem notes:

- [`CLAUDE.md`](CLAUDE.md) — project conventions, build commands, hardware lab
- [`kernel/CLAUDE.md`](kernel/CLAUDE.md) — kernel-specific gotchas (cache coherency, NC memory, spinlocks)
- [`runtime/CLAUDE.md`](runtime/CLAUDE.md) — Rust runtime conventions

Archived / historical material lives under [`docs/archive/`](docs/archive/).

---

## Status

- Cross-platform capability matrix: [`docs/specs/README.md`](docs/specs/README.md)
- Delivery narrative: [`docs/capstone-feature-status.md`](docs/capstone-feature-status.md)
- Open blockers and investigations: `gh issue list`

---

## License

See repository root.

---

*Top-level design notes and early-project planning material are preserved in
[`docs/archive/FUTURE.md`](docs/archive/FUTURE.md).*
