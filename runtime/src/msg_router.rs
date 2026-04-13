//! Topic-based publish/subscribe message router for component IPC.
//!
//! Provides named topics that components can subscribe to.
//! Messages published to a topic are delivered to all subscribers
//! via per-subscriber mailboxes with atomic ready/ack flags.
//!
//! # FFI
//!
//! All public functions are `#[no_mangle] extern "C"` and match the
//! C API in `msg_router.c` (which is removed from the build when
//! the Rust library is linked).

use core::sync::atomic::{AtomicBool, AtomicU32, Ordering};

// =============================================================================
// Constants
// =============================================================================

const MAX_TOPICS: usize = 8;
const MAX_SUBSCRIBERS: usize = 4;
const MAX_MSG_LEN: usize = 60;
const TOPIC_NAME_LEN: usize = 16;
const MAX_WILDCARD_SUBS: usize = 8;
const MSG_PRIORITY_NORMAL: u8 = 0;

/// Ack timeout in seconds. Converted to hardware counter cycles at the
/// call site via `timer_get_frequency()` so the timeout is correct
/// regardless of TIMER_HZ or whether timer IRQs are masked.
const ACK_TIMEOUT_SECS: u64 = 5;

// =============================================================================
// External C functions
// =============================================================================

extern "C" {
    fn uart_puts(s: *const u8);
    fn uart_printf(fmt: *const u8, ...);
    #[link_name = "yield"]
    fn sched_yield();
    /// ARM generic timer counter (CNTPCT_EL0). Always advances regardless
    /// of DAIF.I state — safe to poll from tasks on Pi 5 where timer IRQs
    /// don't fire while a task runs.
    fn timer_get_count() -> u64;
    /// ARM generic timer frequency (CNTFRQ_EL0), in Hz.
    fn timer_get_frequency() -> u64;
    fn slm_irq_save() -> u64;
    fn slm_irq_restore(flags: u64);
}

fn puts(s: &[u8]) {
    unsafe { uart_puts(s.as_ptr()) }
}

// =============================================================================
// Data Structures
// =============================================================================

/// Per-subscriber mailbox for message delivery.
struct Mailbox {
    ready: AtomicU32,
    ack: AtomicU32,
    data: [u8; MAX_MSG_LEN],
    topic: [u8; TOPIC_NAME_LEN],
    /// Atomic to prevent reordering: priority must be visible before ready=1.
    priority: AtomicU32,
}

impl Mailbox {
    const fn new() -> Self {
        Self {
            ready: AtomicU32::new(0),
            ack: AtomicU32::new(0),
            data: [0; MAX_MSG_LEN],
            topic: [0; TOPIC_NAME_LEN],
            priority: AtomicU32::new(0),
        }
    }

    fn clear(&mut self) {
        self.ready.store(0, Ordering::Release);
        self.ack.store(0, Ordering::Release);
        self.data = [0; MAX_MSG_LEN];
        self.topic = [0; TOPIC_NAME_LEN];
        self.priority.store(0, Ordering::Release);
    }

    /// Deliver a message to this mailbox. Writes data and topic first,
    /// then priority (Release), then ready=1 (Release) so the receiver
    /// sees consistent data when it reads ready=1 (Acquire).
    ///
    /// # Safety
    /// Caller promises `topic_name` and `data` are NUL-terminated within
    /// `TOPIC_NAME_LEN` and `MAX_MSG_LEN` bytes respectively.
    unsafe fn deliver(&mut self, topic_name: *const u8, data: *const u8, prio: u8) {
        str_copy(&mut self.data, data, MAX_MSG_LEN);
        str_copy(&mut self.topic, topic_name, TOPIC_NAME_LEN);
        self.ack.store(0, Ordering::Release);
        self.priority.store(prio as u32, Ordering::Release);
        self.ready.store(1, Ordering::Release);
    }
}

/// A subscription linking a component to its mailbox.
struct Subscription {
    component_idx: i32, // -1 = unused
    mailbox: Mailbox,
}

impl Subscription {
    const fn new() -> Self {
        Self {
            component_idx: -1,
            mailbox: Mailbox::new(),
        }
    }

    fn clear(&mut self) {
        self.component_idx = -1;
        self.mailbox.clear();
    }
}

