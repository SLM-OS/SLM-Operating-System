/*
 * timdiag_pi5_probes.h — Track B hardware probes for Pi 5 timer-IRQ
 * delivery (issue #134). See kernel/src/timdiag_pi5_probes.c for
 * the design and per-probe rationale.
 *
 * Driven from kernel/src/shell_sys.c:cmd_timdiag via the
 * `timdiag {sgi,bypass,smc}` subcommands. All probes are gated on
 * PLATFORM_RASPI5 + PI5_IRQ_DIAG; outside that gate the symbols
 * are not defined and the shell dispatch is also #ifdef'd out.
 */

#ifndef TIMDIAG_PI5_PROBES_H
#define TIMDIAG_PI5_PROBES_H

#include "config.h"

#if defined(PLATFORM_RASPI5) && defined(PI5_IRQ_DIAG)

void timdiag_pi5_probe_sgi(void);
void timdiag_pi5_probe_bypass(void);
void timdiag_pi5_probe_smc(void);

#endif /* PLATFORM_RASPI5 && PI5_IRQ_DIAG */

#endif /* TIMDIAG_PI5_PROBES_H */
