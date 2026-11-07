//! The whole forward pass as one GPU command stream (#475,
//! `docs/adr/0003-slm-gpu-graph.md`).
//!
//! At `slm launch` this module turns a loaded model into a [`Plan`]: every
//! launch of a forward step, which buffer it reads and writes, and where a
//! barrier is needed. `kernel/gpu/slm_prog.c` encodes the plan into QMDs,
//! cbufs and one pushbuffer. A step then only:
//!
//! 1. writes the token embeddings and positions into the step page (the
//!    embedding is one row of `token_embd.weight`, decoded on the CPU);
//! 2. patches the cbufs that depend on the position: K and V output
//!    pointers, RoPE on K, the attention's sequence length;
//! 3. submits one GPFIFO entry and waits without holding a lock;
//! 4. reads the last position's logits after one L2 evict.
//!
//! KV append is not a separate op: the K and V projections write straight
//! into the cache slot of the current position, so nothing is copied.
//!
//! `forward_one` stays the reference path. Tests run the plan through a
//! software GPU ([`soft`]) and compare with llama.cpp's logits, the same
//! comparison `slm gpu golden` runs on hardware.

use alloc::boxed::Box;
use alloc::string::String;
use alloc::vec::Vec;
use core::fmt::Write as _;

use crate::slm::forward::embedding_lookup_cpu;
use crate::slm::gguf::{f16_to_f32, f32_to_f16, GgmlType};
use crate::slm::registry::LoadedSlm;

/// Keep in step with `enum slm_prog_kernel`.
#[repr(u32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Kernel {
    RmsNorm = 0,
    Rope = 1,
    SwiGlu = 2,
    Gqa = 3,
    GqaBatch = 4,
    MmQ4k = 5,
    MmQ6k = 6,
    Add = 7,
}

/// Keep in step with `struct slm_prog_op`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ProgOp {
    pub kernel: u32,
    pub rows: u32,
    pub dim: [u32; 4],
    pub ptr: [u64; 4],
    pub scal: [u32; 2],
}

const _: () = assert!(core::mem::size_of::<ProgOp>() == 64);

#[repr(C)]
pub struct OpenArgs {
    pub weights: *const u8,
    pub weights_bytes: u64,
    pub data_bytes: u64,
    pub step_bytes: u32,
    pub max_launches: [u32; 2],
    pub flags: u32,
    pub barrier: u32,
}

#[repr(C)]
#[derive(Default)]
pub struct OpenOut {
    pub handle: i32,
    pub weights_big: u32,
    pub weights_gpu_va: u64,
    pub data_gpu_va: u64,
    pub step_gpu_va: u64,
    pub data_cpu: usize,
    pub step_cpu: usize,
    pub weights_map_us: u64,
    pub table_pages_added: u32,
    pub pad: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct RunStats {
    pub gpu_ns: u64,
    pub wait_us: u64,
    pub evict_us: u64,
}

pub const ERR_NO_BACKEND: i32 = -1;
pub const ERR_NO_KERNEL: i32 = -5;

pub const FLAG_LEAN_QMD: u32 = 1;
pub const FLAG_PROFILE: u32 = 2;

const MM_OUT_F16: u32 = 1;
const RMS_EPS: f32 = 1e-6;
/// `GQA_ATTN_MAX_SEQ_LEN`: the kernel's shared-memory logits array.
const GQA_MAX_SEQ: usize = 4096;

extern "C" {
    fn slm_prog_available() -> i32;
    fn slm_prog_open(a: *const OpenArgs, out: *mut OpenOut) -> i32;
    fn slm_prog_close(h: i32);
    fn slm_prog_add(h: i32, shape: u32, op: *const ProgOp) -> i32;
    fn slm_prog_barrier(h: i32, shape: u32) -> i32;
    fn slm_prog_finish(h: i32, shape: u32) -> i32;
    fn slm_prog_patch(h: i32, shape: u32, idx: u32, op: *const ProgOp) -> i32;
    fn slm_prog_update(h: i32, shape: u32, idx: u32, op: *const ProgOp) -> i32;
    fn slm_prog_clean(h: i32, p: *const u8, len: usize);
    fn slm_prog_invalidate(h: i32, p: *const u8, len: usize);
    fn slm_prog_run(h: i32, shape: u32, stats: *mut RunStats) -> i32;
    fn slm_prog_max_batch() -> u32;
}

// ---------------------------------------------------------------------------
// Model description
// ---------------------------------------------------------------------------

/// Dimensions the plan and the arena depend on.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Dims {
    pub layers: u32,
    pub hidden: u32,
    pub intermediate: u32,
    pub head_dim: u32,
    pub n_head: u32,
    pub n_head_kv: u32,
    pub vocab: u32,
}

#[derive(Clone, Copy, Debug)]
pub struct Matmul {
    pub kernel: Kernel,
    pub w_off: u64,
    /// File offset of an FP32 bias, or none.
    pub bias_off: Option<u64>,
}

#[derive(Clone, Copy, Debug)]
pub struct LayerDesc {
    pub q: Matmul,
    pub k: Matmul,
    pub v: Matmul,
    pub o: Matmul,
    pub gate: Matmul,
    pub up: Matmul,
    pub down: Matmul,
}

#[derive(Clone, Debug)]
pub struct ModelDesc {
    pub dims: Dims,
    pub layers: Vec<LayerDesc>,
    pub lm_head: Matmul,
}

fn mm_kernel(t: GgmlType) -> Option<Kernel> {
    match t.0 {
        x if x == GgmlType::Q4_K.0 => Some(Kernel::MmQ4k),
        x if x == GgmlType::Q6_K.0 => Some(Kernel::MmQ6k),
        _ => None,
    }
}

impl ModelDesc {
    /// Read every tensor's type and file offset. `Err` names what this
    /// program can't run; the session then stays on the CPU.
    pub fn from_slm(slm: &LoadedSlm, vocab: u32) -> Result<Self, &'static str> {
        let a = slm.arch();
        if a.block_count == 0
            || a.head_dim == 0
            || a.head_count == 0
            || a.head_count_kv == 0
            || a.head_count % a.head_count_kv != 0
            || a.head_dim % 2 != 0
        {
            return Err("unsupported model shape");
        }
        let dims = Dims {
            layers: a.block_count,
            hidden: a.embedding_length,
            intermediate: a.feed_forward_length,
            head_dim: a.head_dim,
            n_head: a.head_count,
            n_head_kv: a.head_count_kv,
            vocab,
        };
        // The Q4_K and Q6_K kernels decode whole 256-element blocks.
        if dims.hidden % 256 != 0
            || dims.intermediate % 256 != 0
            || (dims.n_head * dims.head_dim) % 256 != 0
        {
            return Err("activation sizes are not multiples of 256");
        }
        let base = slm.weight_bytes().ok_or("model has no weight bytes")?.as_ptr() as usize;
        let offset = |bytes: &[u8]| (bytes.as_ptr() as usize - base) as u64;
        let mm = |w: &str, b: Option<&str>| -> Result<Matmul, &'static str> {
            let (info, bytes) =
                slm.tensor_info_and_bytes(w).ok_or("a layer tensor is missing")?;
            let kernel = mm_kernel(GgmlType(info.ggml_type)).ok_or("weights are not Q4_K or Q6_K")?;
            let bias_off = b.and_then(|n| slm.tensor_bytes(n)).map(&offset);
            Ok(Matmul { kernel, w_off: offset(bytes), bias_off })
        };
        let mut layers = Vec::with_capacity(dims.layers as usize);
        let mut nb = String::new();
        let mut nb2 = String::new();
        for l in 0..dims.layers {
            let mut with = |suffix: &str, bias: Option<&str>| -> Result<Matmul, &'static str> {
                nb.clear();
                let _ = write!(nb, "blk.{}.{}", l, suffix);
                match bias {
                    Some(bs) => {
                        nb2.clear();
                        let _ = write!(nb2, "blk.{}.{}", l, bs);
                        mm(&nb, Some(&nb2))
                    }
                    None => mm(&nb, None),
                }
            };
            layers.push(LayerDesc {
                q: with("attn_q.weight", Some("attn_q.bias"))?,
                k: with("attn_k.weight", Some("attn_k.bias"))?,
                v: with("attn_v.weight", Some("attn_v.bias"))?,
                o: with("attn_output.weight", None)?,
                gate: with("ffn_gate.weight", None)?,
                up: with("ffn_up.weight", None)?,
                down: with("ffn_down.weight", None)?,
            });
        }
        let lm_name = if slm.tensor_info("output.weight").is_some() {
            "output.weight"
        } else {
            "token_embd.weight"
        };
        Ok(Self { dims, layers, lm_head: mm(lm_name, None)? })
    }
}

// ---------------------------------------------------------------------------
// Arena
// ---------------------------------------------------------------------------

const FP16: u64 = 2;
const FP32: u64 = 4;

fn align256(v: u64) -> u64 {
    (v + 255) & !255
}

/// Byte offsets in the data region (L2-cacheable) and the step page.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Arena {
    pub x: u64,
    pub xn: u64,
    pub q: u64,
    pub attn: u64,
    pub o: u64,
    pub gate: u64,
    pub up: u64,
    pub silu: u64,
    pub m: u64,
    pub gamma: u64,
    pub rope: u64,
    pub kv_k: u64,
    pub kv_v: u64,
    pub logits: u64,
    pub data_bytes: u64,
    pub step_embed: u64,
    pub step_positions: u64,
    pub step_bytes: u32,
    /// Bytes of K (or V) per position per layer.
    pub kv_per_pos: u64,
    /// Bytes of K (or V) per layer.
    pub kv_per_layer: u64,
}