/// A named topic with its subscribers.
struct Topic {
    name: [u8; TOPIC_NAME_LEN],
    subs: [Subscription; MAX_SUBSCRIBERS],
    sub_count: i32,
}

impl Topic {
    const fn new() -> Self {
        Self {
            name: [0; TOPIC_NAME_LEN],
            subs: [
                Subscription::new(),
                Subscription::new(),
                Subscription::new(),
                Subscription::new(),
            ],
            sub_count: 0,
        }
    }

    fn clear(&mut self) {
        self.name = [0; TOPIC_NAME_LEN];
        self.sub_count = 0;
        for sub in &mut self.subs {
            sub.clear();
        }
    }

    fn is_active(&self) -> bool {
        self.name[0] != 0
    }
}

// =============================================================================
// Global State
// =============================================================================

static mut TOPICS: [Topic; MAX_TOPICS] = [
    Topic::new(), Topic::new(), Topic::new(), Topic::new(),
    Topic::new(), Topic::new(), Topic::new(), Topic::new(),
];
static mut TOPIC_COUNT: i32 = 0;

/// Wildcard subscription: pattern ending in '*' matches topic prefixes.
struct WildcardSub {
    pattern: [u8; TOPIC_NAME_LEN], // e.g., "/sensors/*"
    component_idx: i32,            // -1 = unused
    mailbox: Mailbox,
}

impl WildcardSub {
    const fn new() -> Self {
        Self {
            pattern: [0; TOPIC_NAME_LEN],
            component_idx: -1,
            mailbox: Mailbox::new(),
        }
    }

    fn clear(&mut self) {
        self.pattern = [0; TOPIC_NAME_LEN];
        self.component_idx = -1;
        self.mailbox.clear();
    }

    fn is_active(&self) -> bool {
        self.pattern[0] != 0
    }
}

static mut WILDCARD_SUBS: [WildcardSub; MAX_WILDCARD_SUBS] = [
    WildcardSub::new(), WildcardSub::new(),
    WildcardSub::new(), WildcardSub::new(),
    WildcardSub::new(), WildcardSub::new(),
    WildcardSub::new(), WildcardSub::new(),
];

/// Tracks the mailbox source returned by the last msg_router_receive() call
/// for each component, so msg_router_ack() targets the exact same mailbox.
/// Prevents race conditions when a higher-priority message arrives between
/// receive() and ack().
const MAX_COMPONENTS: usize = 32;

#[derive(Clone, Copy)]
struct LastReceived {
    topic_idx: i32,    // -1 = none, 0..MAX_TOPICS = exact, >=MAX_TOPICS = invalid
    sub_idx: i32,      // subscriber slot within topic
    wildcard_idx: i32, // -1 = not wildcard, 0..MAX_WILDCARD_SUBS = wildcard slot
}

impl LastReceived {
    const fn none() -> Self {
        Self { topic_idx: -1, sub_idx: -1, wildcard_idx: -1 }
    }
}

static mut LAST_RECEIVED: [LastReceived; MAX_COMPONENTS] = [LastReceived::none(); MAX_COMPONENTS];

/// Spinlock protecting all router state: `TOPICS`, `TOPIC_COUNT`,
/// `WILDCARD_SUBS`, `LAST_RECEIVED`.
///
/// The lock is released before the publish ack-wait loop (which yields)
/// to avoid deadlocking with subscribers that call `msg_router_ack()`.
static MSG_ROUTER_LOCK: AtomicBool = AtomicBool::new(false);

/// RAII guard for `MSG_ROUTER_LOCK`.
///
/// Disables local IRQs on acquire and restores them on drop. Preemption
/// is also disabled implicitly (timer IRQ is masked) so a task holding
/// the lock cannot be preempted into another task that tries to publish.
/// This guards against any ISR path that calls into the router once
/// preemptive multi-core is enabled (#57).
struct SpinGuard {
    irq_flags: u64,
}

impl SpinGuard {
    fn new() -> Self {
        // SAFETY: slm_irq_save is a kernel FFI that reads DAIF (ARM64) /
        // EFLAGS (x86_64) and masks IRQs. Always safe to call.
        let irq_flags = unsafe { slm_irq_save() };
        while MSG_ROUTER_LOCK
            .compare_exchange_weak(false, true, Ordering::Acquire, Ordering::Relaxed)
            .is_err()
        {
            core::hint::spin_loop();
        }
        SpinGuard { irq_flags }
    }
}

