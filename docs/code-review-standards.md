# SLM-OS Code Review Standards

The review criteria are defined in one place:
[`.claude/skills/slmos-review/SKILL.md`](../.claude/skills/slmos-review/SKILL.md),
run as `/slmos-review`. This page explains how that review fits into the
project; it does not repeat the criteria.

## Why a project-specific review

SLM-OS is a bare-metal operating system with no libc, Linux kernel or POSIX
layer. Mistakes that would be ordinary bugs in an application become crashes,
silent boards or security holes here. Many of the review checks come from
failures seen on Pi 5 and Jetson hardware that QEMU does not reproduce.

## Scope of the review

| Target | Command |
|--------|---------|
| Current branch against `origin/main`, including uncommitted work | `/slmos-review` |
| A pull request | `/slmos-review <PR-number>` |
| Specific paths | `/slmos-review <path> ...` |
| Changes since an earlier review | `/slmos-review --since <sha>` |

The review reads code only, apart from running `make check-braces`, which
configures each platform without compiling. Builds and QEMU tests belong to
`/ci`, and hardware runs belong to `/deploy-and-test`.

The brace rule is checked mechanically by `clang-tidy` (`.clang-tidy`,
`scripts/check-c-braces.py`) on changed lines, for code that the main and test
kernel of each platform compile. `make check-braces BRACES_ARGS=--all` checks
the whole tree and reports 0, so a non-zero count is a new regression. Code it
cannot see may still have unbraced bodies: `#if` blocks off in every
configuration, files outside the builds, and macro bodies.

## Severity and completion

Findings are Critical, Warning or Suggestion. Each Critical or Warning names a
concrete failure scenario. A change is ready when no Critical or Warning
findings remain; Suggestions alone do not call for another review round.

## Related checklists

- [`docs/net-driver-checklist.md`](net-driver-checklist.md): NIC drivers
- [`kernel/include/string.h`](../kernel/include/string.h): `secure_zero`, for wiping secrets from the stack
- [`CLAUDE.md`](../CLAUDE.md) "Post-Change Checklist": tests, documentation and
  verification required after every code change
- [`kernel/CLAUDE.md`](../kernel/CLAUDE.md) and
  [`runtime/CLAUDE.md`](../runtime/CLAUDE.md): subsystem rules the review enforces