impl Arena {
    /// `batch` rows of activations, `max_ctx` positions of KV cache.
    pub fn compute(d: &Dims, max_ctx: u64, batch: u64) -> Option<Self> {
        if max_ctx == 0 || batch == 0 {
            return None;
        }
        let hidden = d.hidden as u64;
        let inter = d.intermediate as u64;
        let nqhd = (d.n_head as u64).checked_mul(d.head_dim as u64)?;
        let per_pos = (d.n_head_kv as u64).checked_mul(d.head_dim as u64)?.checked_mul(FP16)?;
        let per_layer = per_pos.checked_mul(max_ctx)?;
        let layers = d.layers as u64;

        let mut at = 0u64;
        let mut take = |bytes: u64| -> Option<u64> {
            let r = at;
            at = at.checked_add(align256(bytes))?;
            Some(r)
        };
        let row_hidden = batch.checked_mul(hidden)?.checked_mul(FP16)?;
        let x = take(row_hidden)?;
        let xn = take(row_hidden)?;
        let q = take(batch.checked_mul(nqhd)?.checked_mul(FP16)?)?;
        let attn = take(batch.checked_mul(nqhd)?.checked_mul(FP16)?)?;
        let o = take(row_hidden)?;
        let row_inter = batch.checked_mul(inter)?.checked_mul(FP16)?;
        let gate = take(row_inter)?;
        let up = take(row_inter)?;
        let silu = take(row_inter)?;
        let m = take(row_hidden)?;
        // Two norms per layer, plus the final one.
        let gamma = take(layers.checked_mul(2)?.checked_add(1)?.checked_mul(hidden)?.checked_mul(FP16)?)?;
        let rope = take(max_ctx.checked_mul(d.head_dim as u64)?.checked_mul(FP32)?)?;
        let kv_k = take(per_layer.checked_mul(layers)?)?;
        let kv_v = take(per_layer.checked_mul(layers)?)?;
        let logits = take((d.vocab as u64).checked_mul(FP32)?)?;
        let data_bytes = at;

        let step_embed = 0u64;
        let step_positions = align256(row_hidden);
        let step_bytes = u32::try_from(step_positions.checked_add(align256(batch.checked_mul(4)?))?).ok()?;
        Some(Self {
            x,
            xn,
            q,
            attn,
            o,
            gate,
            up,
            silu,
            m,
            gamma,
            rope,
            kv_k,
            kv_v,
            logits,
            data_bytes,
            step_embed,
            step_positions,
            step_bytes,
            kv_per_pos: per_pos,
            kv_per_layer: per_layer,
        })
    }
}

// ---------------------------------------------------------------------------
// Plan
// ---------------------------------------------------------------------------

/// A pointer in a plan, resolved per step by [`PlanOp::materialize`].
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Ref {
    Null,
    Weights(u64),
    Data(u64),
    Step(u64),
    /// `base + pos * stride`: this step's slot in the KV cache.
    DataAtPos { off: u64, stride: u64 },
    /// `base + (rows - 1) * stride`: the last row of a batch.
    DataLastRow { off: u64, stride: u64 },
    StepLastRow { off: u64, stride: u64 },
}

/// A cbuf scalar, resolved per step.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Scal {
    Const(u32),
    /// `pos + rows`: positions the single row attends to.
    SeqLen,
    /// `pos`: positions before the batch's first row.
    SeqBase,
}

/// Logical buffer ids for dependency tracking.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Buf {
    Embed,
    X,
    Xn,
    Q,
    Attn,
    O,
    Gate,
    Up,
    Silu,
    M,
    KvK(u32),
    KvV(u32),
    Logits,
}

#[derive(Clone, Copy, Debug)]
pub struct Bases {
    pub weights: u64,
    pub data: u64,
    pub step: u64,
}

#[derive(Clone, Copy, Debug)]
pub struct StepArgs {
    pub pos: u32,
    pub rows: u32,
}

#[derive(Clone, Debug)]
pub struct PlanOp {
    pub kernel: Kernel,
    /// True when the op has one row per batch row; else one row always.
    pub batched: bool,
    pub dim: [u32; 4],
    pub ptr: [Ref; 4],
    pub scal: [Scal; 2],
    pub reads: Vec<Buf>,
    pub writes: Vec<Buf>,
}

impl PlanOp {
    fn resolve(r: Ref, b: &Bases, s: &StepArgs) -> u64 {
        match r {
            Ref::Null => 0,
            Ref::Weights(o) => b.weights + o,
            Ref::Data(o) => b.data + o,
            Ref::Step(o) => b.step + o,
            Ref::DataAtPos { off, stride } => b.data + off + (s.pos as u64) * stride,
            Ref::DataLastRow { off, stride } => b.data + off + (s.rows as u64 - 1) * stride,
            Ref::StepLastRow { off, stride } => b.step + off + (s.rows as u64 - 1) * stride,
        }
    }

    pub fn materialize(&self, b: &Bases, s: &StepArgs) -> ProgOp {
        let scal = |v: Scal| match v {
            Scal::Const(c) => c,
            Scal::SeqLen => s.pos + s.rows,
            Scal::SeqBase => s.pos,
        };
        ProgOp {
            kernel: self.kernel as u32,
            rows: if self.batched { s.rows } else { 1 },
            dim: self.dim,
            ptr: [
                Self::resolve(self.ptr[0], b, s),
                Self::resolve(self.ptr[1], b, s),
                Self::resolve(self.ptr[2], b, s),
                Self::resolve(self.ptr[3], b, s),
            ],
            scal: [scal(self.scal[0]), scal(self.scal[1])],
        }
    }

    /// Does a step have to rewrite this op's cbuf at a fixed batch size?
    pub fn depends_on_pos(&self) -> bool {
        self.ptr.iter().any(|r| matches!(r, Ref::DataAtPos { .. }))
            || self.scal.iter().any(|v| matches!(v, Scal::SeqLen | Scal::SeqBase))
    }
}

/// Everything a program does, in order. `barrier_before[i]` says a barrier
/// precedes op `i`.
#[derive(Clone, Debug)]
pub struct Plan {
    pub ops: Vec<PlanOp>,
    pub barrier_before: Vec<bool>,
    /// Indices of ops with position-dependent cbufs.
    pub patch: Vec<u32>,
    pub batched: bool,
}

struct PlanBuilder {
    plan: Plan,
    written: Vec<Buf>,
    read: Vec<Buf>,
}

impl PlanBuilder {
    fn new(batched: bool) -> Self {
        Self {
            plan: Plan { ops: Vec::new(), barrier_before: Vec::new(), patch: Vec::new(), batched },
            written: Vec::new(),
            read: Vec::new(),
        }
    }

    /// Barrier when an op reads something written since the last barrier,
    /// or writes something read or written since then. No barrier between
    /// Q, K and V, which only share an input.
    fn push(&mut self, op: PlanOp) {
        let dep = op.reads.iter().any(|b| self.written.contains(b))
            || op.writes.iter().any(|b| self.written.contains(b) || self.read.contains(b));
        if dep {
            self.written.clear();
            self.read.clear();
        }
        self.plan.barrier_before.push(dep);
        self.read.extend_from_slice(&op.reads);
        self.written.extend_from_slice(&op.writes);
        if op.depends_on_pos() {
            self.plan.patch.push(self.plan.ops.len() as u32);
        }
        self.plan.ops.push(op);
    }
}

fn mm_op(m: &Matmul, x: Ref, out: Ref, k: u32, n: u32, reads: Buf, writes: Buf, f16: bool, batched: bool) -> PlanOp {
    PlanOp {
        kernel: m.kernel,
        batched,
        dim: [k, n, 0, 0],
        ptr: [
            x,
            Ref::Weights(m.w_off),
            out,
            m.bias_off.map(Ref::Weights).unwrap_or(Ref::Null),
        ],
        scal: [Scal::Const(if f16 { MM_OUT_F16 } else { 0 }), Scal::Const(0)],
        reads: alloc::vec![reads],
        writes: alloc::vec![writes],
    }
}