impl Drop for SpinGuard {
    fn drop(&mut self) {
        MSG_ROUTER_LOCK.store(false, Ordering::Release);
        // SAFETY: restoring the caller's prior IRQ state; flags came from
        // the paired slm_irq_save() in new().
        unsafe { slm_irq_restore(self.irq_flags) };
    }
}

// =============================================================================
// String Helpers
// =============================================================================

/// Compare a C string (null-terminated) with a byte slice in our topic
/// name buffer. Bounded by `buf.len() + 1` bytes on the cstr side — the
/// extra byte is the null terminator check at `i == buf.len()`.
fn str_eq_cstr(buf: &[u8], cstr: *const u8) -> bool {
    let mut i = 0;
    // Walk at most `buf.len()` positions; the final iteration reads one
    // more byte from `cstr` to confirm termination, which is within the
    // caller's bound as long as `cstr` has at least `buf.len() + 1` bytes
    // of readable memory (true for all callers — strings passed in are
    // NUL-terminated within TOPIC_NAME_LEN).
    loop {
        let a = if i < buf.len() { buf[i] } else { 0 };
        // SAFETY: `i <= buf.len()` and caller contract guarantees cstr is
        // NUL-terminated within buf.len() + 1 bytes.
        let b = unsafe { *cstr.add(i) };
        if a == 0 && b == 0 {
            return true;
        }
        if a != b {
            return false;
        }
        if i >= buf.len() {
            // Should not reach here if either a or b was 0 above.
            return false;
        }
        i += 1;
    }
}

/// Copy a NUL-terminated C string into a fixed-size buffer.
///
/// Reads at most `max_src_len` bytes from `src`, leaves space for a NUL
/// terminator in `dst`. The caller must pass the upper bound for the
/// source string so an unterminated input cannot read past allocated
/// memory.
fn str_copy(dst: &mut [u8], src: *const u8, max_src_len: usize) {
    let max_dst = dst.len();
    if max_dst == 0 {
        return;
    }
    let limit = core::cmp::min(max_dst - 1, max_src_len);
    let mut i = 0;
    // SAFETY: the loop guard `i < limit` keeps the deref within
    // `max_src_len` of `src`, which the caller guarantees is readable.
    unsafe {
        while i < limit {
            let c = *src.add(i);
            if c == 0 {
                break;
            }
            dst[i] = c;
            i += 1;
        }
    }
    dst[i] = 0;
}

/// Check if a pattern (NUL-terminated within `max_len` bytes) ends with '*'.
fn is_wildcard_pattern(name: *const u8, max_len: usize) -> bool {
    let mut i = 0;
    let mut last = 0u8;
    // SAFETY: loop guard bounds deref within `max_len` of `name`.
    unsafe {
        while i < max_len {
            let c = *name.add(i);
            if c == 0 {
                break;
            }
            last = c;
            i += 1;
        }
    }
    last == b'*'
}

/// Check if a topic name matches a wildcard pattern.
/// Pattern "/sensors/*" matches "/sensors/data", "/sensors/temp", etc.
fn wildcard_matches(pattern: &[u8; TOPIC_NAME_LEN], topic: *const u8) -> bool {
    // Find the '*' position in pattern
    let mut prefix_len = 0;
    while prefix_len < TOPIC_NAME_LEN && pattern[prefix_len] != 0 && pattern[prefix_len] != b'*' {
        prefix_len += 1;
    }
    // No '*' found — not a wildcard
    if prefix_len >= TOPIC_NAME_LEN || pattern[prefix_len] != b'*' {
        return false;
    }
    // Compare prefix
    unsafe {
        for i in 0..prefix_len {
            if *topic.add(i) == 0 || *topic.add(i) != pattern[i] {
                return false;
            }
        }
    }
    true
}

// =============================================================================
// Public FFI API
// =============================================================================

