//! Live per-session telemetry for `slm stats` (#475,
//! `docs/design/slm-integration.md` §Telemetry).
//!
//! A prompt takes its session out of the slot table, so the session's own
//! counters can't be read while it runs. The numbers `slm stats` shows live
//! here instead, in a block the session shares with its slot:
//!
//! - One writer, the task running the prompt. It updates at most once per
//!   forward step and never waits for a reader.
//! - Any number of readers, from any task. A read is a bounded retry over a
//!   sequence counter, so a reader can come away with a mixed snapshot (it says
//!   so) but can't hold the decode up. No lock is shared with the decode.
//! - No heap after [`Telemetry::new`]: the writer's path is atomics and fixed
//!   arrays. `tests::per_token_path_does_not_allocate` holds this.
//!
//! Figures that aren't what they claim don't belong here. Tensor-core
//! utilization has two derived measures, each named for what it is, and no
//! counter: see `docs/design/slm-integration.md` §Tensor-core utilization.

use core::sync::atomic::{fence, AtomicBool, AtomicU32, AtomicU64, Ordering::{Acquire, Relaxed, Release}};

use crate::inference::gpu_slm::{select_tier, OpKind, Tier};

/// Op classes the breakdown uses. Values are FFI (`SLM_OPCLASS_*`).
#[repr(u32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum OpClass {
    Embed = 0,
    RmsNorm = 1,
    /// Q, K, V and output projections.
    MatmulAttn = 2,
    Rope = 3,
    Gqa = 4,
    /// Gate, up and down projections. On the CPU path also SwiGLU, which
    /// `swiglu_mlp_q` runs inside the same call.
    MatmulMlp = 5,
    SwiGlu = 6,
    Add = 7,
    LmHead = 8,
}

pub const OP_CLASSES: usize = 9;

/// Inter-token samples kept (spec: ring of 256).
pub const ITL_RING: usize = 256;
/// Tokens the rolling figures cover.
pub const ROLLING: usize = 32;

/// `SlmTelemetryC` value for a figure with no real source right now.
pub const NA: u32 = u32::MAX;

/// Saturation rate of `mma` across all SMs, from C3K's `slm_forward` harness
/// (`cpi_mma`: every warp issuing mma and nothing else) under JetPack, native
/// CUDA 12.6, MAXN_SUPER, at a fixed 1020 MHz GPC clock. Measured outside
/// SLM-OS, and scaled to the running clock by [`mma_issue_bp`].
pub const MMA_RATE_PER_S: u64 = 3_440_000_000;
pub const MMA_RATE_GPC_HZ: u64 = 1_020_000_000;

/// What a session's program runs per decode token, from its plan.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct PlanFacts {
    pub launches: u32,
    pub barriers: u32,
    pub marks: u32,
    pub class_launches: [u32; OP_CLASSES],
    /// `Tier` as u32 per class.
    pub class_tier: [u32; OP_CLASSES],
    /// Quantized weight bytes the matmuls read. Bytes the kernels name, not a
    /// DRAM counter: the L2 absorbs some.
    pub weight_bytes: u64,
    /// `mma` instructions per token, 0 on SIMT matmuls.
    pub mma: u64,
    /// Matmul MACs on tensor cores and in SIMT.
    pub macs_tc: u64,
    pub macs_simt: u64,
    /// GQA's Q.K and p.V MACs per KV position, all SIMT.
    pub gqa_macs_per_pos: u64,
    /// K and V bytes GQA reads per KV position.
    pub kv_bytes_per_pos: u64,
    pub arena_bytes: u64,
}

/// One class in [`SlmTelemetryC`]. Keep in step with slm_ffi.h.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct SlmOpClassC {
    /// GPU or CPU time in the last profiled token.
    pub last_ns: u64,
    /// The same, summed over this prompt's profiled tokens.
    pub total_ns: u64,
    /// Ops per token.
    pub ops: u32,
    /// `Tier` as u32: 1 HMMA, 2 SIMT, 3 CPU.
    pub tier: u32,
}