/// Build the launches for one program shape: `batched` covers prefill
/// (one row per token, causal attention), otherwise a single row.
pub fn build_plan(d: &ModelDesc, a: &Arena, batched: bool) -> Plan {
    let dims = &d.dims;
    let hidden = dims.hidden;
    let inter = dims.intermediate;
    let nqhd = dims.n_head * dims.head_dim;
    let nkvhd = dims.n_head_kv * dims.head_dim;
    let eps = RMS_EPS.to_bits();
    let gamma_row = hidden as u64 * FP16;
    let mut pb = PlanBuilder::new(batched);

    let norm = |x: Ref, gamma: u64, out: Ref, reads: Buf, writes: Buf, batched: bool| PlanOp {
        kernel: Kernel::RmsNorm,
        batched,
        dim: [hidden, 0, 0, 0],
        ptr: [x, Ref::Data(gamma), out, Ref::Null],
        scal: [Scal::Const(eps), Scal::Const(0)],
        reads: alloc::vec![reads],
        writes: alloc::vec![writes],
    };
    let add = |x: Ref, y: u64, reads: [Buf; 2]| PlanOp {
        kernel: Kernel::Add,
        batched: true,
        dim: [hidden, 0, 0, 0],
        ptr: [x, Ref::Data(y), Ref::Data(a.x), Ref::Null],
        scal: [Scal::Const(0), Scal::Const(0)],
        reads: reads.to_vec(),
        writes: alloc::vec![Buf::X],
    };
    let rope = |vec: Ref, heads: u32, buf: Buf| PlanOp {
        kernel: Kernel::Rope,
        batched: true,
        dim: [heads, dims.head_dim, 0, 0],
        ptr: [vec, Ref::Step(a.step_positions), Ref::Data(a.rope), Ref::Null],
        scal: [Scal::Const(0), Scal::Const(0)],
        reads: alloc::vec![buf],
        writes: alloc::vec![buf],
    };

    for (i, l) in d.layers.iter().enumerate() {
        let li = i as u32;
        let kv_k = a.kv_k + i as u64 * a.kv_per_layer;
        let kv_v = a.kv_v + i as u64 * a.kv_per_layer;
        let k_at = Ref::DataAtPos { off: kv_k, stride: a.kv_per_pos };
        let v_at = Ref::DataAtPos { off: kv_v, stride: a.kv_per_pos };
        // The first layer reads the embeddings the CPU wrote to the step page.
        let (x_ref, x_buf) = if i == 0 {
            (Ref::Step(a.step_embed), Buf::Embed)
        } else {
            (Ref::Data(a.x), Buf::X)
        };

        pb.push(norm(x_ref, a.gamma + (2 * i as u64) * gamma_row, Ref::Data(a.xn), x_buf, Buf::Xn, true));
        pb.push(mm_op(&l.q, Ref::Data(a.xn), Ref::Data(a.q), hidden, nqhd, Buf::Xn, Buf::Q, true, true));
        pb.push(mm_op(&l.k, Ref::Data(a.xn), k_at, hidden, nkvhd, Buf::Xn, Buf::KvK(li), true, true));
        pb.push(mm_op(&l.v, Ref::Data(a.xn), v_at, hidden, nkvhd, Buf::Xn, Buf::KvV(li), true, true));
        pb.push(rope(Ref::Data(a.q), dims.n_head, Buf::Q));
        pb.push(rope(k_at, dims.n_head_kv, Buf::KvK(li)));
        pb.push(PlanOp {
            kernel: if batched { Kernel::GqaBatch } else { Kernel::Gqa },
            batched,
            dim: [dims.n_head, dims.n_head_kv, dims.head_dim, 0],
            ptr: [Ref::Data(a.q), Ref::Data(kv_k), Ref::Data(kv_v), Ref::Data(a.attn)],
            scal: [if batched { Scal::SeqBase } else { Scal::SeqLen }, Scal::Const(0)],
            reads: alloc::vec![Buf::Q, Buf::KvK(li), Buf::KvV(li)],
            writes: alloc::vec![Buf::Attn],
        });
        pb.push(mm_op(&l.o, Ref::Data(a.attn), Ref::Data(a.o), nqhd, hidden, Buf::Attn, Buf::O, true, true));
        pb.push(add(x_ref, a.o, [x_buf, Buf::O]));
        pb.push(norm(Ref::Data(a.x), a.gamma + (2 * i as u64 + 1) * gamma_row, Ref::Data(a.xn), Buf::X, Buf::Xn, true));
        pb.push(mm_op(&l.gate, Ref::Data(a.xn), Ref::Data(a.gate), hidden, inter, Buf::Xn, Buf::Gate, true, true));
        pb.push(mm_op(&l.up, Ref::Data(a.xn), Ref::Data(a.up), hidden, inter, Buf::Xn, Buf::Up, true, true));
        pb.push(PlanOp {
            kernel: Kernel::SwiGlu,
            batched: true,
            dim: [inter, 0, 0, 0],
            ptr: [Ref::Data(a.gate), Ref::Data(a.up), Ref::Data(a.silu), Ref::Null],
            scal: [Scal::Const(0), Scal::Const(0)],
            reads: alloc::vec![Buf::Gate, Buf::Up],
            writes: alloc::vec![Buf::Silu],
        });
        pb.push(mm_op(&l.down, Ref::Data(a.silu), Ref::Data(a.m), inter, hidden, Buf::Silu, Buf::M, true, true));
        pb.push(add(Ref::Data(a.x), a.m, [Buf::X, Buf::M]));
    }
    // Only the last position produces logits.
    pb.push(norm(
        Ref::DataLastRow { off: a.x, stride: gamma_row },
        a.gamma + 2 * dims.layers as u64 * gamma_row,
        Ref::Data(a.xn),
        Buf::X,
        Buf::Xn,
        false,
    ));
    pb.push(mm_op(&d.lm_head, Ref::Data(a.xn), Ref::Data(a.logits), hidden, dims.vocab, Buf::Xn, Buf::Logits, false, false));
    pb.plan
}

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------

/// Barrier kinds and QMD options under measurement (`slm gpu opts`).
static OPT_BARRIER: core::sync::atomic::AtomicU32 = core::sync::atomic::AtomicU32::new(0);
static OPT_FLAGS: core::sync::atomic::AtomicU32 = core::sync::atomic::AtomicU32::new(0);

/// Used by the next `slm launch`. `barrier` is an `enum ga10b_prog_barrier`.
pub fn set_options(barrier: u32, flags: u32) {
    OPT_BARRIER.store(barrier, core::sync::atomic::Ordering::Relaxed);
    OPT_FLAGS.store(flags, core::sync::atomic::Ordering::Relaxed);
}

// ---------------------------------------------------------------------------
// The program
// ---------------------------------------------------------------------------

/// Why no program exists for a session.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BuildError {
    /// This boot has no channel to run on (kexec, QEMU, Pi 5).
    NoBackend,
    /// The model or the library can't run this way.
    Failed(&'static str),
}

const SHAPE_DECODE: u32 = 0;
const SHAPE_PREFILL: u32 = 1;

pub struct GpuProgram {
    h: i32,
    arena: Arena,
    bases: Bases,
    decode: Plan,
    prefill: Option<Plan>,
    data_cpu: usize,
    step_cpu: usize,
    dims: Dims,
    pub max_batch: u32,
    prefill_rows: u32,
    /// Tokens the GPU KV cache holds, for the replay if the GPU fails.
    pub history: Vec<u32>,
    embed_scratch: Vec<f32>,
    pub steps: u64,
    pub tokens: u64,
    pub gpu_ns_total: u64,
    pub wait_us_total: u64,
    pub evict_us_total: u64,
    pub weights_big: bool,
    pub weights_map_us: u64,
    pub table_pages_added: u32,
    pub launches: u32,
    pub barriers: u32,
    pub data_bytes: u64,
}

// SAFETY: the raw addresses are the program's own memory, used only by the
// task holding the session, and the session table hands a session to one
// task at a time.
unsafe impl Send for GpuProgram {}

fn err_message(rc: i32) -> &'static str {
    match rc {
        ERR_NO_BACKEND => "no SLM-OS-built GPU channel (needs a UEFI-direct boot)",
        -2 => "invalid GPU program",
        -3 => "out of memory for the session arena",
        -4 => "GPU mapping failed",
        ERR_NO_KERNEL => "a kernel is missing from the operator library",
        -6 => "GPU step timed out",
        -7 => "GPU submit failed",
        -8 => "GPU program failed earlier",
        -9 => "L2 evict timed out",
        -10 => "GPU step started with IRQs masked",
        _ => "GPU program error",
    }
}

impl GpuProgram {
    /// Allocate the arena, map the weights, build every launch. On `Err`
    /// nothing is left behind.
    pub fn build(slm: &LoadedSlm, max_ctx: usize, vocab: u32, rope_table: &[f32]) -> Result<Box<Self>, BuildError> {
        // SAFETY: plain FFI query with no arguments.
        if unsafe { slm_prog_available() } == 0 {
            return Err(BuildError::NoBackend);
        }
        if max_ctx > GQA_MAX_SEQ {
            return Err(BuildError::Failed("--ctx above the attention kernel's 4096 positions"));
        }
        let desc = ModelDesc::from_slm(slm, vocab).map_err(BuildError::Failed)?;
        // SAFETY: plain FFI query with no arguments.
        let max_batch = unsafe { slm_prog_max_batch() }.max(1);
        let arena = Arena::compute(&desc.dims, max_ctx as u64, max_batch as u64)
            .ok_or(BuildError::Failed("session arena does not fit"))?;
        let decode = build_plan(&desc, &arena, false);
        let prefill = (max_batch > 1).then(|| build_plan(&desc, &arena, true));

        let weights = slm.weight_bytes().ok_or(BuildError::Failed("model has no weight bytes"))?;
        let mut flags = OPT_FLAGS.load(core::sync::atomic::Ordering::Relaxed);
        flags &= FLAG_LEAN_QMD | FLAG_PROFILE;
        let args = OpenArgs {
            weights: weights.as_ptr(),
            weights_bytes: weights.len() as u64,
            data_bytes: arena.data_bytes,
            step_bytes: arena.step_bytes,
            max_launches: [
                decode.ops.len() as u32,
                prefill.as_ref().map_or(0, |p| p.ops.len() as u32),
            ],
            flags,
            barrier: OPT_BARRIER.load(core::sync::atomic::Ordering::Relaxed),
        };
        let mut out = OpenOut::default();
        // SAFETY: both pointers are to live locals of the declared types.
        let rc = unsafe { slm_prog_open(&args, &mut out) };
        if rc != 0 {
            return Err(if rc == ERR_NO_BACKEND {
                BuildError::NoBackend
            } else {
                BuildError::Failed(err_message(rc))
            });
        }

        let mut p = Box::new(Self {
            h: out.handle,
            arena,
            bases: Bases { weights: out.weights_gpu_va, data: out.data_gpu_va, step: out.step_gpu_va },
            decode,
            prefill,
            data_cpu: out.data_cpu,
            step_cpu: out.step_cpu,
            dims: desc.dims,
            max_batch,
            prefill_rows: 0,
            history: Vec::new(),
            embed_scratch: alloc::vec![0.0f32; desc.dims.hidden as usize + 256],
            steps: 0,
            tokens: 0,
            gpu_ns_total: 0,
            wait_us_total: 0,
            evict_us_total: 0,
            weights_big: out.weights_big != 0,
            weights_map_us: out.weights_map_us,
            table_pages_added: out.table_pages_added,
            launches: 0,
            barriers: 0,
            data_bytes: arena.data_bytes,
        });
        // From here on, Drop closes the session.
        p.fill_constants(slm, rope_table).map_err(BuildError::Failed)?;
        p.emit(SHAPE_DECODE).map_err(|rc| BuildError::Failed(err_message(rc)))?;
        if p.prefill.is_some() {
            p.emit(SHAPE_PREFILL).map_err(|rc| BuildError::Failed(err_message(rc)))?;
        }
        Ok(p)
    }