/// Initialize the message router. Clears all topics and subscriptions.
#[no_mangle]
pub extern "C" fn msg_router_init() {
    let _g = SpinGuard::new();
    // SAFETY: MSG_ROUTER_LOCK held — exclusive access to router statics.
    unsafe {
        TOPIC_COUNT = 0;
        for topic in &mut *core::ptr::addr_of_mut!(TOPICS) {
            topic.clear();
        }
        for ws in &mut *core::ptr::addr_of_mut!(WILDCARD_SUBS) {
            ws.clear();
        }
        for lr in &mut *core::ptr::addr_of_mut!(LAST_RECEIVED) {
            *lr = LastReceived::none();
        }
    }
}

/// Subscribe a component to a topic. Creates the topic if it doesn't exist.
/// If the topic name ends with '*', creates a wildcard subscription that
/// matches all topics with the given prefix (e.g., "/sensors/*" matches
/// "/sensors/data", "/sensors/temp").
/// Returns 0 on success, -1 on error.
#[no_mangle]
pub extern "C" fn msg_router_subscribe(topic_name: *const u8, component_idx: i32) -> i32 {
    if topic_name.is_null() {
        return -1;
    }

    // Wildcard subscription
    if is_wildcard_pattern(topic_name, TOPIC_NAME_LEN) {
        let _g = SpinGuard::new();
        // SAFETY: MSG_ROUTER_LOCK held — exclusive access to WILDCARD_SUBS.
        unsafe {
            for i in 0..MAX_WILDCARD_SUBS {
                if !WILDCARD_SUBS[i].is_active() {
                    str_copy(&mut WILDCARD_SUBS[i].pattern, topic_name, TOPIC_NAME_LEN);
                    WILDCARD_SUBS[i].component_idx = component_idx;
                    WILDCARD_SUBS[i].mailbox.clear();
                    return 0;
                }
            }
        }
        puts(b"[msg] No free wildcard slots\n\0");
        return -1;
    }

    let _g = SpinGuard::new();
    // SAFETY: MSG_ROUTER_LOCK held — exclusive access to TOPICS / TOPIC_COUNT.
    unsafe {
        // Find existing topic
        let mut topic_idx: Option<usize> = None;
        for i in 0..MAX_TOPICS {
            if TOPICS[i].is_active() && str_eq_cstr(&TOPICS[i].name, topic_name) {
                topic_idx = Some(i);
                break;
            }
        }

        // Create new topic if not found
        if topic_idx.is_none() {
            for i in 0..MAX_TOPICS {
                if !TOPICS[i].is_active() {
                    str_copy(&mut TOPICS[i].name, topic_name, TOPIC_NAME_LEN);
                    TOPICS[i].sub_count = 0;
                    topic_idx = Some(i);
                    TOPIC_COUNT += 1;
                    break;
                }
            }
        }

        let idx = match topic_idx {
            Some(i) => i,
            None => {
                puts(b"[msg] No free topic slots\n\0");
                return -1;
            }
        };

        // Add subscriber to first free slot
        for j in 0..MAX_SUBSCRIBERS {
            if TOPICS[idx].subs[j].component_idx == -1 {
                TOPICS[idx].subs[j].component_idx = component_idx;
                TOPICS[idx].subs[j].mailbox.clear();
                TOPICS[idx].sub_count += 1;
                return 0;
            }
        }

        uart_printf(
            b"[msg] Topic '%s' full (%d subscribers)\n\0".as_ptr(),
            topic_name,
            MAX_SUBSCRIBERS as i32,
        );
        -1
    }
}

