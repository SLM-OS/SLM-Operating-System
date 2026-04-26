# Shell Command History — Plan

**Tracking:** 🎫 [#434](https://github.com/SLM-OS/SLM-Operating-System/issues/434)
**Status:** Implemented (kernel/src/shell_history.c, kernel/src/shell.c::shell_read_command, kernel/tests/test_shell_history.c).

Add per-session command history with up/down arrow recall to the SLM-OS
shell. Out of scope for the first cut: reverse search (Ctrl-R), prefix
search, persistent history across reboot, in-line cursor movement
(left/right arrows), and multi-line commands.

---

## Goal

A telnet or UART shell session remembers the last N commands typed in
that session. Pressing **up arrow** recalls the previous command;
pressing **down arrow** moves toward the current edit buffer. The
recalled command is editable in place and submitted with **Enter**.

---

## Storage

Per-session ring buffer on the existing `shell_session` struct.

- Capacity: **32 entries** (`SHELL_HISTORY_DEPTH`).
- Per-entry size: **128 bytes** (`SHELL_HISTORY_LINE_MAX`); commands
  longer than this are truncated and not stored.
- Memory cost: 32 × 128 = **4 KB per session**, ~16 KB total at the
  current 4-session telnet ceiling.

```c
struct shell_history {
    char     entries[SHELL_HISTORY_DEPTH][SHELL_HISTORY_LINE_MAX];
    uint8_t  count;     /* number of valid entries (saturates at DEPTH) */
    uint8_t  head;      /* index of the next slot to overwrite (oldest if full) */
    int8_t   cursor;    /* -1 = current edit buffer; 0..count-1 = recalled */
};
```

Stored newest-first by index from `head` going backwards (modulo
`SHELL_HISTORY_DEPTH`). `cursor == -1` means the user is on the live
edit buffer; non-negative means the user is browsing history.

---

## Capture rules

`shell_history_add(session, line)` is called from the shell REPL right
before dispatching a command. Rules:

- **Skip empty / whitespace-only lines.**
- **Skip exact duplicates of the most recent entry** (bash `HISTCONTROL=ignoredups`-style).
- **Truncate at `SHELL_HISTORY_LINE_MAX - 1`** (preserves trailing NUL).
- Always reset `cursor` to -1 after add so the next up-arrow starts at
  the most recent.

---

## Input handling

Arrow keys arrive as 3-byte escape sequences from any standard
terminal (telnet in raw mode + UART):

| Key | Bytes |
|---|---|
| Up arrow | `0x1B 0x5B 0x41`  (ESC `[` A) |
| Down arrow | `0x1B 0x5B 0x42` (ESC `[` B) |

The line-edit loop in `shell_run` (and the telnet equivalent) gets a
small state machine:

1. On `0x1B`: enter "saw-ESC" state.
2. On `0x5B` after ESC: enter "saw-CSI" state.
3. On `0x41` after CSI: invoke `history_prev`.
4. On `0x42` after CSI: invoke `history_next`.
5. Any unexpected byte resets the state machine and is appended to the
   line buffer normally.

A bare ESC followed by no second byte within ~50 ms (or a non-`[`
follow-up byte) is treated as a literal ESC and discarded — there is
no Vi-style mode toggle to break.

---

## Recall semantics

`history_prev(session)`:
- If `cursor == count - 1`, no-op (already at oldest).
- Else: increment `cursor`, copy `entries[…]` into the live edit
  buffer, repaint the line.

`history_next(session)`:
- If `cursor == -1`, no-op (already on the live buffer).
- Else: decrement `cursor`. If still ≥ 0, copy that entry; if it just
  became -1, clear the line back to empty.
- Repaint.

Editing a recalled line is destructive against the in-progress edit
buffer only; the stored history entry is unchanged. Pressing Enter
captures the (possibly edited) text as a new history entry per the
capture rules above.

---

## Repaint

After a recall:
1. Move cursor to start of input region: emit `\r`.
2. Clear from cursor to end of line: emit `\x1B[K`.
3. Reprint the shell prompt + the recalled text.
4. Cursor lands at the end of the recalled text (no in-line cursor
   movement required by this spec).

---

## API

In `kernel/include/shell.h`:

```c
#define SHELL_HISTORY_DEPTH    32
#define SHELL_HISTORY_LINE_MAX 128

struct shell_session;  /* extended in shell_session.h */

void        shell_history_add(struct shell_session *s, const char *line);
const char *shell_history_prev(struct shell_session *s);
const char *shell_history_next(struct shell_session *s);
void        shell_history_reset_cursor(struct shell_session *s);
```

`prev`/`next` return `NULL` when the cursor would not move (caller
treats `NULL` as "no repaint needed").

---

## Tests

Add to `kernel/tests/test_shell_session.c` (or a new
`test_shell_history.c`):

- Add 1 entry, verify prev returns it, next returns NULL.
- Fill ring beyond capacity, verify oldest is overwritten and cursor
  bounds at the new oldest.
- Empty / whitespace / duplicate-of-previous skipping.
- Truncation at `SHELL_HISTORY_LINE_MAX - 1`.
- `add()` resets cursor to -1.
- Independence: two sessions have independent history.
- ESC-sequence parser: feed `ESC [ A`, assert `history_prev` called;
  feed bare `ESC X`, assert `X` ends up in the line buffer.

---

## Non-goals (explicit)

- **Reverse search (Ctrl-R)** — out of scope; deferred.
- **Prefix-search history** — out of scope.
- **Persistent history across reboot** — `/mnt/files` is RAM-backed
  today; revisit when persistent storage (#35) lands.
- **Left/right arrow in-line cursor movement** — out of scope.
  Backspace-from-end is the only edit operation supported.
- **Multi-line / heredoc commands** — out of scope.
- **History expansion (`!!`, `!N`)** — out of scope.

---

## Estimated effort

- Storage struct + add/prev/next + cursor management: 0.5 d
- ESC-sequence parser + repaint integration into `shell_run` and the
  telnet input path: 0.5–1 d
- Tests: 0.5 d
- **Total: 1.5–2 d.**
