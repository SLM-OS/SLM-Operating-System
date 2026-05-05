/*
 * telnet.c - RFC 854 IAC state machine implementation
 *
 * See telnet.h for the public interface and scope.
 */

#include "telnet.h"
#include "string.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ============================================================================
 * Response helpers
 * ============================================================================ */

static void send_iac3(struct telnet_parser *tp, uint8_t cmd, uint8_t opt)
{
    uint8_t buf[3] = { TELNET_IAC, cmd, opt };
    tp->ops.send_to_peer(tp->ops.ctx, buf, sizeof(buf));
}

/* Send IAC SB TERMINAL-TYPE SEND IAC SE (request the peer's terminal
 * type string). */
static void send_ttype_request(struct telnet_parser *tp)
{
    const uint8_t buf[] = {
        TELNET_IAC, TELNET_SB, TELNET_OPT_TTYPE, TELNET_TTYPE_SEND,
        TELNET_IAC, TELNET_SE,
    };
    tp->ops.send_to_peer(tp->ops.ctx, buf, sizeof(buf));
}

/* ============================================================================
 * Option handlers
 *
 * Respond to the peer's WILL/WONT/DO/DONT, tracking our own state so
 * we don't chat in a negotiation loop. Convention (RFC 1143, "The Q
 * method") simplified: we only respond when the answer would actually
 * change state.
 * ============================================================================ */

static void handle_opt_do(struct telnet_parser *tp, uint8_t opt)
{
    /* Peer asks us to turn option ON (client's "please DO x") */
    switch (opt) {
    case TELNET_OPT_ECHO:
        if (!(tp->negotiated_flags & TELNET_F_WILL_ECHO)) {
            tp->negotiated_flags |= TELNET_F_WILL_ECHO;
            send_iac3(tp, TELNET_WILL, TELNET_OPT_ECHO);
        }
        break;
    case TELNET_OPT_SGA:
        if (!(tp->negotiated_flags & TELNET_F_WILL_SGA)) {
            tp->negotiated_flags |= TELNET_F_WILL_SGA;
            send_iac3(tp, TELNET_WILL, TELNET_OPT_SGA);
        }
        break;
    default:
        /* Refuse anything else. */
        send_iac3(tp, TELNET_WONT, opt);
        break;
    }
}

static void handle_opt_dont(struct telnet_parser *tp, uint8_t opt)
{
    /* Peer asks us to turn option OFF. RFC 1143 Q-method: only reply
     * when state actually changes, to avoid negotiation loops. */
    switch (opt) {
    case TELNET_OPT_ECHO:
        if (tp->negotiated_flags & TELNET_F_WILL_ECHO) {
            tp->negotiated_flags &= ~TELNET_F_WILL_ECHO;
            send_iac3(tp, TELNET_WONT, TELNET_OPT_ECHO);
        }
        break;
    case TELNET_OPT_SGA:
        if (tp->negotiated_flags & TELNET_F_WILL_SGA) {
            tp->negotiated_flags &= ~TELNET_F_WILL_SGA;
            send_iac3(tp, TELNET_WONT, TELNET_OPT_SGA);
        }
        break;
    default:
        /* Option we never had on; stay silent (Q-method). */
        break;
    }
}

static void handle_opt_will(struct telnet_parser *tp, uint8_t opt)
{
    /* Peer offers to turn option ON on their side */
    switch (opt) {
    case TELNET_OPT_NAWS:
        if (!(tp->negotiated_flags & TELNET_F_DO_NAWS)) {
            tp->negotiated_flags |= TELNET_F_DO_NAWS;
            send_iac3(tp, TELNET_DO, TELNET_OPT_NAWS);
        }
        break;
    case TELNET_OPT_TTYPE:
        if (!(tp->negotiated_flags & TELNET_F_DO_TTYPE)) {
            tp->negotiated_flags |= TELNET_F_DO_TTYPE;
            send_iac3(tp, TELNET_DO, TELNET_OPT_TTYPE);
            /* Immediately request the terminal type string. */
            send_ttype_request(tp);
        }
        break;
    default:
        /* Refuse anything else. */
        send_iac3(tp, TELNET_DONT, opt);
        break;
    }
}

static void handle_opt_wont(struct telnet_parser *tp, uint8_t opt)
{
    /* Peer refuses / stops option on their side. RFC 1143 Q-method:
     * only reply DONT when we had previously sent DO — otherwise
     * ping-pong with a pedantic peer. */
    switch (opt) {
    case TELNET_OPT_NAWS:
        if (tp->negotiated_flags & TELNET_F_DO_NAWS) {
            tp->negotiated_flags &= ~TELNET_F_DO_NAWS;
            send_iac3(tp, TELNET_DONT, TELNET_OPT_NAWS);
        }
        break;
    case TELNET_OPT_TTYPE:
        if (tp->negotiated_flags & TELNET_F_DO_TTYPE) {
            tp->negotiated_flags &= ~TELNET_F_DO_TTYPE;
            send_iac3(tp, TELNET_DONT, TELNET_OPT_TTYPE);
        }
        break;
    default:
        /* Option we never requested; stay silent. */
        break;
    }
}

