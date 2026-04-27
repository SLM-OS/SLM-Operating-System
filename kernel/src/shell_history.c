/*
 * shell_history.c — Per-session command-history ring (#434)
 *
 * Implements docs/archive/plans/shell-command-history-plan.md. Stores up to
 * SHELL_HISTORY_DEPTH lines per shell session in a fixed-size circular
 * buffer; up arrow recalls older entries, down arrow walks back toward
 * the live edit buffer. State lives entirely on shell_session->history
 * — no globals — so console and TCP sessions never share recall.
 *
 * Layout invariants:
 *   entries[]  ring of SHELL_HISTORY_DEPTH NUL-terminated strings.
 *   head       slot that the next add() will overwrite. The most
 *              recently added entry is therefore at (head-1) mod
 *              DEPTH; the oldest valid entry (when full) is at head.
 *   count      saturates at SHELL_HISTORY_DEPTH and tracks how many
 *              slots are valid.
 *   cursor     browse position used by prev/next:
 *                -1 .......... live edit buffer (no recall in flight)
 *                 0 .......... most recent entry showing
 *                 count - 1 .. oldest entry showing
 *
 *   Entry index from cursor: (head - 1 - cursor) & (DEPTH - 1).
 *
 * SHELL_HISTORY_DEPTH must stay a power of two so the masking arithmetic
 * holds; a static_assert inside this file pins that invariant.
 */

#include "shell.h"
#include "shell_session.h"
#include "string.h"

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/* Local mask alias; the power-of-two invariant that makes this valid is
 * pinned by static_assert in shell.h so every user of the macros sees
 * the same constraint. */
#define SHELL_HISTORY_MASK (SHELL_HISTORY_DEPTH - 1)

/* True for "" or any string consisting only of spaces and tabs. The
 * REPL passes raw input here; trimming is the history layer's job. */
static bool history_line_is_blank(const char *line)
{
    if (!line) {
        return true;
    }
    for (const char *p = line; *p; p++) {
        if (*p != ' ' && *p != '\t') {
            return false;
        }
    }
    return true;
}

/* Sentinel returned by shell_history_next when the cursor walks back
 * onto the live edit buffer. Distinct from NULL so the caller knows to
 * repaint with an empty input region rather than do nothing. */
static const char shell_history_empty[] = "";

void shell_history_add(struct shell_session *s, const char *line)
{
    if (!s || !line) {
        return;
    }
    if (history_line_is_blank(line)) {
        return;
    }

    struct shell_history *h = &s->history;

    /* Skip exact duplicate of the most recent entry (HISTCONTROL=ignoredups
     * in bash terms). Cursor still resets so the next up arrow lands on
     * the most recent entry rather than wherever the previous browse left
     * off. */
    if (h->count > 0) {
        uint8_t newest = (uint8_t)((h->head - 1) & SHELL_HISTORY_MASK);
        if (strcmp(line, h->entries[newest]) == 0) {
            h->cursor = -1;
            return;
        }
    }

    char *slot = h->entries[h->head];
    size_t i = 0;
    while (i + 1 < SHELL_HISTORY_LINE_MAX && line[i]) {
        slot[i] = line[i];
        i++;
    }
    slot[i] = '\0';

    h->head = (uint8_t)((h->head + 1) & SHELL_HISTORY_MASK);
    if (h->count < SHELL_HISTORY_DEPTH) {
        h->count++;
    }
    h->cursor = -1;
}

const char *shell_history_prev(struct shell_session *s)
{
    if (!s) {
        return NULL;
    }
    struct shell_history *h = &s->history;
    if (h->count == 0) {
        return NULL;
    }
    /* Already at oldest entry — clamp. */
    if (h->cursor + 1 >= (int)h->count) {
        return NULL;
    }
    h->cursor++;
    uint8_t slot = (uint8_t)((h->head - 1 - h->cursor) & SHELL_HISTORY_MASK);
    return h->entries[slot];
}

const char *shell_history_next(struct shell_session *s)
{
    if (!s) {
        return NULL;
    }
    struct shell_history *h = &s->history;
    if (h->cursor < 0) {
        return NULL;   /* already on live buffer */
    }
    h->cursor--;
    if (h->cursor < 0) {
        return shell_history_empty;
    }
    uint8_t slot = (uint8_t)((h->head - 1 - h->cursor) & SHELL_HISTORY_MASK);
    return h->entries[slot];
}

void shell_history_reset_cursor(struct shell_session *s)
{
    if (!s) {
        return;
    }
    s->history.cursor = -1;
}
