/*
 * kernel/drivers/bpmp/bpmp.c - Public BPMP API (Tegra234)
 *
 * Glue layer tying hsp.c + ivc.c + mrq.c together behind the public
 * bpmp.h interface. Replaces the old monolithic kernel/drivers/bpmp.c
 * that shipped with #190 and never successfully completed a single
 * MRQ round-trip post-kexec (root causes: wrong IVC struct layout,
 * missing handshake, hardcoded doorbell offset).
 */

#include "platform.h"
#include "bpmp.h"

#if defined(PLATFORM_JETSON_ORIN_NANO)

#include "hsp.h"
#include "ivc.h"
#include "mrq.h"
#include "debug.h"
#include <stdbool.h>
#include <stdint.h>

static bool g_bpmp_initialised;

int bpmp_init(void)
{
    if (g_bpmp_initialised) {
        return 0;
    }

    int rc = hsp_init(HSP_TOP_BASE);
    if (rc != 0) {
        WARN("BPMP: hsp_init failed rc=%d", rc);
        return rc;
    }

    rc = mrq_init(BPMP_TX_BASE, BPMP_RX_BASE);
    if (rc != 0) {
        WARN("BPMP: mrq_init failed rc=%d", rc);
        return rc;
    }

    g_bpmp_initialised = true;
    return 0;
}

bool bpmp_is_available(void)
{
    if (!g_bpmp_initialised) {
        return false;
    }

    /*
     * MRQ_PING payload is a 4-byte challenge (see linux-bpmp-abi.h
     * struct mrq_ping_request). BPMP replies with reply = challenge
     * left-shifted by 1, carry-bit discarded (per the ABI doc,
     * not a rotate).
     */
    uint32_t challenge = 0xDEADBEEF;
    uint32_t expected  = challenge << 1;
    uint32_t reply     = 0;
    int32_t  err       = -1;

    int rc = mrq_send(MRQ_PING,
                      &challenge, sizeof(challenge),
                      &reply, sizeof(reply),
                      &err);
    if (rc != 0) {
        WARN("BPMP: MRQ_PING transport rc=%d", rc);
        return false;
    }
    if (err != 0) {
        WARN("BPMP: MRQ_PING rejected by firmware (err=%ld)", (long)err);
        return false;
    }
    if (reply != expected) {
        WARN("BPMP: MRQ_PING reply mismatch (reply=0x%08lx expected=0x%08lx)",
             (unsigned long)reply, (unsigned long)expected);
        return false;
    }
    INFO("BPMP: MRQ_PING OK (reply=0x%08lx)", (unsigned long)reply);
    return true;
}

/* ============================================================================
 * MRQ_CLK wrappers
 * ============================================================================ */

struct mrq_clk_payload_v1 {
    uint32_t cmd_and_id;
    uint32_t reserved[3];
};

static int bpmp_clk_command(uint32_t cmd, uint32_t clock_id)
{
    if (!g_bpmp_initialised) {
        return -1;
    }

    struct mrq_clk_payload_v1 req = {
        .cmd_and_id = (cmd << 24) | (clock_id & 0x00FFFFFF),
    };

    int32_t err = 0;
    int rc = mrq_send(MRQ_CLK, &req, sizeof(req), NULL, 0, &err);
    if (rc != 0) {
        return rc;
    }
    return (int)err;
}

int bpmp_clk_enable(uint32_t clock_id)
{
    return bpmp_clk_command(CMD_CLK_ENABLE, clock_id);
}

int bpmp_clk_disable(uint32_t clock_id)
{
    return bpmp_clk_command(CMD_CLK_DISABLE, clock_id);
}

int bpmp_clk_is_enabled(uint32_t clock_id, int *state_out)
{
    if (!g_bpmp_initialised || state_out == NULL) {
        return -1;
    }

    struct mrq_clk_payload_v1 req = {
        .cmd_and_id = (CMD_CLK_IS_ENABLED << 24) | (clock_id & 0x00FFFFFF),
    };

    /* Response is struct cmd_clk_is_enabled_response { int32_t state; } */
    int32_t reply_state = 0;
    int32_t err = 0;
    int rc = mrq_send(MRQ_CLK, &req, sizeof(req),
                      &reply_state, sizeof(reply_state),
                      &err);
    if (rc != 0) {
        return rc;
    }
    if (err != 0) {
        return (int)err;
    }

    *state_out = (int)reply_state;
    return 0;
}

/* ============================================================================
 * MRQ_RESET wrappers
 * ============================================================================ */

struct mrq_reset_request {
    uint32_t cmd;
    uint32_t reset_id;
};

