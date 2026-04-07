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

use core::sync::atomic::{AtomicU32, Ordering};

// =============================================================================
// Constants
// =============================================================================

const MAX_TOPICS: usize = 8;
const MAX_SUBSCRIBERS: usize = 4;
const MAX_MSG_LEN: usize = 60;
const TOPIC_NAME_LEN: usize = 16;

/// Ack timeout in pit_ticks (500 ticks = 5 seconds at 100 Hz).
const ACK_TIMEOUT_TICKS: u64 = 500;

// =============================================================================
// External C functions
// =============================================================================

extern "C" {
    fn uart_puts(s: *const u8);
    fn uart_printf(fmt: *const u8, ...);
    #[link_name = "yield"]
    fn sched_yield();
    static pit_ticks: u64;
}

/// Read `pit_ticks` using volatile access (it's modified by ISR).
fn get_ticks() -> u64 {
    unsafe { core::ptr::read_volatile(&pit_ticks) }
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
}

impl Mailbox {
    const fn new() -> Self {
        Self {
            ready: AtomicU32::new(0),
            ack: AtomicU32::new(0),
            data: [0; MAX_MSG_LEN],
            topic: [0; TOPIC_NAME_LEN],
        }
    }

    fn clear(&mut self) {
        self.ready.store(0, Ordering::Release);
        self.ack.store(0, Ordering::Release);
        self.data = [0; MAX_MSG_LEN];
        self.topic = [0; TOPIC_NAME_LEN];
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

// =============================================================================
// String Helpers
// =============================================================================

/// Compare a C string (null-terminated) with a byte slice in our topic name buffer.
fn str_eq_cstr(buf: &[u8], cstr: *const u8) -> bool {
    unsafe {
        let mut i = 0;
        loop {
            let a = if i < buf.len() { buf[i] } else { 0 };
            let b = *cstr.add(i);
            if a == 0 && b == 0 {
                return true;
            }
            if a != b {
                return false;
            }
            i += 1;
        }
    }
}

/// Copy a C string into a fixed-size buffer.
fn str_copy(dst: &mut [u8], src: *const u8) {
    let max = dst.len();
    let mut i = 0;
    unsafe {
        while i < max - 1 {
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

// =============================================================================
// Public FFI API
// =============================================================================

/// Initialize the message router. Clears all topics and subscriptions.
#[no_mangle]
pub extern "C" fn msg_router_init() {
    unsafe {
        TOPIC_COUNT = 0;
        for topic in &mut TOPICS {
            topic.clear();
        }
    }
}

/// Subscribe a component to a topic. Creates the topic if it doesn't exist.
/// Returns 0 on success, -1 on error.
#[no_mangle]
pub extern "C" fn msg_router_subscribe(topic_name: *const u8, component_idx: i32) -> i32 {
    if topic_name.is_null() {
        return -1;
    }

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
                    str_copy(&mut TOPICS[i].name, topic_name);
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

/// Publish a message to a topic. Delivers to all subscribers and waits
/// for acknowledgment (yield-based, 5-second timeout).
/// Returns the number of subscribers that received the message.
#[no_mangle]
pub extern "C" fn msg_router_publish(topic_name: *const u8, data: *const u8) -> i32 {
    if topic_name.is_null() || data.is_null() {
        return 0;
    }

    unsafe {
        // Find topic
        let mut topic_idx: Option<usize> = None;
        for i in 0..MAX_TOPICS {
            if TOPICS[i].is_active() && str_eq_cstr(&TOPICS[i].name, topic_name) {
                topic_idx = Some(i);
                break;
            }
        }

        let idx = match topic_idx {
            Some(i) => i,
            None => {
                uart_printf(b"[msg] Topic '%s' not found\n\0".as_ptr(), topic_name);
                return 0;
            }
        };

        let mut delivered = 0i32;

        for j in 0..MAX_SUBSCRIBERS {
            if TOPICS[idx].subs[j].component_idx == -1 {
                continue;
            }

            let mb = &mut TOPICS[idx].subs[j].mailbox;

            // Copy message into subscriber's mailbox
            str_copy(&mut mb.data, data);
            str_copy(&mut mb.topic, topic_name);
            mb.ack.store(0, Ordering::Release);
            mb.ready.store(1, Ordering::Release);

            // Wait for ack (5-second timeout)
            let timeout = get_ticks() + ACK_TIMEOUT_TICKS;
            while get_ticks() < timeout {
                if mb.ack.load(Ordering::Acquire) != 0 {
                    delivered += 1;
                    break;
                }
                sched_yield();
            }
        }

        delivered
    }
}

/// Check if a subscriber has a pending message.
/// Returns a pointer to the message data, or NULL if no message.
/// Caller must call `msg_router_ack()` after processing.
#[no_mangle]
pub extern "C" fn msg_router_receive(
    component_idx: i32,
    topic_out: *mut u8,
) -> *const u8 {
    unsafe {
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
                    if !topic_out.is_null() {
                        // Copy topic name to caller's buffer
                        let src = &mb.topic;
                        let mut k = 0;
                        while k < TOPIC_NAME_LEN - 1 && src[k] != 0 {
                            *topic_out.add(k) = src[k];
                            k += 1;
                        }
                        *topic_out.add(k) = 0;
                    }
                    return mb.data.as_ptr();
                }
            }
        }
        core::ptr::null()
    }
}

/// Acknowledge receipt of a message. Clears the ready flag and sets ack.
#[no_mangle]
pub extern "C" fn msg_router_ack(component_idx: i32) {
    unsafe {
        for i in 0..MAX_TOPICS {
            if !TOPICS[i].is_active() {
                continue;
            }
            for j in 0..MAX_SUBSCRIBERS {
                if TOPICS[i].subs[j].component_idx != component_idx {
                    continue;
                }
                let mb = &mut TOPICS[i].subs[j].mailbox;
                if mb.ready.load(Ordering::Acquire) != 0 {
                    mb.ready.store(0, Ordering::Release);
                    mb.ack.store(1, Ordering::Release);
                    return;
                }
            }
        }
    }
}

/// List all topics and their subscribers.
#[no_mangle]
pub extern "C" fn msg_router_list() {
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