    pub fn dims(&self) -> Dims {
        self.dims
    }

    /// Gammas as FP16 and the RoPE table, then clean. The layout comes
    /// from [`Arena`].
    fn fill_constants(&mut self, slm: &LoadedSlm, rope_table: &[f32]) -> Result<(), &'static str> {
        let d = self.dims;
        let hidden = d.hidden as usize;
        let mut name = String::new();
        for g in 0..(2 * d.layers as usize + 1) {
            name.clear();
            if g == 2 * d.layers as usize {
                name.push_str("output_norm.weight");
            } else if g % 2 == 0 {
                let _ = write!(name, "blk.{}.attn_norm.weight", g / 2);
            } else {
                let _ = write!(name, "blk.{}.ffn_norm.weight", g / 2);
            }
            let bytes = slm.tensor_bytes(&name).ok_or("a norm weight is missing")?;
            if bytes.len() != hidden * 4 {
                return Err("a norm weight has the wrong size");
            }
            let off = self.arena.gamma as usize + g * hidden * 2;
            for (j, c) in bytes.chunks_exact(4).enumerate() {
                let f = f32::from_le_bytes([c[0], c[1], c[2], c[3]]);
                // SAFETY: off..off + hidden * 2 is inside the data region
                // (Arena::compute sized gamma for 2 * layers + 1 rows).
                unsafe {
                    core::ptr::write_unaligned(
                        (self.data_cpu + off + j * 2) as *mut u16,
                        f32_to_f16(f),
                    );
                }
            }
        }
        let rope_bytes = rope_table.len() * 4;
        if (self.arena.kv_k - self.arena.rope) < rope_bytes as u64 {
            return Err("RoPE table does not fit");
        }
        // SAFETY: bounds checked above; source and destination don't overlap.
        unsafe {
            core::ptr::copy_nonoverlapping(
                rope_table.as_ptr() as *const u8,
                (self.data_cpu + self.arena.rope as usize) as *mut u8,
                rope_bytes,
            );
            slm_prog_clean(
                self.h,
                (self.data_cpu + self.arena.gamma as usize) as *const u8,
                (self.arena.kv_k - self.arena.gamma) as usize,
            );
        }
        Ok(())
    }

    fn plan(&self, shape: u32) -> &Plan {
        if shape == SHAPE_DECODE {
            &self.decode
        } else {
            self.prefill.as_ref().unwrap_or(&self.decode)
        }
    }

    fn emit(&mut self, shape: u32) -> Result<(), i32> {
        // Pos 0, full batch: a step rewrites what changes before it runs.
        let rows = if shape == SHAPE_DECODE { 1 } else { self.max_batch };
        let s = StepArgs { pos: 0, rows };
        let bases = self.bases;
        let h = self.h;
        let plan = self.plan(shape);
        let mut barriers = 0u32;
        for (op, &bar) in plan.ops.iter().zip(plan.barrier_before.iter()) {
            if bar {
                // SAFETY: the handle is ours and open.
                let rc = unsafe { slm_prog_barrier(h, shape) };
                if rc < 0 {
                    return Err(rc);
                }
                barriers += 1;
            }
            let m = op.materialize(&bases, &s);
            // SAFETY: `m` outlives the call.
            let rc = unsafe { slm_prog_add(h, shape, &m) };
            if rc < 0 {
                return Err(rc);
            }
        }
        let n = plan.ops.len() as u32;
        // SAFETY: the handle is ours and open.
        let rc = unsafe { slm_prog_finish(h, shape) };
        if rc < 0 {
            return Err(rc);
        }
        if shape == SHAPE_DECODE {
            self.launches = n;
            self.barriers = barriers;
        } else {
            self.prefill_rows = self.max_batch;
        }
        Ok(())
    }

    /// Write `tokens`' embeddings and positions into the step page. Takes
    /// the model (the caller holds the registry) and touches no GPU state.
    pub fn stage_tokens(&mut self, slm: &LoadedSlm, tokens: &[u32], pos: usize) -> Option<()> {
        let hidden = self.dims.hidden as usize;
        if tokens.is_empty() || tokens.len() > self.max_batch as usize {
            return None;
        }
        for (r, &t) in tokens.iter().enumerate() {
            let dst = self.step_cpu + self.arena.step_embed as usize + r * hidden * 2;
            // SAFETY: the step page holds max_batch rows of `hidden` FP16s,
            // aligned to 2, and only this task writes it between steps.
            let row = unsafe { core::slice::from_raw_parts_mut(dst as *mut u16, hidden) };
            embedding_lookup_cpu(slm, t, row, &mut self.embed_scratch)?;
            let pdst = self.step_cpu + self.arena.step_positions as usize + r * 4;
            // SAFETY: as above, the positions array holds max_batch i32s.
            unsafe { core::ptr::write_unaligned(pdst as *mut u32, (pos + r) as u32) };
        }
        // SAFETY: the range is the step page.
        unsafe {
            slm_prog_clean(self.h, self.step_cpu as *const u8, self.arena.step_bytes as usize);
        }
        Some(())
    }

    /// One step over the rows [`Self::stage_tokens`] wrote. `out` receives
    /// the last row's logits. Waits with IRQs on, so no spinlock may be
    /// held across this call.
    pub fn run(&mut self, tokens: &[u32], pos: usize, out: &mut Vec<f32>) -> Result<(), &'static str> {
        let rows = tokens.len() as u32;
        let shape = if rows > 1 { SHAPE_PREFILL } else { SHAPE_DECODE };
        if rows > 1 && self.prefill.is_none() {
            return Err("batched step without a prefill program");
        }
        let s = StepArgs { pos: pos as u32, rows };
        self.patch_step(shape, &s).map_err(err_message)?;

        let mut stats = RunStats::default();
        // SAFETY: the handle is ours; stats outlives the call.
        let rc = unsafe { slm_prog_run(self.h, shape, &mut stats) };
        if rc != 0 {
            return Err(err_message(rc));
        }
        self.read_logits(out);
        self.steps += 1;
        self.tokens += rows as u64;
        self.gpu_ns_total = self.gpu_ns_total.wrapping_add(stats.gpu_ns);
        self.wait_us_total = self.wait_us_total.wrapping_add(stats.wait_us);
        self.evict_us_total = self.evict_us_total.wrapping_add(stats.evict_us);
        self.history.extend_from_slice(tokens);
        Ok(())
    }

    fn patch_step(&mut self, shape: u32, s: &StepArgs) -> Result<(), i32> {
        let h = self.h;
        let bases = self.bases;
        // A new batch size changes grids, so every QMD is rewritten.
        let reshape = shape == SHAPE_PREFILL && self.prefill_rows != s.rows;
        let plan = self.plan(shape);
        if reshape {
            for (i, op) in plan.ops.iter().enumerate() {
                let m = op.materialize(&bases, s);
                // SAFETY: `m` outlives the call; idx came from this plan.
                let rc = unsafe { slm_prog_update(h, shape, i as u32, &m) };
                if rc < 0 {
                    return Err(rc);
                }
            }
        } else {
            for &i in &plan.patch {
                let m = plan.ops[i as usize].materialize(&bases, s);
                // SAFETY: as above.
                let rc = unsafe { slm_prog_patch(h, shape, i, &m) };
                if rc < 0 {
                    return Err(rc);
                }
            }
        }
        if shape == SHAPE_PREFILL {
            self.prefill_rows = s.rows;
        }
        Ok(())
    }

    fn read_logits(&mut self, out: &mut Vec<f32>) {
        let n = self.dims.vocab as usize;
        let p = (self.data_cpu + self.arena.logits as usize) as *const u8;
        // SAFETY: the logits buffer holds `vocab` f32s, 256 B aligned, and
        // the GPU is done with it: slm_prog_run returned after an evict.
        unsafe {
            slm_prog_invalidate(self.h, p, n * 4);
            out.clear();
            out.extend_from_slice(core::slice::from_raw_parts(p as *const f32, n));
        }
    }

    /// Copy of the KV cache's FP16 rows, for tests of what a step wrote.
    #[cfg(test)]
    pub fn data_ptr(&self) -> usize {
        self.data_cpu
    }

    /// Mean GPU time per step, in microseconds.
    pub fn gpu_us_per_token(&self) -> u64 {
        if self.tokens == 0 {
            0
        } else {
            self.gpu_ns_total / 1000 / self.tokens
        }
    }
}

