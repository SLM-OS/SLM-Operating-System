/*
 * telnet.h - RFC 854 IAC state machine for the TCP shell
 *
 * Layered between the raw-TCP RX path (shell_io_tcp's tcp_recv
 * callback) and the per-session RX ring. Filters IAC sequences out
 * of the byte stream, negotiates a minimal subset of options, and
 * records window size + terminal type on the session for commands
 * like `top` to consume.
 *
 * Scope (per docs/archive/plans/multi-session-shell-plan.md §2):
 *   - Option negotiation: WILL / WONT / DO / DONT for
 *       ECHO (RFC 857), SUPPRESS-GO-AHEAD (RFC 858),
 *       NAWS (RFC 1073), TERMINAL-TYPE (RFC 1091)
 *   - IAC IP (Interrupt Process) -> inject Ctrl+C into RX ring
 *   - IAC IAC -> literal 0xFF data byte
 *   - CR LF / CR NUL -> single CR (so shell_read_line sees one Enter)
 *
 * Out of scope:
 *   - BINARY / LINEMODE options
 *   - Authentication / encryption options
 *
 * The parser is pure: no lwIP, no shell_session, no globals. All
 * side effects go through the telnet_ops vtable supplied by the
 * caller, which makes the state machine unit-testable.
 */

#ifndef TELNET_H
#define TELNET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ============================================================================
 * Protocol constants (RFC 854 + option RFCs)
 * ============================================================================ */

#define TELNET_IAC        0xFF  /* Interpret As Command */
#define TELNET_SE         0xF0  /* End of subnegotiation */
#define TELNET_NOP        0xF1
#define TELNET_DM         0xF2  /* Data Mark */
#define TELNET_BRK        0xF3  /* Break */
#define TELNET_IP         0xF4  /* Interrupt Process */
#define TELNET_AO         0xF5  /* Abort Output */
#define TELNET_AYT        0xF6  /* Are You There */
#define TELNET_EC         0xF7  /* Erase Character */
#define TELNET_EL         0xF8  /* Erase Line */
#define TELNET_GA         0xF9  /* Go Ahead */
#define TELNET_SB         0xFA  /* Subnegotiation Begin */
#define TELNET_WILL       0xFB
#define TELNET_WONT       0xFC
#define TELNET_DO         0xFD
#define TELNET_DONT       0xFE

#define TELNET_OPT_BINARY  0x00
#define TELNET_OPT_ECHO    0x01
#define TELNET_OPT_SGA     0x03  /* Suppress Go Ahead */
#define TELNET_OPT_TTYPE   0x18  /* Terminal type */
#define TELNET_OPT_NAWS    0x1F  /* Negotiate About Window Size */

/* TERMINAL-TYPE subnegotiation sub-commands */
#define TELNET_TTYPE_IS    0x00  /* client -> server: "my term is ..." */
#define TELNET_TTYPE_SEND  0x01  /* server -> client: "tell me your term" */

/* Max subnegotiation payload we'll buffer. TERMINAL-TYPE values are
 * usually ~16 bytes ("xterm-256color"); 64 is safe + bounds anything
 * a misbehaving peer might throw at us. */
#define TELNET_SB_BUF_SIZE 64

/* Max terminal-type string length we'll record (includes NUL). */
#define TELNET_TTYPE_MAX   32

/* ============================================================================
 * Caller-supplied hooks
 * ============================================================================ */

struct telnet_ops {
    /* Pass a decoded data byte up to the shell's RX path. Called for
     * every non-IAC byte and for IAC IAC (literal 0xFF). */
    void (*inject_rx)(void *ctx, uint8_t byte);

    /* Send telnet response bytes to the peer. The parser batches
     * these (one call per full IAC-sequence response). */
    void (*send_to_peer)(void *ctx, const uint8_t *data, size_t len);

    /* NAWS subnegotiation completed. cols / rows are in cells. */
    void (*on_naws)(void *ctx, uint16_t cols, uint16_t rows);

