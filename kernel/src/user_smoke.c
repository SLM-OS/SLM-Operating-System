/*
 * user_smoke.c - Smoke EL0 task for #697 PR-4.
 *
 * Runs at EL0 in a per-task TTBR0_EL1 address space. Logs a fixed
 * banner via SYS_LOG, then exits via SYS_EXIT. Exists primarily to
 * prove that the EL0 entry plumbing (per-task L1 from PR-3, .text.user
 * + stack mappings from PR-4, ERET from user_entry.S) actually
 * delivers EL0 → EL1 → EL0 round-trips on QEMU virt and Pi 5.
 *
 * Section attributes ((".text.user") / (".rodata.user")) place all
 * symbols in the page-aligned `.text.user` linker output section so a
 * per-task L1 can map exactly this range with VMM_FLAG_USER without
 * exposing adjacent kernel pages. PC-relative references (adrp/add)
 * inside this section work at the user VA because the relative offset
 * between code and rodata is preserved by the contiguous user mapping.
 *
 * NOTE: every function defined here MUST carry the section attribute
 * — anything that falls into plain `.text` lives at a kernel VA the
 * user L1 doesn't map and would fault on first fetch.
 */

#include "user_syscall.h"

#define USER_SMOKE_SECTION   __attribute__((section(".text.user"), used, noinline))
#define USER_SMOKE_RODATA    __attribute__((section(".rodata.user"), used))

/* Fixed banner — must live in .rodata.user so adrp+add inside
 * user_smoke_main resolves to a user VA at runtime. */
static const char user_smoke_banner[] USER_SMOKE_RODATA =
    "[USERTEST] hello\n";

/* Length captured at compile time so the user code doesn't pull in a
 * libc strlen — there is no libc at EL0. */
#define USER_SMOKE_BANNER_LEN ((uint32_t)(sizeof(user_smoke_banner) - 1))

/*
 * EL0 entry point. Called via ERET from user_task_enter (kernel/arch/
 * arm64/user_entry.S) with SP_EL0 set to the per-task user stack and
 * TTBR0_EL1 pointing at the per-task L1.
 *
 * The function takes a void* for compatibility with task_entry_t;
 * the smoke does not use the argument.
 */
void user_smoke_main(void *arg) USER_SMOKE_SECTION;
void user_smoke_main(void *arg)
{
    (void)arg;
    sys_log(user_smoke_banner, USER_SMOKE_BANNER_LEN);
    sys_exit(0);
}
