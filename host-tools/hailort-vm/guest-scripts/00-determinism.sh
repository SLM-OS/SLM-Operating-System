#!/bin/bash
# Disable every background activity inside the guest that could perturb HailoRT's
# MMIO write order between capture runs. Applied at first boot (build time); the
# changes persist into the captured qcow2 snapshot.
#
# Anything added here must have a stated reason in the comment above the line —
# masked units accumulate fast and the rationale rots without inline notes.

set -euo pipefail

# Apt-daily and unattended-upgrades touch the disk at random intervals and run
# postinst hooks that may load modules out-of-order.
systemctl mask apt-daily.service             || true
systemctl mask apt-daily.timer               || true
systemctl mask apt-daily-upgrade.service     || true
systemctl mask apt-daily-upgrade.timer       || true
systemctl mask unattended-upgrades.service   || true

# NTP / time sync: any clock step can cause HailoRT internal timers to jitter.
systemctl mask systemd-timesyncd.service     || true

# Snapd refreshes itself on a schedule and starts background processes.
systemctl mask snapd.service                 || true
systemctl mask snapd.socket                  || true
systemctl mask snapd.seeded.service          || true

# IRQ rebalancing across CPUs is an explicit source of cross-run nondeterminism.
systemctl mask irqbalance.service            || true

# tuned applies per-profile scheduler / kernel tweaks asynchronously after login.
systemctl mask tuned.service                 || true

# cron timers run user jobs we can't predict.
systemctl mask cron.service                  || true

# Telemetry channels.
systemctl mask motd-news.service             || true
systemctl mask motd-news.timer               || true
systemctl mask ua-timer.service              || true
systemctl mask ua-timer.timer                || true

# Hailo's own monitor daemon, if it appears in a future .deb. Defensive — does
# nothing if the unit is absent.
systemctl mask hailo-monitor.service         || true

# Pin every default IRQ to CPU 0. New IRQs created post-boot also inherit this.
echo 1 > /proc/irq/default_smp_affinity || true

# ASLR off — already in /etc/sysctl.d/99-hailort-determinism.conf, but enforce now
# so the first boot's hailortcli invocation also sees it.
echo 0 > /proc/sys/kernel/randomize_va_space || true

# Touch a stamp so the rest of the build can confirm this script ran.
touch /opt/hailort-stage/determinism-applied.stamp