/// Internal publish with priority. Delivers to exact topic subscribers
/// and wildcard subscribers, then waits for acknowledgment.
///
/// # Concurrency
/// Holds `MSG_ROUTER_LOCK` only while scanning the topic/wildcard arrays
/// to gather target mailbox pointers. The lock is released before the
/// ack-wait loop (which calls `sched_yield`) so subscribers calling
/// `msg_router_ack()` — which also acquires the lock — cannot deadlock.
///
/// # Safety
/// Caller promises `topic_name` (≤ TOPIC_NAME_LEN + NUL) and `data`
/// (≤ MAX_MSG_LEN + NUL) are readable NUL-terminated C strings.
unsafe fn publish_internal(topic_name: *const u8, data: *const u8, priority: u8) -> i32 {
    const MAX_TARGETS: usize = MAX_SUBSCRIBERS + MAX_WILDCARD_SUBS;
    let mut targets: [*mut Mailbox; MAX_TARGETS] = [core::ptr::null_mut(); MAX_TARGETS];
    let mut target_count = 0usize;
    let mut found_topic = false;

    // Gather target mailbox pointers under the lock, then release it
    // before the yield-wait loop.
    {
        let _g = SpinGuard::new();
        // SAFETY: MSG_ROUTER_LOCK held — exclusive access to TOPICS /
        // WILDCARD_SUBS. Mailbox addresses taken here remain valid for
        // the rest of this call because the mailboxes live in static
        // arrays; they can only be torn down by `unsubscribe_all()`,
        // which also takes the lock (contention window is narrow and
        // acceptable for current workloads).
        for i in 0..MAX_TOPICS {
            if TOPICS[i].is_active() && str_eq_cstr(&TOPICS[i].name, topic_name) {
                found_topic = true;
                for j in 0..MAX_SUBSCRIBERS {
                    if TOPICS[i].subs[j].component_idx == -1 {
                        continue;
                    }
                    targets[target_count] = &mut TOPICS[i].subs[j].mailbox;
                    target_count += 1;
                }
                break;
            }
        }
        for i in 0..MAX_WILDCARD_SUBS {
            if !WILDCARD_SUBS[i].is_active() || WILDCARD_SUBS[i].component_idx == -1 {
                continue;
            }
            if !wildcard_matches(&WILDCARD_SUBS[i].pattern, topic_name) {
                continue;
            }
            if target_count < MAX_TARGETS {
                targets[target_count] = &mut WILDCARD_SUBS[i].mailbox;
                target_count += 1;
            }
        }
    } // MSG_ROUTER_LOCK released

    if !found_topic {
        uart_printf(b"[msg] Topic '%s' not found\n\0".as_ptr(), topic_name);
    }

    // Deliver and wait for ack on each target. Mailbox atomics handle
    // cross-CPU sync on the ready/ack flags.
    //
    // Timeout uses the ARM generic timer (CNTPCT_EL0) rather than the
    // tick-driven `pit_ticks`. On Pi 5, tasks run with DAIF.I=1 so timer
    // IRQs don't fire while `publish_internal` is executing, meaning
    // `pit_ticks` never advances and the loop would hang forever. The
    // hardware counter increments continuously regardless of IRQ mask
    // state.
    let mut delivered = 0i32;
    let timeout_cycles = timer_get_frequency().saturating_mul(ACK_TIMEOUT_SECS);
    for t in 0..target_count {
        // SAFETY: pointer points at a Mailbox inside the TOPICS /
        // WILDCARD_SUBS static arrays (stable storage).
        let mb = &mut *targets[t];
        mb.deliver(topic_name, data, priority);
        let start = timer_get_count();
        loop {
            if mb.ack.load(Ordering::Acquire) != 0 {
                delivered += 1;
                break;
            }
            if timer_get_count().wrapping_sub(start) >= timeout_cycles {
                break;
            }
            sched_yield();
        }
    }

    delivered
}

/// Publish a message to a topic with normal priority.
/// Delivers to all subscribers (exact and wildcard) and waits for ack.
/// Returns the number of subscribers that received the message.
#[no_mangle]
pub extern "C" fn msg_router_publish(topic_name: *const u8, data: *const u8) -> i32 {
    if topic_name.is_null() || data.is_null() {
        return 0;
    }
    unsafe { publish_internal(topic_name, data, MSG_PRIORITY_NORMAL) }
}

/// Publish a message with explicit priority (0 = normal, higher = more urgent).
/// Higher-priority messages are delivered first by msg_router_receive.
/// Returns the number of subscribers that received the message.
#[no_mangle]
pub extern "C" fn msg_router_publish_priority(
    topic_name: *const u8,
    data: *const u8,
    priority: u8,
) -> i32 {
    if topic_name.is_null() || data.is_null() {
        return 0;
    }
    unsafe { publish_internal(topic_name, data, priority) }
}