/// Snapshot for `slm stats`. Keep in step with `SlmTelemetryC` in slm_ffi.h.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct SlmTelemetryC {
    /// A prompt is running; the figures are live.
    pub running: u32,
    /// 1 when no write overlapped the read.
    pub consistent: u32,
    /// `--profile` is armed.
    pub profile: u32,
    /// `SLM_PATH_*`.
    pub path: u32,
    pub stop_reason: u32,
    pub kv_used: u32,
    pub max_ctx: u32,
    pub prompts: u32,
    pub tokens_in: u64,
    pub tokens_out: u64,
    /// Input pieces the tokenizer's vocab lacks, over the session.
    pub oov_pieces: u64,

    // This prompt, or the last one if none is running.
    pub ttft_ns: u64,
    pub prefill_tokens: u64,
    pub prefill_ns: u64,
    pub decode_tokens: u64,
    pub decode_ns: u64,
    pub itl_count: u64,
    pub itl_min_ns: u64,
    pub itl_avg_ns: u64,
    pub itl_max_ns: u64,
    pub itl_last_ns: u64,
    /// Mean of the last [`ROLLING`] inter-token gaps.
    pub itl_rolling_ns: u64,

    /// Weights + KV + GPU arena, the most seen at a prompt's start.
    pub peak_resident_bytes: u64,

    // GPU, this prompt.
    pub gpu_steps: u64,
    pub gpu_busy_ns: u64,
    pub gpu_last_ns: u64,
    pub gpu_wait_us: u64,
    pub gpu_evict_us: u64,

    // The session's plan.
    pub launches: u32,
    pub barriers: u32,
    pub marks: u32,
    pub ops_tier1: u32,
    pub ops_tier2: u32,
    pub ops_cpu: u32,
    pub weight_bytes: u64,
    /// GQA's KV reads at the current position.
    pub kv_read_bytes: u64,
    pub mma: u64,
    pub arena_bytes: u64,

    /// Decode tokens with a per-class breakdown.
    pub profiled_tokens: u64,
    /// Matmul time per profiled token.
    pub matmul_avg_ns: u64,
    /// mma issue time over matmul wall time, in hundredths of a percent, or [`NA`].
    pub tc_issue_bp: u32,
    /// Tensor-core share of matmul + GQA MACs, in hundredths of a percent, or [`NA`].
    pub tc_mac_share_bp: u32,

    pub classes: [SlmOpClassC; OP_CLASSES],
}

const _: () = assert!(core::mem::size_of::<SlmTelemetryC>() == 488);

/// One session's live figures.
pub struct Telemetry {
    seq: AtomicU32,
    profile: AtomicBool,
    running: AtomicBool,
    path: AtomicU32,
    stop_reason: AtomicU32,
    kv_used: AtomicU32,
    max_ctx: AtomicU32,
    prompts: AtomicU32,
    tokens_in: AtomicU64,
    tokens_out: AtomicU64,
    oov_pieces: AtomicU64,
    ttft_ns: AtomicU64,
    prefill_tokens: AtomicU64,
    prefill_ns: AtomicU64,
    decode_tokens: AtomicU64,
    decode_ns: AtomicU64,
    itl_count: AtomicU64,
    itl_min_ns: AtomicU64,
    itl_max_ns: AtomicU64,
    itl_sum_ns: AtomicU64,
    itl_ring: [AtomicU64; ITL_RING],
    peak_resident_bytes: AtomicU64,
    gpu_steps: AtomicU64,
    gpu_busy_ns: AtomicU64,
    gpu_last_ns: AtomicU64,
    gpu_wait_us: AtomicU64,
    gpu_evict_us: AtomicU64,
    profiled_tokens: AtomicU64,
    class_last_ns: [AtomicU64; OP_CLASSES],
    class_total_ns: [AtomicU64; OP_CLASSES],
    // The plan. Written only while no prompt runs.
    launches: AtomicU32,
    barriers: AtomicU32,
    marks: AtomicU32,
    class_ops: [AtomicU32; OP_CLASSES],
    class_tier: [AtomicU32; OP_CLASSES],
    weight_bytes: AtomicU64,
    mma: AtomicU64,
    macs_tc: AtomicU64,
    macs_simt: AtomicU64,
    gqa_macs_per_pos: AtomicU64,
    kv_bytes_per_pos: AtomicU64,
    arena_bytes: AtomicU64,
}

/// Times a forward step's classes on the CPU path. When off, a lap is one
/// branch and no clock read.
pub struct LapTimer {
    on: bool,
    t: u64,
    ns: [u64; OP_CLASSES],
}