impl Drop for GpuProgram {
    fn drop(&mut self) {
        // SAFETY: the handle came from slm_prog_open and is closed once.
        unsafe { slm_prog_close(self.h) };
    }
}

/// Round-trip FP16 sum, the semantics the ADD kernel must match.
pub fn add_f16(a: u16, b: u16) -> u16 {
    f32_to_f16(f16_to_f32(a) + f16_to_f32(b))
}

// ---------------------------------------------------------------------------
// Golden comparison (`slm gpu golden`)
// ---------------------------------------------------------------------------

/// Keep in step with `SlmGpuGoldenReport` in slm_ffi.h.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct GoldenReport {
    /// Steps compared.
    pub steps: u32,
    /// First step with a logit off by more than `LOGIT_TOLERANCE`, or -1.
    pub first_bad_step: i32,
    pub first_bad_index: i32,
    /// Thousandths, since kernel C has no floating point.
    pub first_bad_got_milli: i32,
    pub first_bad_want_milli: i32,
    /// Largest error over all steps, in thousandths.
    pub max_err_milli: i32,
    pub gpu_us_per_token: u64,
}

/// Run the fixture's prompt and greedy continuation on a program and compare
/// every step's logits with llama.cpp's. With `batched`, the prompt goes in
/// as one prefill step, so only its last row has a reference.
pub fn golden_run(model: u32, rec: &[u8], batched: bool, rep: &mut GoldenReport) -> Result<(), &'static str> {
    use crate::slm::golden::{Records, LOGIT_TOLERANCE};
    let r = Records::new(rec).ok_or("bad record file")?;
    let (ids, tokens, logits, n_vocab) = match (
        r.u32s("prompt.ids"),
        r.u32s("greedy.tokens"),
        r.f32s("greedy.logits"),
        r.u32s("greedy.n_vocab"),
    ) {
        (Ok(a), Ok(b), Ok(c), Ok(d)) if !d.is_empty() && d[0] != 0 && !a.is_empty() => {
            (a, b, c, d[0] as usize)
        }
        _ => return Err("record file is missing fields"),
    };
    let info = crate::slm::registry::get_info(model as usize).ok_or("model not loaded")?;
    if info.vocab_size as usize != n_vocab {
        return Err("record file is for another model");
    }
    let a = crate::slm::registry::with_loaded_slm(model as usize, |slm| {
        (slm.arch().head_dim, slm.arch().rope_freq_base, slm.arch().architecture)
    })
    .ok_or("model not loaded")?;
    let ctx = 64usize;
    let rope = crate::inference::ops_transformer::RopeTable::new(
        a.0 as usize,
        ctx,
        a.1,
        crate::inference::ops_transformer::RopePairing::for_arch(a.2),
    );
    let mut prog = crate::slm::registry::with_loaded_slm(model as usize, |slm| {
        GpuProgram::build(slm, ctx, info.vocab_size, &rope.cos_sin)
    })
    .ok_or("model not loaded")?
    .map_err(|e| match e {
        BuildError::NoBackend => "no SLM-OS-built GPU channel (needs a UEFI-direct boot)",
        BuildError::Failed(w) => w,
    })?;

    let steps = logits.len() / n_vocab;
    let mut out = Vec::new();
    rep.first_bad_step = -1;
    rep.first_bad_index = -1;
    rep.max_err_milli = 0;
    rep.steps = 0;
    let mut max_err = 0.0f32;
    let mut step = 0usize;
    let mut compare = |step: usize, out: &[f32], rep: &mut GoldenReport| {
        let want = &logits[step * n_vocab..(step + 1) * n_vocab];
        for (i, (g, w)) in out.iter().zip(want.iter()).enumerate() {
            let e = (g - w).abs();
            if !(e <= max_err) {
                max_err = e;
            }
            if !(e <= LOGIT_TOLERANCE) && rep.first_bad_step < 0 {
                rep.first_bad_step = step as i32;
                rep.first_bad_index = i as i32;
                rep.first_bad_got_milli = (*g * 1000.0) as i32;
                rep.first_bad_want_milli = (*w * 1000.0) as i32;
            }
        }
        rep.max_err_milli = (max_err * 1000.0) as i32;
        rep.steps += 1;
    };
    let mut pos = 0usize;
    if batched && ids.len() >= 2 && (prog.max_batch as usize) >= ids.len() {
        crate::slm::registry::with_loaded_slm(model as usize, |slm| {
            prog.stage_tokens(slm, &ids, 0)
        })
        .flatten()
        .ok_or("could not stage the prompt")?;
        prog.run(&ids, 0, &mut out)?;
        pos = ids.len();
        step = ids.len() - 1;
        compare(step, &out, rep);
        step += 1;
    }
    while step < steps {
        let t = if step < ids.len() {
            ids[step]
        } else {
            *tokens.get(step - ids.len()).ok_or("record file is short")?
        };
        let toks = [t];
        crate::slm::registry::with_loaded_slm(model as usize, |slm| {
            prog.stage_tokens(slm, &toks, pos)
        })
        .flatten()
        .ok_or("could not stage a token")?;
        prog.run(&toks, pos, &mut out)?;
        compare(step, &out, rep);
        pos += 1;
        step += 1;
    }
    rep.gpu_us_per_token = prog.gpu_us_per_token();
    Ok(())
}

// ---------------------------------------------------------------------------
// Software GPU for host tests
// ---------------------------------------------------------------------------

#[cfg(test)]
pub mod soft {
    //! `slm_prog_*` implemented on the CPU kernels. Interprets the exact
    //! `ProgOp`s the C layer would encode, including their VAs, so a wrong
    //! pointer, stride, position or barrier-free dependency shows up as a
    //! wrong logit against llama.cpp.
    use super::*;
    use crate::inference::ops_transformer::{gqa_decode_step, matmul_quant_rows, rmsnorm, swiglu_elementwise};
    use std::sync::Mutex;

    pub const W_BASE: u64 = 0x10_0000_0000;
    pub const D_BASE: u64 = 0x20_0000_0000;
    pub const S_BASE: u64 = 0x30_0000_0000;

    pub struct Soft {
        pub weights: usize,
        pub weights_len: usize,
        pub data: Vec<u8>,
        pub step: Vec<u8>,
        pub shapes: [Vec<(ProgOp, usize)>; 2],
        pub barriers: [Vec<usize>; 2],
        pub runs: u32,
        pub open: bool,
    }

    /// Failure injection and backend selection.
    pub struct Knobs {
        pub no_backend: bool,
        pub missing_kernel: bool,
        pub max_batch: u32,
        /// Fail run number N (0-based) with this rc.
        pub fail_run: Option<(u32, i32)>,
        pub closes: u32,
    }

    pub static STATE: Mutex<Option<Soft>> = Mutex::new(None);
    pub static KNOBS: Mutex<Knobs> = Mutex::new(Knobs {
        no_backend: false,
        missing_kernel: false,
        max_batch: 64,
        fail_run: None,
        closes: 0,
    });

    pub fn reset_knobs() {
        let mut k = KNOBS.lock().unwrap();
        *k = Knobs { no_backend: false, missing_kernel: false, max_batch: 64, fail_run: None, closes: 0 };
    }