static int bpmp_reset_command(uint32_t cmd, uint32_t reset_id)
{
    if (!g_bpmp_initialised) {
        return -1;
    }

    struct mrq_reset_request req = { .cmd = cmd, .reset_id = reset_id };

    int32_t err = 0;
    int rc = mrq_send(MRQ_RESET, &req, sizeof(req), NULL, 0, &err);
    if (rc != 0) {
        return rc;
    }
    return (int)err;
}

int bpmp_reset_assert(uint32_t reset_id)
{
    return bpmp_reset_command(CMD_RESET_ASSERT, reset_id);
}

int bpmp_reset_deassert(uint32_t reset_id)
{
    return bpmp_reset_command(CMD_RESET_DEASSERT, reset_id);
}

/* ============================================================================
 * MRQ_UPHY
 * ============================================================================ */

/*
 * Wire layout from docs/reference/linux-bpmp-abi.h struct mrq_uphy_request:
 *
 *   offset 0   uint16_t lane
 *   offset 2   uint16_t cmd
 *   offset 4+  union { sub-command payloads }
 *
 * For CMD_UPHY_PCIE_CONTROLLER_STATE the sub-command payload is:
 *   offset 4   uint8_t pcie_controller
 *   offset 5   uint8_t enable
 * Total packed size: 6 bytes (Linux quotes "6" in the ABI doc
 * "Minimum payload length" table).
 */
struct mrq_uphy_ctrl_state_payload {
    uint16_t lane;                /* 0 — we don't target a specific lane */
    uint16_t cmd;                 /* CMD_UPHY_PCIE_CONTROLLER_STATE */
    uint8_t  pcie_controller;
    uint8_t  enable;
};

int bpmp_uphy_pcie_controller_state(uint32_t pcie_controller_id, bool enable)
{
    if (!g_bpmp_initialised) {
        return -1;
    }

    struct mrq_uphy_ctrl_state_payload req = {
        .lane            = 0,
        .cmd             = CMD_UPHY_PCIE_CONTROLLER_STATE,
        .pcie_controller = (uint8_t)(pcie_controller_id & 0xFF),
        .enable          = enable ? 1u : 0u,
    };

    int32_t err = 0;
    int rc = mrq_send(MRQ_UPHY, &req, sizeof(req), NULL, 0, &err);
    if (rc != 0) {
        return rc;
    }
    return (int)err;
}

/* ============================================================================
 * MRQ_PG (Power Gate / Power Domain)
 * ============================================================================ */

/*
 * linux-bpmp-abi.h struct mrq_pg_request for CMD_PG_SET_STATE:
 *   offset 0   uint32  cmd    (1 = CMD_PG_SET_STATE)
 *   offset 4   uint32  id     (TEGRA234_POWER_DOMAIN_*)
 *   offset 8   uint32  state  (PG_STATE_ON=1, _OFF=0)
 * Total 12 bytes (matches ABI doc).
 */
struct mrq_pg_set_state_payload {
    uint32_t cmd;
    uint32_t id;
    uint32_t state;
};

int bpmp_pg_set_state(uint32_t domain_id, bool on)
{
    if (!g_bpmp_initialised) {
        return -1;
    }

    struct mrq_pg_set_state_payload req = {
        .cmd   = CMD_PG_SET_STATE,
        .id    = domain_id,
        .state = on ? PG_STATE_ON : PG_STATE_OFF,
    };

    int32_t err = 0;
    int rc = mrq_send(MRQ_PG, &req, sizeof(req), NULL, 0, &err);
    if (rc != 0) {
        return rc;
    }
    return (int)err;
}

#else /* !PLATFORM_JETSON_ORIN_NANO */

/* Stubs for QEMU / Pi 5 / x86-64. */

int  bpmp_init(void)                         { return 0; }
bool bpmp_is_available(void)                 { return false; }
int  bpmp_clk_enable(uint32_t id)            { (void)id; return 0; }
int  bpmp_clk_disable(uint32_t id)           { (void)id; return 0; }
int  bpmp_clk_is_enabled(uint32_t id, int *out) { (void)id; if (out) *out = 0; return 0; }
int  bpmp_reset_assert(uint32_t id)          { (void)id; return 0; }
int  bpmp_reset_deassert(uint32_t id)        { (void)id; return 0; }
int  bpmp_uphy_pcie_controller_state(uint32_t id, bool en) { (void)id; (void)en; return 0; }
int  bpmp_pg_set_state(uint32_t id, bool on) { (void)id; (void)on; return 0; }

#endif /* PLATFORM_JETSON_ORIN_NANO */