impl LapTimer {
    pub fn new(on: bool) -> Self {
        let t = if on { crate::kernel_ffi::get_time_ns() } else { 0 };
        Self { on, t, ns: [0; OP_CLASSES] }
    }

    /// Charge the time since the last lap to `class`.
    #[inline]
    pub fn lap(&mut self, class: OpClass) {
        if self.on {
            let now = crate::kernel_ffi::get_time_ns();
            self.ns[class as usize] += now.wrapping_sub(self.t);
            self.t = now;
        }
    }

    pub fn finish(&self) -> Option<&[u64; OP_CLASSES]> {
        self.on.then_some(&self.ns)
    }
}

impl Telemetry {
    /// `layers` sizes the op table of a session with no program.
    pub fn new(layers: u32, max_ctx: u32) -> Self {
        let t = Self {
            seq: AtomicU32::new(0),
            profile: AtomicBool::new(false),
            running: AtomicBool::new(false),
            path: AtomicU32::new(0),
            stop_reason: AtomicU32::new(0),
            kv_used: AtomicU32::new(0),
            max_ctx: AtomicU32::new(max_ctx),
            prompts: AtomicU32::new(0),
            tokens_in: AtomicU64::new(0),
            tokens_out: AtomicU64::new(0),
            oov_pieces: AtomicU64::new(0),
            ttft_ns: AtomicU64::new(0),
            prefill_tokens: AtomicU64::new(0),
            prefill_ns: AtomicU64::new(0),
            decode_tokens: AtomicU64::new(0),
            decode_ns: AtomicU64::new(0),
            itl_count: AtomicU64::new(0),
            itl_min_ns: AtomicU64::new(0),
            itl_max_ns: AtomicU64::new(0),
            itl_sum_ns: AtomicU64::new(0),
            itl_ring: [const { AtomicU64::new(0) }; ITL_RING],
            peak_resident_bytes: AtomicU64::new(0),
            gpu_steps: AtomicU64::new(0),
            gpu_busy_ns: AtomicU64::new(0),
            gpu_last_ns: AtomicU64::new(0),
            gpu_wait_us: AtomicU64::new(0),
            gpu_evict_us: AtomicU64::new(0),
            profiled_tokens: AtomicU64::new(0),
            class_last_ns: [const { AtomicU64::new(0) }; OP_CLASSES],
            class_total_ns: [const { AtomicU64::new(0) }; OP_CLASSES],
            launches: AtomicU32::new(0),
            barriers: AtomicU32::new(0),
            marks: AtomicU32::new(0),
            class_ops: [const { AtomicU32::new(0) }; OP_CLASSES],
            class_tier: [const { AtomicU32::new(Tier::Cpu as u32) }; OP_CLASSES],
            weight_bytes: AtomicU64::new(0),
            mma: AtomicU64::new(0),
            macs_tc: AtomicU64::new(0),
            macs_simt: AtomicU64::new(0),
            gqa_macs_per_pos: AtomicU64::new(0),
            kv_bytes_per_pos: AtomicU64::new(0),
            arena_bytes: AtomicU64::new(0),
        };
        t.set_cpu_ops(layers);
        t
    }

    /// What `forward_one` runs per token. Same ops as a program's plan.
    pub fn set_cpu_ops(&self, layers: u32) {
        let l = layers;
        let ops = [1, 2 * l + 1, 4 * l, 2 * l, l, 3 * l, l, 2 * l, 1];
        for (slot, n) in self.class_ops.iter().zip(ops) {
            slot.store(n, Relaxed);
        }
        for slot in &self.class_tier {
            slot.store(Tier::Cpu as u32, Relaxed);
        }
    }

    /// Record the session's program. `None` drops it.
    pub fn set_plan(&self, facts: Option<&PlanFacts>, layers: u32) {
        let zero = PlanFacts::default();
        let f = facts.unwrap_or(&zero);
        self.launches.store(f.launches, Relaxed);
        self.barriers.store(f.barriers, Relaxed);
        self.marks.store(f.marks, Relaxed);
        self.weight_bytes.store(f.weight_bytes, Relaxed);
        self.mma.store(f.mma, Relaxed);
        self.macs_tc.store(f.macs_tc, Relaxed);
        self.macs_simt.store(f.macs_simt, Relaxed);
        self.gqa_macs_per_pos.store(f.gqa_macs_per_pos, Relaxed);
        self.kv_bytes_per_pos.store(f.kv_bytes_per_pos, Relaxed);
        self.arena_bytes.store(f.arena_bytes, Relaxed);
        if facts.is_some() {
            for i in 0..OP_CLASSES {
                self.class_ops[i].store(f.class_launches[i], Relaxed);
                self.class_tier[i].store(f.class_tier[i], Relaxed);
            }
        } else {
            self.set_cpu_ops(layers);
        }
    }

