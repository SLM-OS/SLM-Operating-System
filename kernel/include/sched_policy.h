/*
 * sched_policy.h - Pluggable scheduler policy interface for SLM-OS
 *
 * Defines a vtable that allows different CPU assignment strategies to be
 * swapped at runtime. The scheduler core (run queues, locking, context
 * switch, pick_next_task) is unchanged — policies only control which CPU
 * a new task is placed on and optionally act on timer ticks.
 *
 * Built-in policy: "heuristic" (round-robin load balancing with deadline
 * pressure). AI policies (MLP, PPO) are added when CONFIG_AI_SCHEDULER
 * is enabled.
 */

#ifndef SCHED_POLICY_H
#define SCHED_POLICY_H

#include "task.h"
#include <stdint.h>

/* Maximum length of a policy name (including null terminator) */
#define SCHED_POLICY_MAX_NAME  16

/* Maximum number of registered policies */
#define SCHED_POLICY_MAX       8

/*
 * Scheduler policy operations.
 *
 * A policy implements CPU assignment for new tasks. The scheduler core
 * handles everything else: run queue insertion, locking, context switch,
 * and pick_next_task().
 *
 * All callbacks are called with interrupts in an unspecified state —
 * implementations must not assume IRQs are enabled or disabled.
 */
struct sched_policy_ops {
    /* Human-readable name (must be <= SCHED_POLICY_MAX_NAME-1 chars) */
    const char *name;

    /*
     * Initialize the policy. Called when the policy is activated via
     * sched_set_policy(). May be NULL if no init is needed.
     *
     * Returns: 0 on success, negative on error (policy will not be activated)
     */
    int (*init)(void);

    /*
     * Shut down the policy. Called when switching away to a different
     * policy. May be NULL if no cleanup is needed.
     */
    void (*shutdown)(void);

    /*
     * Select a CPU for a new task.
     *
     * Called from scheduler_add_task() when task->cpu_affinity is
     * CPU_AFFINITY_ANY. Tasks with explicit affinity bypass this callback.
     *
     * @task: The task being added (read-only except effective_priority)
     *
     * Returns: Target CPU ID (must be < cpu_count and not isolated,
     *          unless the task has explicit affinity)
     */
    uint32_t (*assign_cpu)(struct task *task);

    /*
     * Per-tick callback for periodic rebalancing or stats collection.
     * Called from scheduler_tick() on each CPU. May be NULL.
     *
     * @cpu: The CPU that received the timer tick
     */
    void (*tick)(uint32_t cpu);
};

/*
 * Register a policy so it can be selected by name via sched_set_policy()
 * or the shell command.
 *
 * Returns: 0 on success, -1 if registry is full
 */
int sched_register_policy(const struct sched_policy_ops *policy);

/*
 * Switch to a different scheduling policy.
 *
 * Calls shutdown() on the current policy (if non-NULL), then init() on
 * the new policy. If init() fails, the previous policy remains active.
 *
 * Thread-safe: disables interrupts during the swap.
 *
 * @policy: Policy ops to activate
 *
 * Returns: 0 on success, -1 on error (init failed or NULL policy)
 */
int sched_set_policy(const struct sched_policy_ops *policy);

/*
 * Get the name of the currently active policy.
 *
 * Returns: Policy name string (never NULL)
 */
const char *sched_get_policy(void);

/*
 * Look up a registered policy by name.
 *
 * Returns: Policy ops pointer, or NULL if not found
 */
const struct sched_policy_ops *sched_find_policy(const char *name);

/*
 * Get the number of registered policies.
 */
int sched_policy_count(void);

/*
 * Get a registered policy by index (for enumeration).
 *
 * Returns: Policy ops pointer, or NULL if index out of range
 */
const struct sched_policy_ops *sched_policy_get(int index);

/* Built-in heuristic policy (always available) */
extern const struct sched_policy_ops sched_policy_heuristic;

#endif /* SCHED_POLICY_H */