/// Check if a subscriber has a pending message.
/// Returns a pointer to the message data, or NULL if no message.
/// When multiple messages are pending, returns the highest-priority one.
/// Records which mailbox was returned so `msg_router_ack()` targets it exactly.
/// Caller must call `msg_router_ack()` after processing.
#[no_mangle]
pub extern "C" fn msg_router_receive(
    component_idx: i32,
    topic_out: *mut u8,
) -> *const u8 {
    let _g = SpinGuard::new();
    // SAFETY: MSG_ROUTER_LOCK held — exclusive access to TOPICS /
    // WILDCARD_SUBS / LAST_RECEIVED for the duration of this call.
    unsafe {
        let mut best_priority: u8 = 0;
        let mut best_data: *const u8 = core::ptr::null();
        let mut best_topic: *const [u8; TOPIC_NAME_LEN] = core::ptr::null();
        let mut best_src = LastReceived::none();

        // Scan exact topic subscriptions
        for i in 0..MAX_TOPICS {
            if !TOPICS[i].is_active() {
                continue;
            }
            for j in 0..MAX_SUBSCRIBERS {
                if TOPICS[i].subs[j].component_idx != component_idx {
                    continue;
                }
                let mb = &TOPICS[i].subs[j].mailbox;
                if mb.ready.load(Ordering::Acquire) != 0 {
                    let prio = mb.priority.load(Ordering::Acquire) as u8;
                    if best_data.is_null() || prio > best_priority {
                        best_priority = prio;
                        best_data = mb.data.as_ptr();
                        best_topic = &mb.topic;
                        best_src = LastReceived {
                            topic_idx: i as i32,
                            sub_idx: j as i32,
                            wildcard_idx: -1,
                        };
                    }
                }
            }
        }

        // Scan wildcard subscriptions
        for i in 0..MAX_WILDCARD_SUBS {
            if WILDCARD_SUBS[i].component_idx != component_idx {
                continue;
            }
            let mb = &WILDCARD_SUBS[i].mailbox;
            if mb.ready.load(Ordering::Acquire) != 0 {
                let prio = mb.priority.load(Ordering::Acquire) as u8;
                if best_data.is_null() || prio > best_priority {
                    best_priority = prio;
                    best_data = mb.data.as_ptr();
                    best_topic = &mb.topic;
                    best_src = LastReceived {
                        topic_idx: -1,
                        sub_idx: -1,
                        wildcard_idx: i as i32,
                    };
                }
            }
        }

        // Record which mailbox we returned so ack() targets it exactly
        if component_idx >= 0 && (component_idx as usize) < MAX_COMPONENTS {
            LAST_RECEIVED[component_idx as usize] = best_src;
        }

        if !best_data.is_null() && !topic_out.is_null() {
            let src = &*best_topic;
            let mut k = 0;
            while k < TOPIC_NAME_LEN - 1 && src[k] != 0 {
                *topic_out.add(k) = src[k];
                k += 1;
            }
            *topic_out.add(k) = 0;
        }

        best_data
    }
}

/// Acknowledge receipt of a message. Targets the exact mailbox that
/// was returned by the most recent `msg_router_receive()` call for this
/// component, preventing race conditions with newly-arrived messages.
#[no_mangle]
pub extern "C" fn msg_router_ack(component_idx: i32) {
    if component_idx < 0 || (component_idx as usize) >= MAX_COMPONENTS {
        return;
    }

    let _g = SpinGuard::new();
    // SAFETY: MSG_ROUTER_LOCK held — exclusive access to LAST_RECEIVED,
    // TOPICS, and WILDCARD_SUBS.
    unsafe {
        let lr = LAST_RECEIVED[component_idx as usize];

        if lr.wildcard_idx >= 0 && (lr.wildcard_idx as usize) < MAX_WILDCARD_SUBS {
            let mb = &mut WILDCARD_SUBS[lr.wildcard_idx as usize].mailbox;
            if mb.ready.load(Ordering::Acquire) != 0 {
                mb.ready.store(0, Ordering::Release);
                mb.ack.store(1, Ordering::Release);
            }
        } else if lr.topic_idx >= 0 && (lr.topic_idx as usize) < MAX_TOPICS
               && lr.sub_idx >= 0 && (lr.sub_idx as usize) < MAX_SUBSCRIBERS {
            let mb = &mut TOPICS[lr.topic_idx as usize].subs[lr.sub_idx as usize].mailbox;
            if mb.ready.load(Ordering::Acquire) != 0 {
                mb.ready.store(0, Ordering::Release);
                mb.ack.store(1, Ordering::Release);
            }
        }

        // Clear the record
        LAST_RECEIVED[component_idx as usize] = LastReceived::none();
    }
}