    pub fn set_path(&self, path: u32) {
        self.path.store(path, Relaxed);
    }

    /// `slm stats --profile`: from the next step on.
    pub fn set_profile(&self, on: bool) {
        self.profile.store(on, Relaxed);
    }

    /// Read at most once per forward step.
    #[inline]
    pub fn profile_on(&self) -> bool {
        self.profile.load(Relaxed)
    }

    /// Everything but the plan, the profile switch and the path: `slm reset`.
    pub fn reset(&self) {
        self.write(|| {
            for c in [&self.tokens_in, &self.tokens_out, &self.oov_pieces] {
                c.store(0, Relaxed);
            }
            self.prompts.store(0, Relaxed);
            self.stop_reason.store(0, Relaxed);
            self.kv_used.store(0, Relaxed);
            self.clear_prompt();
        });
    }

    fn clear_prompt(&self) {
        for c in [
            &self.ttft_ns,
            &self.prefill_tokens,
            &self.prefill_ns,
            &self.decode_tokens,
            &self.decode_ns,
            &self.itl_count,
            &self.itl_min_ns,
            &self.itl_max_ns,
            &self.itl_sum_ns,
            &self.gpu_steps,
            &self.gpu_busy_ns,
            &self.gpu_last_ns,
            &self.gpu_wait_us,
            &self.gpu_evict_us,
            &self.profiled_tokens,
        ] {
            c.store(0, Relaxed);
        }
        for i in 0..OP_CLASSES {
            self.class_last_ns[i].store(0, Relaxed);
            self.class_total_ns[i].store(0, Relaxed);
        }
    }

    #[inline]
    fn write(&self, f: impl FnOnce()) {
        let s = self.seq.load(Relaxed).wrapping_add(1);
        self.seq.store(s, Relaxed);
        fence(Release);
        f();
        self.seq.store(s.wrapping_add(1), Release);
    }

    fn add(c: &AtomicU64, n: u64) {
        c.store(c.load(Relaxed).wrapping_add(n), Relaxed);
    }

    // ---- The writer: one task, the one running the prompt. ----

    pub fn prompt_begin(&self, path: u32, kv_used: usize, resident_bytes: u64) {
        self.write(|| {
            self.clear_prompt();
            self.path.store(path, Relaxed);
            self.kv_used.store(kv_used as u32, Relaxed);
            self.stop_reason.store(0, Relaxed);
            if resident_bytes > self.peak_resident_bytes.load(Relaxed) {
                self.peak_resident_bytes.store(resident_bytes, Relaxed);
            }
            self.running.store(true, Relaxed);
        });
    }

    pub fn note_oov(&self, pieces: u64) {
        if pieces != 0 {
            self.write(|| Self::add(&self.oov_pieces, pieces));
        }
    }

    /// `tokens` more prompt tokens are in the KV cache, `ns` after prefill began.
    #[inline]
    pub fn prefill_progress(&self, tokens: u64, ns: u64, kv_used: usize) {
        self.write(|| {
            Self::add(&self.prefill_tokens, tokens);
            Self::add(&self.tokens_in, tokens);
            self.prefill_ns.store(ns, Relaxed);
            self.kv_used.store(kv_used as u32, Relaxed);
        });
    }

    pub fn first_token(&self, ttft_ns: u64) {
        self.write(|| self.ttft_ns.store(ttft_ns, Relaxed));
    }