    /* TERMINAL-TYPE subnegotiation completed. `term` is NUL-terminated
     * and no longer than TELNET_TTYPE_MAX. */
    void (*on_term_type)(void *ctx, const char *term);

    /* IAC IP received — caller typically sets a session-scoped
     * interrupt flag AND injects 0x03 into the RX ring so
     * shell_read_line sees a Ctrl+C. */
    void (*on_interrupt)(void *ctx);

    /* Opaque context pointer passed to every callback above. */
    void *ctx;
};

/* ============================================================================
 * Parser state
 * ============================================================================ */

enum telnet_state {
    T_DATA,        /* passing bytes through */
    T_CR,          /* just saw CR; eat next LF or NUL */
    T_IAC,         /* just saw IAC; next byte is a command */
    T_OPT_WILL,    /* just saw IAC WILL; next byte is option code */
    T_OPT_WONT,
    T_OPT_DO,
    T_OPT_DONT,
    T_SB,          /* inside IAC SB payload */
    T_SB_IAC,      /* just saw IAC inside SB; next byte is SE or IAC */
};

/* Bit flags tracking which options we have actively negotiated as
 * either side WILL/DO so we don't respond to duplicate requests in
 * an option-negotiation loop. */
#define TELNET_F_WILL_ECHO      (1u << 0)
#define TELNET_F_WILL_SGA       (1u << 1)
#define TELNET_F_DO_NAWS        (1u << 2)
#define TELNET_F_DO_TTYPE       (1u << 3)

struct telnet_parser {
    enum telnet_state state;
    uint8_t           subneg[TELNET_SB_BUF_SIZE];
    size_t            subneg_len;
    uint32_t          negotiated_flags;   /* TELNET_F_* */
    /* Pass-through for binary uploads (#621 follow-up). When true,
     * the parser still performs IAC IAC -> 0xFF unstuffing (so the
     * sender can transmit literal 0xFF data bytes) but stops
     * special-casing CR: '\r' is injected as a plain data byte
     * instead of triggering the T_CR LF/NUL-swallow state. Without
     * this, any binary stream containing a 0x0D 0x0A or 0x0D 0x00
     * pair gets the second byte silently dropped — for an SLM-OS
     * GGUF upload of ~100 MB that's ~14000 byte drops, every byte
     * after the first drop shifts by one, corrupting the file.
     * Caller flips via the shell_io vtable's set_binary_mode hook
     * (TCP backend forwards here; non-telnet backends no-op). */
    bool              binary_mode;
    struct telnet_ops ops;
};

/* ============================================================================
 * Public API
 * ============================================================================ */

/* Initialize a fresh parser. `ops` may be copied by value; the
 * caller's ops struct doesn't need to outlive the init call (but
 * `ops.ctx` and the function pointers must remain valid for the
 * lifetime of the parser). */
void telnet_init(struct telnet_parser *tp, const struct telnet_ops *ops);

/* Emit the server's initial option negotiation:
 *   IAC WILL ECHO
 *   IAC WILL SUPPRESS-GO-AHEAD
 *   IAC DO NAWS
 *   IAC DO TERMINAL-TYPE
 * Call once right after telnet_init to proactively negotiate
 * character-at-a-time server-echoed mode. Raw clients that don't
 * speak telnet see 12 bytes of noise on connection. */
void telnet_send_initial_negotiation(struct telnet_parser *tp);

/* Feed one byte of received traffic into the parser. Depending on
 * current state, the byte is forwarded to ops.inject_rx, consumed
 * internally as part of an IAC sequence, or triggers a response
 * via ops.send_to_peer / one of the on_* callbacks. */
void telnet_rx_byte(struct telnet_parser *tp, uint8_t b);

/* Convenience: feed a buffer. */
void telnet_rx(struct telnet_parser *tp, const uint8_t *buf, size_t len);

#endif /* TELNET_H */