/// Remove all subscriptions for a component. Reclaims topics with no remaining
/// subscribers. Called during component unload to prevent orphaned subscriptions.
#[no_mangle]
pub extern "C" fn msg_router_unsubscribe_all(component_idx: i32) {
    let _g = SpinGuard::new();
    // SAFETY: MSG_ROUTER_LOCK held — exclusive access to TOPICS /
    // WILDCARD_SUBS / TOPIC_COUNT.
    unsafe {
        for i in 0..MAX_TOPICS {
            if !TOPICS[i].is_active() {
                continue;
            }
            for j in 0..MAX_SUBSCRIBERS {
                if TOPICS[i].subs[j].component_idx == component_idx {
                    TOPICS[i].subs[j].clear();
                    TOPICS[i].sub_count -= 1;
                }
            }
            // Reclaim topic if no subscribers remain
            if TOPICS[i].sub_count <= 0 {
                TOPICS[i].clear();
                TOPIC_COUNT -= 1;
            }
        }
        // Also clear wildcard subscriptions
        for i in 0..MAX_WILDCARD_SUBS {
            if WILDCARD_SUBS[i].component_idx == component_idx {
                WILDCARD_SUBS[i].clear();
            }
        }
    }
}

/// Get the list of topics a component is subscribed to.
/// Writes topic names into `topic_names` (array of TOPIC_NAME_LEN buffers)
/// and sets `count_out` to the number found. At most `max_topics` entries.
#[no_mangle]
pub extern "C" fn msg_router_get_subscriptions(
    component_idx: i32,
    topic_names: *mut [u8; TOPIC_NAME_LEN],
    count_out: *mut i32,
    max_topics: i32,
) {
    if topic_names.is_null() || count_out.is_null() {
        return;
    }
    let _g = SpinGuard::new();
    // SAFETY: MSG_ROUTER_LOCK held — exclusive read access to TOPICS.
    unsafe {
        let mut count = 0i32;
        for i in 0..MAX_TOPICS {
            if !TOPICS[i].is_active() {
                continue;
            }
            for j in 0..MAX_SUBSCRIBERS {
                if TOPICS[i].subs[j].component_idx == component_idx {
                    if count < max_topics {
                        let dst = &mut *topic_names.add(count as usize);
                        *dst = TOPICS[i].name;
                    }
                    count += 1;
                    break; // Only count each topic once per component
                }
            }
        }
        *count_out = count;
    }
}

/// List all topics and their subscribers.
#[no_mangle]
pub extern "C" fn msg_router_list() {
    let _g = SpinGuard::new();
    // SAFETY: MSG_ROUTER_LOCK held — exclusive read access to TOPICS /
    // TOPIC_COUNT. The component_get_info() FFI call takes its own lock
    // (component::registry::LOCK) and does not touch msg_router state,
    // so no lock inversion.
    unsafe {
        uart_printf(
            b"Message Router (%d topics):\n\0".as_ptr(),
            TOPIC_COUNT,
        );

        if TOPIC_COUNT == 0 {
            puts(b"  (no topics)\n\0");
            return;
        }

        for i in 0..MAX_TOPICS {
            if !TOPICS[i].is_active() {
                continue;
            }
            uart_printf(
                b"  Topic '%s' (%d subscribers):\n\0".as_ptr(),
                TOPICS[i].name.as_ptr(),
                TOPICS[i].sub_count,
            );

            for j in 0..MAX_SUBSCRIBERS {
                if TOPICS[i].subs[j].component_idx == -1 {
                    continue;
                }

                // Try to get component name via FFI
                let mut info = core::mem::zeroed::<crate::component::ComponentInfo>();
                let idx = TOPICS[i].subs[j].component_idx;
                let ret = crate::component::component_get_info(idx as u32, &mut info);
                if ret == 0 {
                    uart_printf(
                        b"    \xe2\x86\x92 component '%s' (idx %d)\n\0".as_ptr(),
                        info.name.as_ptr(),
                        idx,
                    );
                } else {
                    uart_printf(
                        b"    \xe2\x86\x92 component idx %d\n\0".as_ptr(),
                        idx,
                    );
                }
            }
        }
    }
}