    /// One decode step is done: `gap_ns` since the token before it, `decode_ns`
    /// since decoding began.
    #[inline]
    pub fn decode_step(&self, gap_ns: u64, decode_ns: u64, kv_used: usize) {
        self.write(|| {
            let n = self.itl_count.load(Relaxed);
            self.itl_ring[n as usize % ITL_RING].store(gap_ns, Relaxed);
            if n == 0 || gap_ns < self.itl_min_ns.load(Relaxed) {
                self.itl_min_ns.store(gap_ns, Relaxed);
            }
            if gap_ns > self.itl_max_ns.load(Relaxed) {
                self.itl_max_ns.store(gap_ns, Relaxed);
            }
            Self::add(&self.itl_sum_ns, gap_ns);
            self.itl_count.store(n + 1, Relaxed);
            self.decode_ns.store(decode_ns, Relaxed);
            self.kv_used.store(kv_used as u32, Relaxed);
        });
    }

    /// A token left for the callback.
    #[inline]
    pub fn token_out(&self) {
        self.write(|| {
            Self::add(&self.decode_tokens, 1);
            Self::add(&self.tokens_out, 1);
        });
    }

    #[inline]
    pub fn gpu_step(&self, gpu_ns: u64, wait_us: u64, evict_us: u64) {
        self.write(|| {
            Self::add(&self.gpu_steps, 1);
            Self::add(&self.gpu_busy_ns, gpu_ns);
            self.gpu_last_ns.store(gpu_ns, Relaxed);
            Self::add(&self.gpu_wait_us, wait_us);
            Self::add(&self.gpu_evict_us, evict_us);
        });
    }

    /// A token's time per class, from marks or a [`LapTimer`].
    #[inline]
    pub fn classes(&self, ns: &[u64; OP_CLASSES]) {
        self.write(|| {
            for i in 0..OP_CLASSES {
                self.class_last_ns[i].store(ns[i], Relaxed);
                Self::add(&self.class_total_ns[i], ns[i]);
            }
            Self::add(&self.profiled_tokens, 1);
        });
    }

    pub fn prompt_end(&self, stop_reason: u32, kv_used: usize) {
        self.write(|| {
            self.stop_reason.store(stop_reason, Relaxed);
            self.kv_used.store(kv_used as u32, Relaxed);
            self.prompts.store(self.prompts.load(Relaxed).saturating_add(1), Relaxed);
            self.running.store(false, Relaxed);
        });
    }

    // ---- Readers: any task. ----

    /// Copy the figures out. Never waits for the writer beyond a few retries;
    /// `consistent` is 0 if every try overlapped a write. `gpc_hz` scales the
    /// mma rate (0: unknown, and that measure is [`NA`]).
    pub fn snapshot(&self, gpc_hz: u64) -> SlmTelemetryC {
        let mut out = SlmTelemetryC::default();
        for _ in 0..8 {
            let s0 = self.seq.load(Acquire);
            if s0 & 1 == 0 {
                self.read_into(&mut out, gpc_hz);
                fence(Acquire);
                if self.seq.load(Relaxed) == s0 {
                    out.consistent = 1;
                    return out;
                }
            }
            core::hint::spin_loop();
        }
        self.read_into(&mut out, gpc_hz);
        out.consistent = 0;
        out
    }

