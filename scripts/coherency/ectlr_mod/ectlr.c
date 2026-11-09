// SPDX-License-Identifier: GPL-2.0
/*
 * ectlr.c - #1193 read-only dump of IMPLEMENTATION DEFINED control
 * registers on every online CPU.
 *
 * READ ONLY: this module issues MRS only. It contains no MSR to any
 * register; a write to CPUECTLR/CLUSTERECTLR/CPUACTLR* from a lower EL is
 * gated by ACTLR_EL3 and can trap to EL3 (fatal on Jetson BL31).
 *
 * Registers, per CPU, via smp_call_function_single():
 *   MIDR_EL1, MPIDR_EL1, CurrentEL
 *   CPUECTLR_EL1     S3_0_C15_C1_4  (op0 3, op1 0, CRn 15, CRm 1, op2 4)
 *       Cortex-A76 TRM 100798_0401_01 table at line 4915 of the local
 *       extract, Cortex-A78AE TRM 101779_0003_08 line 5522.
 *   CLUSTERECTLR_EL1 S3_0_C15_C3_4  only with cluster=1
 *       DSU TRM 100453_0401_05 section 11.8 (local extract line 5786).
 *       Confirmed for the DSU (Pi 5, Cortex-A76). Jetson's Cortex-A78AE
 *       uses the DSU-AE, which this TRM does not cover, so the parameter
 *       defaults to 0 and is set only on the Pi 5.
 *
 * Output is raw hex. No bit is decoded: bit 6 of CPUECTLR is RES0 on the
 * A76 and PF_MODE[0] on the A78AE, and neither is SMPEN.
 */
#include <linux/cpu.h>
#include <linux/module.h>
#include <linux/smp.h>

static bool cluster;
module_param(cluster, bool, 0444);
MODULE_PARM_DESC(cluster, "also read CLUSTERECTLR_EL1 (DSU only; default off)");

struct ectlr_regs {
	u64 midr, mpidr, currentel, cpuectlr, clusterectlr;
	int cpu;
};

static void ectlr_read(void *arg)
{
	struct ectlr_regs *r = arg;

	r->cpu = smp_processor_id();
	asm volatile("mrs %0, midr_el1" : "=r"(r->midr));
	asm volatile("mrs %0, mpidr_el1" : "=r"(r->mpidr));
	asm volatile("mrs %0, CurrentEL" : "=r"(r->currentel));
	asm volatile("mrs %0, S3_0_C15_C1_4" : "=r"(r->cpuectlr));
	if (cluster) {
		asm volatile("mrs %0, S3_0_C15_C3_4" : "=r"(r->clusterectlr));
	}
}

static int __init ectlr_init(void)
{
	unsigned int cpu;

	pr_info("ectlr1193: begin cluster=%d\n", cluster);
	cpus_read_lock();
	for_each_online_cpu(cpu) {
		struct ectlr_regs r = { .cpu = -1 };
		int rc = smp_call_function_single(cpu, ectlr_read, &r, 1);

		if (rc) {
			pr_info("ectlr1193: cpu%u call failed rc=%d\n", cpu, rc);
			continue;
		}
		if (cluster) {
			pr_info("ectlr1193: cpu%u ran_on=%d midr=0x%016llx mpidr=0x%016llx currentel=0x%llx cpuectlr=0x%016llx clusterectlr=0x%016llx\n",
				cpu, r.cpu, r.midr, r.mpidr, r.currentel,
				r.cpuectlr, r.clusterectlr);
		} else {
			pr_info("ectlr1193: cpu%u ran_on=%d midr=0x%016llx mpidr=0x%016llx currentel=0x%llx cpuectlr=0x%016llx clusterectlr=not-read\n",
				cpu, r.cpu, r.midr, r.mpidr, r.currentel, r.cpuectlr);
		}
	}
	cpus_read_unlock();
	pr_info("ectlr1193: end\n");
	return 0;
}

static void __exit ectlr_exit(void)
{
}

module_init(ectlr_init);
module_exit(ectlr_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("#1193 read-only CPUECTLR/CLUSTERECTLR dump");