/* ============================================================================
 * Subnegotiation dispatch
 * ============================================================================ */

/* NAWS subneg payload layout (RFC 1073):
 *   [0] option = NAWS (31)
 *   [1] width-high  [2] width-low    big-endian cols
 *   [3] height-high [4] height-low   big-endian rows
 * Total = 5 bytes, with cols/rows split across fixed offsets. */
#define NAWS_SUBNEG_LEN          5
#define NAWS_OFFSET_COLS_HIGH    1
#define NAWS_OFFSET_COLS_LOW     2
#define NAWS_OFFSET_ROWS_HIGH    3
#define NAWS_OFFSET_ROWS_LOW     4

/* TERMINAL-TYPE subneg payload (RFC 1091):
 *   [0] option = TTYPE (24)
 *   [1] sub-command (SEND=1, IS=0)
 *   [2..] characters (when IS)
 * Minimum meaningful length is 2 (opt + sub-command). */
#define TTYPE_SUBNEG_MIN_LEN     2
#define TTYPE_OFFSET_SUBCMD      1
#define TTYPE_OFFSET_STRING      2

static void handle_subneg(struct telnet_parser *tp)
{
    if (tp->subneg_len < 1) {
        return;
    }
    uint8_t opt = tp->subneg[0];

    switch (opt) {
    case TELNET_OPT_NAWS: {
        if (tp->subneg_len >= NAWS_SUBNEG_LEN) {
            uint16_t cols = ((uint16_t)tp->subneg[NAWS_OFFSET_COLS_HIGH] << 8)
                          |  tp->subneg[NAWS_OFFSET_COLS_LOW];
            uint16_t rows = ((uint16_t)tp->subneg[NAWS_OFFSET_ROWS_HIGH] << 8)
                          |  tp->subneg[NAWS_OFFSET_ROWS_LOW];
            if (tp->ops.on_naws) {
                tp->ops.on_naws(tp->ops.ctx, cols, rows);
            }
        }
        break;
    }
    case TELNET_OPT_TTYPE: {
        if (tp->subneg_len >= TTYPE_SUBNEG_MIN_LEN
         && tp->subneg[TTYPE_OFFSET_SUBCMD] == TELNET_TTYPE_IS) {
            char term[TELNET_TTYPE_MAX];
            size_t n = tp->subneg_len - TTYPE_OFFSET_STRING;
            if (n >= sizeof(term)) n = sizeof(term) - 1;
            for (size_t i = 0; i < n; i++) {
                term[i] = (char)tp->subneg[TTYPE_OFFSET_STRING + i];
            }
            term[n] = '\0';
            if (tp->ops.on_term_type) {
                tp->ops.on_term_type(tp->ops.ctx, term);
            }
        }
        break;
    }
    default:
        /* Unknown subnegotiation — ignore silently. */
        break;
    }
}

/* ============================================================================
 * IAC command dispatch
 * ============================================================================ */

static void handle_iac_cmd(struct telnet_parser *tp, uint8_t cmd)
{
    switch (cmd) {
    case TELNET_IAC:
        /* IAC IAC == literal 0xFF data byte. */
        tp->ops.inject_rx(tp->ops.ctx, 0xFF);
        tp->state = T_DATA;
        break;
    case TELNET_WILL:
        tp->state = T_OPT_WILL;
        break;
    case TELNET_WONT:
        tp->state = T_OPT_WONT;
        break;
    case TELNET_DO:
        tp->state = T_OPT_DO;
        break;
    case TELNET_DONT:
        tp->state = T_OPT_DONT;
        break;
    case TELNET_SB:
        tp->subneg_len = 0;
        tp->state = T_SB;
        break;
    case TELNET_IP:
        /* Interrupt Process — the telnet equivalent of Ctrl+C. */
        if (tp->ops.on_interrupt) {
            tp->ops.on_interrupt(tp->ops.ctx);
        }
        tp->state = T_DATA;
        break;
    case TELNET_AYT:
        /* "Are You There" — reply with a friendly marker so the
         * user sees something. */
        tp->ops.send_to_peer(tp->ops.ctx,
                             (const uint8_t *)"[SLM-OS]", 8);
        tp->state = T_DATA;
        break;
    case TELNET_NOP:
    case TELNET_DM:
    case TELNET_BRK:
    case TELNET_AO:
    case TELNET_EC:
    case TELNET_EL:
    case TELNET_GA:
        /* Acknowledge the command by returning to data state; no
         * meaningful action in this shell. */
        tp->state = T_DATA;
        break;
    default:
        /* Unknown command byte — resync by returning to data. */
        tp->state = T_DATA;
        break;
    }
}