    fn read_into(&self, o: &mut SlmTelemetryC, gpc_hz: u64) {
        o.running = self.running.load(Relaxed) as u32;
        o.profile = self.profile.load(Relaxed) as u32;
        o.path = self.path.load(Relaxed);
        o.stop_reason = self.stop_reason.load(Relaxed);
        o.kv_used = self.kv_used.load(Relaxed);
        o.max_ctx = self.max_ctx.load(Relaxed);
        o.prompts = self.prompts.load(Relaxed);
        o.tokens_in = self.tokens_in.load(Relaxed);
        o.tokens_out = self.tokens_out.load(Relaxed);
        o.oov_pieces = self.oov_pieces.load(Relaxed);
        o.ttft_ns = self.ttft_ns.load(Relaxed);
        o.prefill_tokens = self.prefill_tokens.load(Relaxed);
        o.prefill_ns = self.prefill_ns.load(Relaxed);
        o.decode_tokens = self.decode_tokens.load(Relaxed);
        o.decode_ns = self.decode_ns.load(Relaxed);

        let n = self.itl_count.load(Relaxed);
        o.itl_count = n;
        o.itl_min_ns = self.itl_min_ns.load(Relaxed);
        o.itl_max_ns = self.itl_max_ns.load(Relaxed);
        o.itl_avg_ns = self.itl_sum_ns.load(Relaxed).checked_div(n).unwrap_or(0);
        o.itl_last_ns = 0;
        o.itl_rolling_ns = 0;
        if n != 0 {
            let idx = |back: u64| ((n - 1 - back) % ITL_RING as u64) as usize;
            o.itl_last_ns = self.itl_ring[idx(0)].load(Relaxed);
            let take = n.min(ROLLING as u64);
            let sum = (0..take).fold(0u64, |a, b| a.wrapping_add(self.itl_ring[idx(b)].load(Relaxed)));
            o.itl_rolling_ns = sum / take;
        }

        o.peak_resident_bytes = self.peak_resident_bytes.load(Relaxed);
        o.gpu_steps = self.gpu_steps.load(Relaxed);
        o.gpu_busy_ns = self.gpu_busy_ns.load(Relaxed);
        o.gpu_last_ns = self.gpu_last_ns.load(Relaxed);
        o.gpu_wait_us = self.gpu_wait_us.load(Relaxed);
        o.gpu_evict_us = self.gpu_evict_us.load(Relaxed);
        o.launches = self.launches.load(Relaxed);
        o.barriers = self.barriers.load(Relaxed);
        o.marks = self.marks.load(Relaxed);
        o.weight_bytes = self.weight_bytes.load(Relaxed);
        o.mma = self.mma.load(Relaxed);
        o.arena_bytes = self.arena_bytes.load(Relaxed);
        o.profiled_tokens = self.profiled_tokens.load(Relaxed);
        o.kv_read_bytes = self.kv_bytes_per_pos.load(Relaxed).saturating_mul(o.kv_used as u64);

        let per_op = o.path == crate::slm::session::PATH_PER_OP;
        o.ops_tier1 = 0;
        o.ops_tier2 = 0;
        o.ops_cpu = 0;
        for i in 0..OP_CLASSES {
            let tier = if per_op { per_op_tier(i) } else { self.class_tier[i].load(Relaxed) };
            let ops = self.class_ops[i].load(Relaxed);
            o.classes[i] = SlmOpClassC {
                last_ns: self.class_last_ns[i].load(Relaxed),
                total_ns: self.class_total_ns[i].load(Relaxed),
                ops,
                tier,
            };
            if tier == Tier::Hmma as u32 {
                o.ops_tier1 += ops;
            } else if tier == Tier::Simt as u32 {
                o.ops_tier2 += ops;
            } else {
                o.ops_cpu += ops;
            }
        }

        let mm_total = o.classes[OpClass::MatmulAttn as usize].total_ns
            + o.classes[OpClass::MatmulMlp as usize].total_ns
            + o.classes[OpClass::LmHead as usize].total_ns;
        o.matmul_avg_ns = mm_total.checked_div(o.profiled_tokens).unwrap_or(0);
        o.tc_issue_bp = mma_issue_bp(o.mma, o.matmul_avg_ns, gpc_hz);
        o.tc_mac_share_bp = mac_share_bp(
            self.macs_tc.load(Relaxed),
            self.macs_simt.load(Relaxed),
            self.gqa_macs_per_pos.load(Relaxed).saturating_mul(o.kv_used as u64),
        );
    }
}

/// The tier table's choice for a class on the per-op path.
fn per_op_tier(class: usize) -> u32 {
    let kind = match class {
        c if c == OpClass::Embed as usize => OpKind::Embedding,
        c if c == OpClass::RmsNorm as usize => OpKind::RmsNorm,
        c if c == OpClass::MatmulAttn as usize || c == OpClass::MatmulMlp as usize => OpKind::Q4kDot,
        c if c == OpClass::Rope as usize => OpKind::Rope,
        c if c == OpClass::Gqa as usize => OpKind::GqaAttn,
        c if c == OpClass::SwiGlu as usize => OpKind::SwiGlu,
        c if c == OpClass::LmHead as usize => OpKind::LmHead,
        _ => return Tier::Cpu as u32,
    };
    select_tier(kind) as u32
}