    fn at<'a>(s: &'a mut Soft, va: u64) -> *mut u8 {
        if va >= S_BASE {
            unsafe { s.step.as_mut_ptr().add((va - S_BASE) as usize) }
        } else if va >= D_BASE {
            unsafe { s.data.as_mut_ptr().add((va - D_BASE) as usize) }
        } else {
            assert!(va >= W_BASE, "unmapped VA {:#x}", va);
            let off = (va - W_BASE) as usize;
            assert!(off < s.weights_len, "VA past the weights");
            (s.weights + off) as *mut u8
        }
    }

    fn u16s<'a>(p: *const u8, n: usize) -> &'a [u16] {
        unsafe { core::slice::from_raw_parts(p as *const u16, n) }
    }
    fn u16s_mut<'a>(p: *mut u8, n: usize) -> &'a mut [u16] {
        unsafe { core::slice::from_raw_parts_mut(p as *mut u16, n) }
    }

    fn exec(s: &mut Soft, op: &ProgOp) {
        let rows = op.rows as usize;
        let [a, b, c, d] = op.ptr;
        match op.kernel {
            x if x == Kernel::RmsNorm as u32 => {
                let n = op.dim[0] as usize;
                let eps = f32::from_bits(op.scal[0]);
                let (xp, gp, op_) = (at(s, a), at(s, b), at(s, c));
                for r in 0..rows {
                    let x = u16s(unsafe { xp.add(r * n * 2) }, n);
                    let g = u16s(gp, n);
                    let o = u16s_mut(unsafe { op_.add(r * n * 2) }, n);
                    rmsnorm(x, g, eps, o).expect("rmsnorm");
                }
            }
            x if x == Kernel::Rope as u32 => {
                let heads = op.dim[0] as usize;
                let hd = op.dim[1] as usize;
                let half = hd / 2;
                let (vp, pp, lp) = (at(s, a), at(s, b), at(s, c));
                for r in 0..rows {
                    let pos = unsafe { *(pp as *const u32).add(r) } as usize;
                    let lut = unsafe { (lp as *const f32).add(pos * hd) };
                    for h in 0..heads {
                        let v = u16s_mut(unsafe { vp.add((r * heads + h) * hd * 2) }, hd);
                        for i in 0..half {
                            let (cs, sn) = unsafe { (*lut.add(2 * i), *lut.add(2 * i + 1)) };
                            let v0 = f16_to_f32(v[i]);
                            let v1 = f16_to_f32(v[i + half]);
                            v[i] = f32_to_f16(v0 * cs - v1 * sn);
                            v[i + half] = f32_to_f16(v0 * sn + v1 * cs);
                        }
                    }
                }
            }
            x if x == Kernel::SwiGlu as u32 => {
                let n = rows * op.dim[0] as usize;
                let (gp, up, op_) = (at(s, a), at(s, b), at(s, c));
                let mut g = u16s(gp, n).to_vec();
                swiglu_elementwise(&mut g, u16s(up, n), n);
                u16s_mut(op_, n).copy_from_slice(&g);
            }
            x if x == Kernel::Gqa as u32 || x == Kernel::GqaBatch as u32 => {
                let (nq, nkv, hd) = (op.dim[0] as usize, op.dim[1] as usize, op.dim[2] as usize);
                let (qp, kp, vp, op_) = (at(s, a), at(s, b), at(s, c), at(s, d));
                for r in 0..rows {
                    let seq = if x == Kernel::Gqa as u32 {
                        op.scal[0] as usize
                    } else {
                        op.scal[0] as usize + r + 1
                    };
                    let q = u16s(unsafe { qp.add(r * nq * hd * 2) }, nq * hd);
                    let k = u16s(kp, seq * nkv * hd);
                    let v = u16s(vp, seq * nkv * hd);
                    let o = u16s_mut(unsafe { op_.add(r * nq * hd * 2) }, nq * hd);
                    let mut scratch = alloc::vec![0.0f32; seq];
                    gqa_decode_step(q, k, v, nq, nkv, hd, seq, &mut scratch, o).expect("gqa");
                }
            }
            x if x == Kernel::MmQ4k as u32 || x == Kernel::MmQ6k as u32 => {
                let quant = if x == Kernel::MmQ4k as u32 { GgmlType::Q4_K } else { GgmlType::Q6_K };
                let (kk, n) = (op.dim[0] as usize, op.dim[1] as usize);
                let f16_out = op.scal[0] & MM_OUT_F16 != 0;
                let (xp, wp, op_) = (at(s, a), at(s, b), at(s, c));
                let bias = if d != 0 { Some(at(s, d) as *const f32) } else { None };
                let row_bytes = crate::inference::quant::quant_row_bytes(quant, kk).expect("row bytes");
                let w = unsafe { core::slice::from_raw_parts(wp as *const u8, n * row_bytes) };
                let mut q8 = alloc::vec![0u8; crate::inference::quant::q8_k_byte_size(kk).unwrap()];
                let mut dq = alloc::vec![0.0f32; kk + 256];
                let mut af = alloc::vec![0.0f32; kk];
                let mut res = alloc::vec![0.0f32; n];
                for r in 0..rows {
                    let acts = u16s(unsafe { xp.add(r * kk * 2) }, kk);
                    matmul_quant_rows(quant, w, n, kk, acts, &mut q8, &mut dq, &mut af, &mut res)
                        .expect("matmul");
                    for j in 0..n {
                        let mut v = res[j];
                        if let Some(bp) = bias {
                            v += unsafe { core::ptr::read_unaligned(bp.add(j)) };
                        }
                        unsafe {
                            if f16_out {
                                *(op_ as *mut u16).add(r * n + j) = f32_to_f16(v);
                            } else {
                                *(op_ as *mut f32).add(r * n + j) = v;
                            }
                        }
                    }
                }
            }
            x if x == Kernel::Add as u32 => {
                let n = rows * op.dim[0] as usize;
                let (ap, bp, op_) = (at(s, a), at(s, b), at(s, c));
                let aa = u16s(ap, n).to_vec();
                let bb = u16s(bp, n).to_vec();
                let o = u16s_mut(op_, n);
                for i in 0..n {
                    o[i] = add_f16(aa[i], bb[i]);
                }
            }
            k => panic!("soft GPU: unknown kernel {k}"),
        }
    }

    #[no_mangle]
    extern "C" fn slm_prog_available() -> i32 {
        !KNOBS.lock().unwrap().no_backend as i32
    }
    #[no_mangle]
    extern "C" fn slm_prog_open(a: *const OpenArgs, out: *mut OpenOut) -> i32 {
        let k = KNOBS.lock().unwrap();
        if k.missing_kernel {
            return ERR_NO_KERNEL;
        }
        let a = unsafe { &*a };
        let mut st = STATE.lock().unwrap();
        // 8-aligned host memory stands in for PMM pages.
        let data = alloc::vec![0u8; a.data_bytes as usize];
        let step = alloc::vec![0u8; a.step_bytes as usize];
        let mut s = Soft {
            weights: a.weights as usize,
            weights_len: a.weights_bytes as usize,
            data,
            step,
            shapes: [Vec::new(), Vec::new()],
            barriers: [Vec::new(), Vec::new()],
            runs: 0,
            open: true,
        };
        let o = unsafe { &mut *out };
        *o = OpenOut {
            handle: 0,
            weights_big: 1,
            weights_gpu_va: W_BASE,
            data_gpu_va: D_BASE,
            step_gpu_va: S_BASE,
            data_cpu: s.data.as_mut_ptr() as usize,
            step_cpu: s.step.as_mut_ptr() as usize,
            weights_map_us: 0,
            table_pages_added: 0,
            pad: 0,
        };
        *st = Some(s);
        0
    }
    #[no_mangle]
    extern "C" fn slm_prog_close(_h: i32) {
        KNOBS.lock().unwrap().closes += 1;
        *STATE.lock().unwrap() = None;
    }
    #[no_mangle]
    extern "C" fn slm_prog_add(_h: i32, shape: u32, op: *const ProgOp) -> i32 {
        let mut g = STATE.lock().unwrap();
        let s = g.as_mut().unwrap();
        let idx = s.shapes[shape as usize].len();
        let bars = s.barriers[shape as usize].len();
        s.shapes[shape as usize].push((unsafe { *op }, bars));
        idx as i32
    }
    #[no_mangle]
    extern "C" fn slm_prog_barrier(_h: i32, shape: u32) -> i32 {
        let mut g = STATE.lock().unwrap();
        let s = g.as_mut().unwrap();
        let at = s.shapes[shape as usize].len();
        s.barriers[shape as usize].push(at);
        0
    }
    #[no_mangle]
    extern "C" fn slm_prog_finish(_h: i32, _shape: u32) -> i32 {
        0
    }
    #[no_mangle]
    extern "C" fn slm_prog_patch(_h: i32, shape: u32, idx: u32, op: *const ProgOp) -> i32 {
        let mut g = STATE.lock().unwrap();
        g.as_mut().unwrap().shapes[shape as usize][idx as usize].0 = unsafe { *op };
        0
    }
    #[no_mangle]
    extern "C" fn slm_prog_update(h: i32, shape: u32, idx: u32, op: *const ProgOp) -> i32 {
        slm_prog_patch(h, shape, idx, op)
    }
    #[no_mangle]
    extern "C" fn slm_prog_clean(_h: i32, _p: *const u8, _len: usize) {}
    #[no_mangle]
    extern "C" fn slm_prog_invalidate(_h: i32, _p: *const u8, _len: usize) {}
    #[no_mangle]
    extern "C" fn slm_prog_run(_h: i32, shape: u32, _stats: *mut RunStats) -> i32 {
        let mut g = STATE.lock().unwrap();
        let s = g.as_mut().unwrap();
        let run = s.runs;
        s.runs += 1;
        if let Some((n, rc)) = KNOBS.lock().unwrap().fail_run {
            if n == run {
                return rc;
            }
        }
        let ops: Vec<ProgOp> = s.shapes[shape as usize].iter().map(|x| x.0).collect();
        for op in &ops {
            exec(s, op);
        }
        0
    }
    #[no_mangle]
    extern "C" fn slm_prog_max_batch() -> u32 {
        KNOBS.lock().unwrap().max_batch
    }
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use crate::slm::golden::{Records, LOGIT_TOLERANCE};
    use crate::slm::registry::{self, load_slm, reset_for_tests, with_loaded_slm};
    use crate::slm::session::{self, GpuMode, ForwardPath};

    const TINY: &[u8] = include_bytes!("../../../scripts/fixtures/slm/tiny_qwen2.gguf");
    const GREEDY: &[u8] = include_bytes!("../../../scripts/fixtures/slm/tiny_qwen2_greedy.rec");

    const QWEN_1_5B: Dims = Dims {
        layers: 28,
        hidden: 1536,
        intermediate: 8960,
        head_dim: 128,
        n_head: 12,
        n_head_kv: 2,
        vocab: 151936,
    };

    fn fake_desc(d: Dims) -> ModelDesc {
        let m = Matmul { kernel: Kernel::MmQ4k, w_off: 0x1000, bias_off: None };
        let l = LayerDesc { q: m, k: m, v: m, o: m, gate: m, up: m, down: m };
        ModelDesc { dims: d, layers: alloc::vec![l; d.layers as usize], lm_head: m }
    }

    #[test]
    fn arena_sizes_match_the_model() {
        let a = Arena::compute(&QWEN_1_5B, 4096, 1).unwrap();
        // 2 heads * 128 dims * 2 B = 512 B per position per layer, K and V.
        assert_eq!(a.kv_per_pos, 512);
        assert_eq!(a.kv_per_layer, 512 * 4096);
        assert_eq!(a.kv_v - a.kv_k, 28 * 512 * 4096);
        assert_eq!(a.logits - a.kv_v, 28 * 512 * 4096);
        assert_eq!(a.rope % 256, 0);
        // KV is about 117 MB in all, so that plus the LUT and activations
        // stays well under the board's memory, and the sum is exact.
        let kv = 2 * 28 * 512 * 4096u64;
        assert!(a.data_bytes > kv && a.data_bytes < kv + 8 * 1024 * 1024, "{}", a.data_bytes);
        assert_eq!(a.data_bytes, a.logits + align256(151936 * 4));

        // A batch only grows activations and the step page.
        let b = Arena::compute(&QWEN_1_5B, 4096, 64).unwrap();
        assert_eq!(b.step_positions, align256(64 * 1536 * 2));
        assert_eq!(b.step_bytes as u64, b.step_positions + align256(64 * 4));
        assert_eq!(b.xn - b.x, 64 * 1536 * 2);
        assert_eq!(b.logits - b.kv_k, a.logits - a.kv_k);

        // Overflow and nonsense are refused rather than wrapped.
        assert!(Arena::compute(&QWEN_1_5B, 0, 1).is_none());
        assert!(Arena::compute(&QWEN_1_5B, 1, 0).is_none());
        assert!(Arena::compute(&QWEN_1_5B, u64::MAX / 2, 1).is_none());
    }

    #[test]
    fn plan_has_expected_shape_and_barriers() {
        let d = fake_desc(QWEN_1_5B);
        let a = Arena::compute(&d.dims, 4096, 1).unwrap();
        let p = build_plan(&d, &a, false);
        // 15 launches a layer, then the final norm and the LM head.
        assert_eq!(p.ops.len(), 28 * 15 + 2);
        assert!(p.ops.len() <= 1000);
        let n_rope = p.ops.iter().filter(|o| o.kernel == Kernel::Rope).count();
        assert_eq!(n_rope, 2 * 28);
        // Every per-position op, and nothing else, is on the patch list:
        // K out, V out, RoPE K, attention.
        assert_eq!(p.patch.len(), 4 * 28);

        // Layer 0: norm | Q K V | RoPE RoPE | GQA | O | add | norm |
        // gate up | swiglu | down | add.
        let want = [
            false, true, false, false, true, false, true, true, true, true, true, false, true,
            true, true,
        ];
        assert_eq!(&p.barrier_before[..15], &want);
        // The next layer's norm reads x, written by the last add.
        assert!(p.barrier_before[15]);
        assert!(!p.barrier_before[16 + 1], "K shares only its input with Q");
        // The first op of the last two (LM head reads the norm's output).
        assert!(p.barrier_before[p.ops.len() - 1]);
        // Only the final norm and LM head are single-row regardless of batch.
        let last = &p.ops[p.ops.len() - 1];
        assert!(!last.batched);
        assert_eq!(last.dim[1], 151936);
        assert_eq!(last.scal[0], Scal::Const(0), "logits stay FP32");
    }

    #[test]
    fn every_dependency_has_a_barrier() {
        // Independent check of the builder's rule, on the finished plan.
        let d = fake_desc(QWEN_1_5B);
        let a = Arena::compute(&d.dims, 4096, 64).unwrap();
        for batched in [false, true] {
            let p = build_plan(&d, &a, batched);
            let mut w: Vec<Buf> = Vec::new();
            let mut r: Vec<Buf> = Vec::new();
            for (op, &bar) in p.ops.iter().zip(p.barrier_before.iter()) {
                if bar {
                    w.clear();
                    r.clear();
                }
                for b in &op.reads {
                    assert!(!w.contains(b), "read of {b:?} races a write");
                }
                for b in &op.writes {
                    assert!(!w.contains(b) && !r.contains(b), "write of {b:?} races");
                }
                r.extend_from_slice(&op.reads);
                w.extend_from_slice(&op.writes);
            }
        }
    }

    #[test]
    fn materialize_resolves_positions_and_last_row() {
        let d = fake_desc(QWEN_1_5B);
        let a = Arena::compute(&d.dims, 4096, 64).unwrap();
        let bases = Bases { weights: 0x1000_0000, data: 0x2000_0000, step: 0x3000_0000 };
        let p = build_plan(&d, &a, false);
        let s = StepArgs { pos: 7, rows: 1 };
        // Layer 1's K projection writes position 7 of layer 1.
        let k1 = p.ops[15 + 2].materialize(&bases, &s);
        assert_eq!(k1.ptr[2], bases.data + a.kv_k + a.kv_per_layer + 7 * a.kv_per_pos);
        assert_eq!(k1.rows, 1);
        // Layer 1's attention sees all 8 positions from the cache's base.
        let g1 = p.ops[15 + 6].materialize(&bases, &s);
        assert_eq!(g1.kernel, Kernel::Gqa as u32);
        assert_eq!(g1.scal[0], 8);
        assert_eq!(g1.ptr[1], bases.data + a.kv_k + a.kv_per_layer);
        // Layer 0 reads the step page; layer 1 reads x.
        assert_eq!(p.ops[0].materialize(&bases, &s).ptr[0], bases.step + a.step_embed);
        assert_eq!(p.ops[15].materialize(&bases, &s).ptr[0], bases.data + a.x);

        // Prefill: the attention takes base position; the final norm takes
        // the last row of x.
        let pb = build_plan(&d, &a, true);
        let s = StepArgs { pos: 3, rows: 5 };
        let gb = pb.ops[6].materialize(&bases, &s);
        assert_eq!(gb.kernel, Kernel::GqaBatch as u32);
        assert_eq!(gb.scal[0], 3);
        assert_eq!(gb.rows, 5);
        let fin = pb.ops[pb.ops.len() - 2].materialize(&bases, &s);
        assert_eq!(fin.rows, 1);
        assert_eq!(fin.ptr[0], bases.data + a.x + 4 * 1536 * 2);
    }

    fn compare_logits_for_tests(got: &[f32], want: &[f32], step: usize) {
        assert_eq!(got.len(), want.len());
        for (i, (g, w)) in got.iter().zip(want.iter()).enumerate() {
            assert!(
                (g - w).abs() <= LOGIT_TOLERANCE,
                "step {step} logit {i}: got {g} want {w}"
            );
        }
    }

    unsafe extern "C" fn collect(
        user: *mut core::ffi::c_void,
        token_id: u32,
        _bytes: *const u8,
        _len: usize,
    ) -> i32 {
        // SAFETY: `user` is the Vec passed by `prompt_on_session`.
        unsafe { (*(user as *mut Vec<u32>)).push(token_id) };
        1
    }

    fn prompt_on_session(sid: u32, text: &[u8], n: u32) -> Vec<u32> {
        let mut got: Vec<u32> = Vec::new();
        // SAFETY: text and got outlive the call.
        let rc = unsafe {
            crate::rust_slm_prompt_ex(
                sid,
                text.as_ptr(),
                text.len(),
                crate::SLM_PROMPT_RAW,
                n,
                collect,
                None,
                &mut got as *mut Vec<u32> as *mut core::ffi::c_void,
            )
        };
        assert_eq!(rc, 0);
        got
    }

    fn load_tiny() -> u32 {
        crate::slm::registry::ensure_mm_initialized_for_tests();
        reset_for_tests();
        load_slm(b"tiny-qwen2", TINY).expect("tiny loads") as u32
    }

    fn rec() -> Records<'static> {
        Records::new(GREEDY).unwrap()
    }

    fn build_tiny(model: u32, ctx: usize) -> Box<GpuProgram> {
        let vocab = registry::get_info(model as usize).unwrap().vocab_size;
        with_loaded_slm(model as usize, |slm| {
            let a = slm.arch();
            let t = crate::inference::ops_transformer::RopeTable::new(
                a.head_dim as usize,
                ctx,
                a.rope_freq_base,
                crate::inference::ops_transformer::RopePairing::HalfSplit,
            );
            GpuProgram::build(slm, ctx, vocab, &t.cos_sin)
        })
        .unwrap()
        .map_err(|_| ())
        .expect("build")
    }

    fn step_prog(p: &mut GpuProgram, model: u32, toks: &[u32], pos: usize, out: &mut Vec<f32>) {
        with_loaded_slm(model as usize, |slm| p.stage_tokens(slm, toks, pos)).unwrap().unwrap();
        p.run(toks, pos, out).unwrap();
    }

    /// The program's dataflow against llama.cpp, one token at a time. This
    /// is what catches a wrong pointer, stride, or position in the plan.
    #[test]
    fn soft_gpu_decode_matches_llama_cpp_logits() {
        soft::reset_knobs();
        let model = load_tiny();
        let r = rec();
        let ids = r.u32s("prompt.ids").unwrap();
        let tokens = r.u32s("greedy.tokens").unwrap();
        let logits = r.f32s("greedy.logits").unwrap();
        let n_vocab = r.u32s("greedy.n_vocab").unwrap()[0] as usize;
        let mut p = build_tiny(model, 64);
        let mut out = Vec::new();
        let steps = logits.len() / n_vocab;
        let mut tok = ids[0];
        for step in 0..steps {
            step_prog(&mut p, model, &[tok], step, &mut out);
            compare_logits_for_tests(&out, &logits[step * n_vocab..(step + 1) * n_vocab], step);
            tok = if step + 1 < ids.len() { ids[step + 1] } else { tokens[step + 1 - ids.len()] };
        }
        assert_eq!(p.steps as usize, steps);
        drop(p);
        reset_for_tests();
    }

    /// Batched prefill must land on the same state as one token at a time:
    /// logits of the last row, and of the next single-token step.
    #[test]
    fn soft_gpu_batched_prefill_matches_sequential() {
        soft::reset_knobs();
        let model = load_tiny();
        let r = rec();
        let ids = r.u32s("prompt.ids").unwrap();
        let logits = r.f32s("greedy.logits").unwrap();
        let tokens = r.u32s("greedy.tokens").unwrap();
        let n_vocab = r.u32s("greedy.n_vocab").unwrap()[0] as usize;
        assert!(ids.len() >= 2, "prompt too short to batch");
        let mut p = build_tiny(model, 64);
        let mut out = Vec::new();
        step_prog(&mut p, model, &ids, 0, &mut out);
        let last = ids.len() - 1;
        compare_logits_for_tests(&out, &logits[last * n_vocab..(last + 1) * n_vocab], last);
        // Decode continues from the batch.
        step_prog(&mut p, model, &[tokens[0]], ids.len(), &mut out);
        let s = ids.len();
        compare_logits_for_tests(&out, &logits[s * n_vocab..(s + 1) * n_vocab], s);
        // A different batch size reshapes the program in place.
        drop(p);
        let mut p = build_tiny(model, 64);
        let half = ids.len() / 2;
        if half >= 2 {
            step_prog(&mut p, model, &ids[..half], 0, &mut out);
            let rest = &ids[half..];
            if rest.len() >= 2 {
                step_prog(&mut p, model, rest, half, &mut out);
                compare_logits_for_tests(&out, &logits[last * n_vocab..(last + 1) * n_vocab], last);
            }
        }
        drop(p);
        reset_for_tests();
    }

    fn open_session(model: u32) -> u32 {
        let s = crate::rust_slm_session_open(model, 64, 0, 0.0, 0, 0.0, 0);
        assert!(s >= 0);
        s as u32
    }

    fn snapshot(sid: u32) -> session::SessionSnapshot {
        session::snapshot(sid as usize).unwrap()
    }

    /// `--gpu on` end to end through the decoder: same tokens as llama.cpp,
    /// steps counted on the program, no per-op work.
    #[test]
    fn session_gpu_on_runs_the_program() {
        soft::reset_knobs();
        let model = load_tiny();
        let r = rec();
        let want = r.u32s("greedy.tokens").unwrap();
        let sid = open_session(model);
        assert_eq!(crate::rust_slm_session_configure(sid, 2, 4), 0);
        let snap = snapshot(sid);
        assert_eq!(snap.path, ForwardPath::Program);
        let got = prompt_on_session(sid, r.get("prompt.text").unwrap(), want.len() as u32);
        assert_eq!(got, want);
        let snap = snapshot(sid);
        assert_eq!(snap.path, ForwardPath::Program);
        assert!(snap.gpu_tokens >= want.len() as u64);
        assert_eq!(snap.cpu_tokens, 0);
        assert_eq!(crate::rust_slm_session_close(sid), 0);
        assert_eq!(soft::KNOBS.lock().unwrap().closes, 1, "close frees the arena");
        reset_for_tests();
    }

    /// A step that fails moves the session to the CPU with the KV cache
    /// rebuilt, so the output doesn't change, and it says why once.
    #[test]
    fn step_failure_falls_back_to_cpu_with_same_output() {
        soft::reset_knobs();
        let model = load_tiny();
        let r = rec();
        let want = r.u32s("greedy.tokens").unwrap();
        let sid = open_session(model);
        assert_eq!(crate::rust_slm_session_configure(sid, 2, 4), 0);
        // Prefill is run 0; fail two decode steps in.
        soft::KNOBS.lock().unwrap().fail_run = Some((3, -6));
        let got = prompt_on_session(sid, r.get("prompt.text").unwrap(), want.len() as u32);
        assert_eq!(got, want, "fallback changed the output");
        let snap = snapshot(sid);
        assert_eq!(snap.path, ForwardPath::Cpu);
        assert_eq!(snap.gpu_note, Some("GPU step timed out"));
        assert!(snap.gpu_tokens > 0);
        assert!(snap.cpu_tokens > 0);
        assert_eq!(soft::KNOBS.lock().unwrap().closes, 1, "the program is gone");
        assert_eq!(crate::rust_slm_session_close(sid), 0);
        reset_for_tests();
    }

    #[test]
    fn launch_modes_choose_paths() {
        soft::reset_knobs();
        let model = load_tiny();
        // Missing kernel: auto stays on the CPU and says so; on fails.
        soft::KNOBS.lock().unwrap().missing_kernel = true;
        let a = open_session(model);
        assert_eq!(crate::rust_slm_session_configure(a, 0, 4), 0);
        assert_eq!(snapshot(a).path, ForwardPath::Cpu);
        assert_eq!(snapshot(a).gpu_note, Some("a kernel is missing from the operator library"));
        assert_eq!(crate::rust_slm_session_configure(a, 2, 4), crate::SLM_ERR_GPU_PROGRAM);
        assert_eq!(crate::rust_slm_session_close(a), 0);

        // No channel at all (kexec, QEMU): auto keeps the per-op tier
        // table; on fails; cpu is cpu.
        soft::reset_knobs();
        soft::KNOBS.lock().unwrap().no_backend = true;
        let b = open_session(model);
        assert_eq!(crate::rust_slm_session_configure(b, 0, 4), 0);
        assert_eq!(snapshot(b).path, ForwardPath::PerOp);
        assert_eq!(crate::rust_slm_session_configure(b, 2, 4), crate::SLM_ERR_GPU_PROGRAM);
        assert_eq!(crate::rust_slm_session_configure(b, 1, 4), 0);
        assert_eq!(snapshot(b).path, ForwardPath::Cpu);
        assert_eq!(snapshot(b).gpu_mode, GpuMode::Cpu);
        assert_eq!(crate::rust_slm_session_close(b), 0);

        // Program available: auto uses it, and cpu after that drops it.
        soft::reset_knobs();
        let c = open_session(model);
        assert_eq!(crate::rust_slm_session_configure(c, 0, 4), 0);
        assert_eq!(snapshot(c).path, ForwardPath::Program);
        assert_eq!(crate::rust_slm_session_configure(c, 1, 4), 0);
        assert_eq!(snapshot(c).path, ForwardPath::Cpu);
        assert_eq!(soft::KNOBS.lock().unwrap().closes, 1);
        assert_eq!(crate::rust_slm_session_close(c), 0);
        reset_for_tests();
    }

    /// The harness the board runs agrees that a correct program is correct,
    /// batched or not, and points at the first wrong logit when it isn't.
    #[test]
    fn golden_harness_passes_and_catches_a_wrong_program() {
        soft::reset_knobs();
        let model = load_tiny();
        let mut rep = GoldenReport::default();
        golden_run(model, GREEDY, false, &mut rep).unwrap();
        assert_eq!(rep.first_bad_step, -1, "{rep:?}");
        assert!(rep.steps > 4);
        // Soft GPU and forward_one share the kernels; only llama.cpp's float
        // order differs, so this is far tighter than the tolerance.
        assert!(rep.max_err_milli < 500, "max_err {}", rep.max_err_milli);
        let mut rep = GoldenReport::default();
        golden_run(model, GREEDY, true, &mut rep).unwrap();
        assert_eq!(rep.first_bad_step, -1, "{rep:?}");

        // Failing the first step is reported as an error, not a pass.
        soft::KNOBS.lock().unwrap().fail_run = Some((0, -6));
        let mut rep = GoldenReport::default();
        assert_eq!(golden_run(model, GREEDY, false, &mut rep), Err("GPU step timed out"));
        soft::reset_knobs();
        reset_for_tests();
    }

    /// The soft GPU runs the same CPU kernels `forward_one` uses, so the two
    /// paths must agree closely. A wrong plan can't hide inside the 1.0
    /// tolerance used against llama.cpp.
    #[test]
    fn soft_gpu_tracks_forward_one_tightly() {
        use crate::slm::forward::{forward_one, ForwardScratch};
        use crate::slm::session::Session;
        use crate::slm::sampler::Sampler;
        soft::reset_knobs();
        let model = load_tiny();
        let r = rec();
        let ids = r.u32s("prompt.ids").unwrap();
        let mut cpu = Session::open(model, 64, Sampler::Greedy, 0).unwrap();
        cpu.path = ForwardPath::Cpu;
        let mut sc = ForwardScratch::new(&cpu.arch, cpu.max_ctx, cpu.vocab_size as usize);
        let mut p = build_tiny(model, 64);
        let mut out = Vec::new();
        for (step, &t) in ids.iter().enumerate() {
            with_loaded_slm(model as usize, |slm| forward_one(&mut cpu, slm, t, &mut sc)).unwrap().unwrap();
            step_prog(&mut p, model, &[t], step, &mut out);
            for (i, (g, w)) in out.iter().zip(sc.logits.iter()).enumerate() {
                assert!((g - w).abs() < 1e-2, "step {step} logit {i}: {g} vs {w}");
            }
        }
        drop(p);
        reset_for_tests();
    }

    /// Reset starts a new conversation on the same program: positions go
    /// back to 0 and the history empties.
    #[test]
    fn reset_restarts_the_program_at_position_zero() {
        soft::reset_knobs();
        let model = load_tiny();
        let r = rec();
        let want = r.u32s("greedy.tokens").unwrap();
        let sid = open_session(model);
        assert_eq!(crate::rust_slm_session_configure(sid, 2, 4), 0);
        let first = prompt_on_session(sid, r.get("prompt.text").unwrap(), want.len() as u32);
        assert_eq!(crate::rust_slm_session_reset(sid), 0);
        let again = prompt_on_session(sid, r.get("prompt.text").unwrap(), want.len() as u32);
        assert_eq!(first, again);
        assert_eq!(crate::rust_slm_session_close(sid), 0);
        reset_for_tests();
    }
}