/* ============================================================================
 * Public API
 * ============================================================================ */

void telnet_init(struct telnet_parser *tp, const struct telnet_ops *ops)
{
    tp->state            = T_DATA;
    tp->subneg_len       = 0;
    tp->negotiated_flags = 0;
    tp->binary_mode      = false;
    tp->ops              = *ops;
}

void telnet_send_initial_negotiation(struct telnet_parser *tp)
{
    /* Four 3-byte IAC sequences = 12 bytes. Send as one write so
     * they hit the wire together. */
    const uint8_t init[] = {
        TELNET_IAC, TELNET_WILL, TELNET_OPT_ECHO,
        TELNET_IAC, TELNET_WILL, TELNET_OPT_SGA,
        TELNET_IAC, TELNET_DO,   TELNET_OPT_NAWS,
        TELNET_IAC, TELNET_DO,   TELNET_OPT_TTYPE,
    };
    tp->negotiated_flags |= TELNET_F_WILL_ECHO | TELNET_F_WILL_SGA
                          | TELNET_F_DO_NAWS   | TELNET_F_DO_TTYPE;
    tp->ops.send_to_peer(tp->ops.ctx, init, sizeof(init));
}

void telnet_rx_byte(struct telnet_parser *tp, uint8_t b)
{
    switch (tp->state) {
    case T_DATA:
        if (b == TELNET_IAC) {
            tp->state = T_IAC;
        } else if (b == '\r' &&
                   !__atomic_load_n(&tp->binary_mode, __ATOMIC_RELAXED)) {
            /* Enter a mini-state that swallows CR LF / CR NUL so the
             * shell sees exactly one submit per keypress, regardless
             * of whether the client sends CR LF (standard telnet)
             * or CR NUL (some clients) or bare LF (raw nc).
             *
             * Skipped in binary_mode: an xput-bin upload of binary
             * data (e.g. a GGUF model) randomly contains 0x0D 0x0A
             * and 0x0D 0x00 pairs at byte-pattern frequency
             * (~1/65536 each). With CR-state swallow active those
             * second bytes get dropped silently and every byte
             * after shifts by one, corrupting the file. */
            tp->ops.inject_rx(tp->ops.ctx, '\r');
            tp->state = T_CR;
        } else {
            tp->ops.inject_rx(tp->ops.ctx, b);
        }
        break;

    case T_CR:
        if ((b == '\n' || b == 0x00) &&
            !__atomic_load_n(&tp->binary_mode, __ATOMIC_RELAXED)) {
            /* Swallow the trailing LF or NUL — shell already saw CR.
             * Defense-in-depth: gated on binary_mode so a future caller
             * that flips binary_mode while the parser is mid-T_CR
             * doesn't drop the next data byte. The xput-bin flow can't
             * actually reach here today (the script terminates the
             * command line with bare LF, so the parser is in T_DATA at
             * binary_mode flip time), but the gate lets the comment on
             * struct telnet_parser.binary_mode hold without caveat. */
        } else if (b == TELNET_IAC) {
            tp->state = T_IAC;
            return;
        } else {
            /* Some other byte — emit it normally. In binary_mode, this
             * is also the path that 0x0A / 0x00 take after the swallow
             * branch's binary_mode check fails. */
            tp->ops.inject_rx(tp->ops.ctx, b);
        }
        tp->state = T_DATA;
        break;

    case T_IAC:
        handle_iac_cmd(tp, b);
        break;

    case T_OPT_WILL:
        handle_opt_will(tp, b);
        tp->state = T_DATA;
        break;

    case T_OPT_WONT:
        handle_opt_wont(tp, b);
        tp->state = T_DATA;
        break;

    case T_OPT_DO:
        handle_opt_do(tp, b);
        tp->state = T_DATA;
        break;

    case T_OPT_DONT:
        handle_opt_dont(tp, b);
        tp->state = T_DATA;
        break;

    case T_SB:
        if (b == TELNET_IAC) {
            tp->state = T_SB_IAC;
        } else if (tp->subneg_len < TELNET_SB_BUF_SIZE) {
            tp->subneg[tp->subneg_len++] = b;
        }
        /* else: silently drop excess — bounded by SB_BUF_SIZE. */
        break;

    case T_SB_IAC:
        if (b == TELNET_SE) {
            handle_subneg(tp);
            tp->state = T_DATA;
        } else if (b == TELNET_IAC) {
            /* Literal 0xFF inside subneg payload. */
            if (tp->subneg_len < TELNET_SB_BUF_SIZE) {
                tp->subneg[tp->subneg_len++] = 0xFF;
            }
            tp->state = T_SB;
        } else {
            /* Malformed — abort subneg. */
            tp->state = T_DATA;
        }
        break;
    }
}

void telnet_rx(struct telnet_parser *tp, const uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        telnet_rx_byte(tp, buf[i]);
    }
}