/// Time the token's mma instructions take at the measured saturation rate, over
/// the matmuls' wall time, in hundredths of a percent. This is an issue-slot
/// share, not a counter: an SM doing other work between mma is not "idle".
pub fn mma_issue_bp(mma: u64, matmul_ns: u64, gpc_hz: u64) -> u32 {
    if mma == 0 || matmul_ns == 0 || gpc_hz == 0 {
        return NA;
    }
    let rate = MMA_RATE_PER_S as u128 * gpc_hz as u128 / MMA_RATE_GPC_HZ as u128;
    let issue_ns = mma as u128 * 1_000_000_000 / rate.max(1);
    (issue_ns * 10_000 / matmul_ns as u128).min(u32::MAX as u128 - 1) as u32
}

/// Tensor-core share of matmul and GQA MACs, in hundredths of a percent.
pub fn mac_share_bp(macs_tc: u64, macs_simt: u64, macs_gqa: u64) -> u32 {
    if macs_tc == 0 {
        return NA;
    }
    let total = macs_tc as u128 + macs_simt as u128 + macs_gqa as u128;
    (macs_tc as u128 * 10_000 / total) as u32
}

/// Mean nanoseconds the per-token writes take, for the cost check in `make test`.
pub fn token_cost_ns() -> u64 {
    const N: u64 = 2048;
    let t = alloc::boxed::Box::new(Telemetry::new(1, 16));
    t.prompt_begin(0, 0, 0);
    let start = crate::kernel_ffi::get_time_ns();
    for i in 0..N {
        t.token_out();
        t.gpu_step(1000, 2, 3);
        t.decode_step(i, i, 1);
    }
    crate::kernel_ffi::get_time_ns().wrapping_sub(start) / N
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn fresh_block_reads_as_zeros() {
        let t = Telemetry::new(2, 64);
        let s = t.snapshot(0);
        assert_eq!(s.consistent, 1);
        assert_eq!(s.running, 0);
        assert_eq!(s.prompts, 0);
        assert_eq!(s.max_ctx, 64);
        assert_eq!(s.itl_avg_ns, 0);
        assert_eq!(s.tc_issue_bp, NA);
        assert_eq!(s.tc_mac_share_bp, NA);
        // 15 per layer + embed, final norm, LM head.
        assert_eq!(s.ops_cpu, 33);
        assert_eq!(s.ops_tier1 + s.ops_tier2, 0);
    }

    #[test]
    fn inter_token_figures_cover_the_prompt_and_the_last_32() {
        let t = Telemetry::new(1, 1024);
        t.prompt_begin(2, 0, 100);
        // More than the ring holds: min and max still cover the whole prompt.
        for i in 0..300u64 {
            t.decode_step(if i == 0 { 5 } else if i == 1 { 9000 } else { 100 + i }, i, 1);
        }
        let s = t.snapshot(0);
        assert_eq!(s.running, 1);
        assert_eq!(s.itl_count, 300);
        assert_eq!(s.itl_min_ns, 5);
        assert_eq!(s.itl_max_ns, 9000);
        assert_eq!(s.itl_last_ns, 399);
        // 368..=399.
        assert_eq!(s.itl_rolling_ns, (368 + 399) / 2);
    }

    #[test]
    fn a_new_prompt_clears_its_figures_but_not_the_totals() {
        let t = Telemetry::new(1, 16);
        t.prompt_begin(2, 0, 100);
        t.prefill_progress(3, 50, 3);
        t.token_out();
        t.classes(&[1; OP_CLASSES]);
        t.prompt_end(1, 4);
        t.prompt_begin(2, 4, 40);
        let s = t.snapshot(0);
        assert_eq!((s.prefill_tokens, s.decode_tokens, s.profiled_tokens), (0, 0, 0));
        assert_eq!((s.tokens_in, s.tokens_out, s.prompts), (3, 1, 1));
        assert_eq!(s.peak_resident_bytes, 100, "a watermark doesn't go down");
        assert_eq!(s.stop_reason, 0);
        t.prompt_end(2, 4);
        t.reset();
        let s = t.snapshot(0);
        assert_eq!((s.tokens_in, s.tokens_out, s.prompts), (0, 0, 0));
    }

    /// C3K's spike: 6.45M mma at 3.44 G/s over a 40.5 ms matmul time is 4.6 %.
    #[test]
    fn mma_issue_share_matches_the_harness_arithmetic() {
        assert_eq!(mma_issue_bp(6_450_000, 40_500_000, MMA_RATE_GPC_HZ), 462);
        // Half the clock: half the rate, so twice the issue time.
        assert_eq!(mma_issue_bp(6_450_000, 40_500_000, MMA_RATE_GPC_HZ / 2), 925);
        assert_eq!(mma_issue_bp(0, 40_500_000, MMA_RATE_GPC_HZ), NA, "no mma: below tier 1");
        assert_eq!(mma_issue_bp(1, 0, MMA_RATE_GPC_HZ), NA, "not profiled yet");
        assert_eq!(mma_issue_bp(1, 1, 0), NA, "clock unknown");
    }

    #[test]
    fn mac_share_counts_gqa_as_simt() {
        assert_eq!(mac_share_bp(9947, 0, 53), 9947);
        assert_eq!(mac_share_bp(0, 100, 5), NA, "SIMT matmuls: nothing on tensor cores");
    }

    #[test]
    fn plan_replaces_and_restores_the_op_table() {
        let t = Telemetry::new(2, 16);
        let mut f = PlanFacts { launches: 4, mma: 7, ..PlanFacts::default() };
        f.class_launches[OpClass::MatmulAttn as usize] = 3;
        f.class_tier = [Tier::Simt as u32; OP_CLASSES];
        f.class_tier[OpClass::MatmulAttn as usize] = Tier::Hmma as u32;
        f.class_launches[OpClass::Add as usize] = 1;
        t.set_plan(Some(&f), 2);
        let s = t.snapshot(0);
        assert_eq!((s.ops_tier1, s.ops_tier2, s.ops_cpu), (3, 1, 0));
        t.set_plan(None, 2);
        let s = t.snapshot(0);
        assert_eq!((s.ops_tier1, s.ops_tier2, s.ops_cpu, s.launches), (0, 0, 33, 0));
    }

    fn one_token(t: &Telemetry, i: u64) {
        t.token_out();
        t.gpu_step(1000, 2, 3);
        t.decode_step(i, i, 1);
    }

    /// The per-token path, with a profile, on a block that has run for a while.
    #[test]
    fn per_token_path_does_not_allocate() {
        use crate::slm::sampler::tests::counting::events_during;
        let t = Telemetry::new(28, 512);
        t.prompt_begin(0, 0, 1);
        let ns = [5u64; OP_CLASSES];
        let (events, ()) = events_during(|| {
            for i in 0..1024 {
                one_token(&t, i);
                t.classes(&ns);
            }
            let mut lap = LapTimer::new(true);
            lap.lap(OpClass::Gqa);
            let _ = t.snapshot(1_000_000_000);
        });
        assert_eq!(events, 0);
    }

    /// The whole writer path against the bar of 23 ms per token (ADR 0001). A
    /// debug build on a loaded host still has to clear 0.1 percent of it.
    #[test]
    fn per_token_path_costs_nothing_measurable() {
        assert!(token_cost_ns() < 23_000, "{} ns per token", token_cost_ns());
    }

    #[test]
    fn lap_timer_off_records_nothing() {
        let mut l = LapTimer::new(false);
        l.lap(OpClass::Gqa);
        assert!(l.finish().is_none());
        let mut l = LapTimer::new(true);
        l.lap(OpClass::Gqa);
        assert!(l.finish().is_some());
    }

    /// A reader on another thread during writes gets consistent snapshots or
    /// says otherwise, and the writer finishes however hard the reader reads.
    #[test]
    fn concurrent_reader_sees_coherent_pairs_and_never_stops_the_writer() {
        use std::sync::atomic::AtomicBool;
        use std::sync::Arc;
        let t = Arc::new(Telemetry::new(1, 1 << 20));
        let stop = Arc::new(AtomicBool::new(false));
        let reader = {
            let (t, stop) = (t.clone(), stop.clone());
            std::thread::spawn(move || {
                let mut good = 0u64;
                while !stop.load(Relaxed) {
                    let s = t.snapshot(0);
                    if s.consistent == 1 {
                        // The writer below moves these together.
                        assert_eq!(s.itl_count, s.decode_ns);
                        good += 1;
                    }
                }
                good
            })
        };
        t.prompt_begin(2, 0, 0);
        for i in 1..=200_000u64 {
            t.decode_step(1, i, 1);
        }
        stop.store(true, Relaxed);
        assert!(reader.join().unwrap() > 0, "no consistent read at all");
        assert_eq!(t.snapshot(0).itl_count, 200_000);
    }
}
