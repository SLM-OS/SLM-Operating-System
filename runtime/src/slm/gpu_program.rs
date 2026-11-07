//! The whole forward pass as one GPU command stream (#475,
//! `docs/adr/0003-slm-gpu-graph.md`).
//!
//! At `slm launch` this module turns a loaded model into a [`Plan`]: every
//! launch of a forward step, which buffer it reads and writes, and where a
//! barrier is needed. `kernel/gpu/slm_prog.c` encodes the plan into QMDs,
//! cbufs and one pushbuffer. A step then only:
//!
//! 1. writes the token ids and positions into the step page, which the
//!    embedding, RoPE and attention kernels read;
//! 2. patches the cbufs that depend on the position: K and V output
//!    pointers, RoPE on K;
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

use crate::slm::gguf::{f16_to_f32, f32_to_f16, GgmlType};
use crate::inference::gpu_slm::Tier;
use crate::slm::registry::LoadedSlm;
use crate::slm::telemetry::{OpClass, PlanFacts, OP_CLASSES};

/// Keep in step with `enum slm_prog_kernel`.
#[repr(u32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Kernel {
    RmsNorm = 0,
    Rope = 1,
    SwiGlu = 2,
    Gqa = 3,
    MmQ4k = 4,
    MmQ6k = 5,
    Add = 6,
    EmbedQ4k = 7,
    EmbedQ6k = 8,
}

/// Keep in step with `struct slm_prog_op`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ProgOp {
    pub kernel: u32,
    pub rows: u32,
    pub dim: [u32; 4],
    pub ptr: [u64; 5],
    pub scal: [u32; 2],
}

const _: () = assert!(core::mem::size_of::<ProgOp>() == 72);

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
/// Run matmuls on the SIMT kernels (plan decision M6-D4's reference).
pub const FLAG_SIMT: u32 = 4;
/// The decode program waits on a semaphore nobody releases.
pub const FLAG_INJECT_HANG: u32 = 8;
/// The first decode op writes to an unmapped GPU address: an MMU fault.
pub const FLAG_INJECT_FAULT: u32 = 16;
const UNMAPPED_VA: u64 = 0x1F_0000_0000;

const MM_OUT_F16: u32 = 1;
const MM_SIMT: u32 = 1;
/// Agreement with llama.cpp on `tiny_qwen2`: llama.cpp quantizes activations
/// to Q8_K, which the kernels don't, and that accounts for up to about 1.43.
pub const GOLDEN_LLAMA_TOLERANCE: f32 = 1.5;
/// Agreement with the CPU reference using exact dot products.
pub const GOLDEN_EXACT_TOLERANCE: f32 = 0.1;
const RMS_EPS: f32 = 1e-6;
/// Keep in step with `slm_prog_encode` and `Q4K_TILE_ROWS` in the kernels.
const MM_TILE_ROWS: u64 = 15;

extern "C" {
    fn slm_prog_available() -> i32;
    fn slm_prog_open(a: *const OpenArgs, out: *mut OpenOut) -> i32;
    fn slm_prog_close(h: i32);
    fn slm_prog_add(h: i32, shape: u32, op: *const ProgOp) -> i32;
    fn slm_prog_barrier(h: i32, shape: u32) -> i32;
    fn slm_prog_mark(h: i32, shape: u32) -> i32;
    fn slm_prog_finish(h: i32, shape: u32) -> i32;
    fn slm_prog_patch(h: i32, shape: u32, idx: u32, op: *const ProgOp) -> i32;
    fn slm_prog_update(h: i32, shape: u32, idx: u32, op: *const ProgOp) -> i32;
    fn slm_prog_clean(h: i32, p: *const u8, len: usize);
    fn slm_prog_invalidate(h: i32, p: *const u8, len: usize);
    fn slm_prog_run(h: i32, shape: u32, stats: *mut RunStats) -> i32;
    fn slm_prog_run_marked(h: i32, shape: u32, stats: *mut RunStats, ns: *mut u64, cap: u32) -> i32;
    fn slm_prog_max_batch(simt: bool) -> u32;
    fn slm_prog_profile_ns(h: i32, shape: u32, idx: u32) -> u64;
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
    pub embed: Matmul,
    /// File offsets of the FP32 gammas: per layer attention, feed-forward.
    pub norms: Vec<(u64, u64)>,
    pub out_norm: u64,
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
        // The other kernels take whole warps of 32.
        if dims.head_dim > 256 || dims.head_dim % 32 != 0 {
            return Err("head_dim is not a multiple of 32 up to 256");
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
        let norm = |n: &str| -> Result<u64, &'static str> {
            let b = slm.tensor_bytes(n).ok_or("a norm weight is missing")?;
            if b.len() != dims.hidden as usize * 4 {
                return Err("a norm weight has the wrong size");
            }
            Ok(offset(b))
        };
        let mut layers = Vec::with_capacity(dims.layers as usize);
        let mut norms = Vec::with_capacity(dims.layers as usize);
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
            let mut name = String::new();
            let _ = write!(name, "blk.{}.attn_norm.weight", l);
            let attn_norm = norm(&name)?;
            name.clear();
            let _ = write!(name, "blk.{}.ffn_norm.weight", l);
            let ffn_norm = norm(&name)?;
            norms.push((attn_norm, ffn_norm));
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
        let e = mm("token_embd.weight", None)?;
        let embed = Matmul {
            kernel: if e.kernel == Kernel::MmQ4k { Kernel::EmbedQ4k } else { Kernel::EmbedQ6k },
            ..e
        };
        Ok(Self {
            dims,
            layers,
            lm_head: mm(lm_name, None)?,
            embed,
            norms,
            out_norm: norm("output_norm.weight")?,
        })
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
    pub rope: u64,
    pub kv_k: u64,
    pub kv_v: u64,
    pub logits: u64,
    pub data_bytes: u64,
    pub step_ids: u64,
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
        let rope = take(max_ctx.checked_mul(d.head_dim as u64)?.checked_mul(FP32)?)?;
        let kv_k = take(per_layer.checked_mul(layers)?)?;
        let kv_v = take(per_layer.checked_mul(layers)?)?;
        let logits = take((d.vocab as u64).checked_mul(FP32)?)?;
        let data_bytes = at;

        let step_ids = 0u64;
        let step_positions = align256(batch.checked_mul(4)?);
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
            rope,
            kv_k,
            kv_v,
            logits,
            data_bytes,
            step_ids,
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
}

/// Logical buffer ids for dependency tracking.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Buf {
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
    pub ptr: [Ref; 5],
    pub scal: [u32; 2],
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
        }
    }

    pub fn materialize(&self, b: &Bases, s: &StepArgs) -> ProgOp {
        ProgOp {
            kernel: self.kernel as u32,
            rows: if self.batched { s.rows } else { 1 },
            dim: self.dim,
            ptr: [
                Self::resolve(self.ptr[0], b, s),
                Self::resolve(self.ptr[1], b, s),
                Self::resolve(self.ptr[2], b, s),
                Self::resolve(self.ptr[3], b, s),
                Self::resolve(self.ptr[4], b, s),
            ],
            scal: self.scal,
        }
    }

    /// Does a step have to rewrite this op's cbuf at a fixed batch size?
    pub fn depends_on_pos(&self) -> bool {
        self.ptr.iter().any(|r| matches!(r, Ref::DataAtPos { .. }))
    }
}

/// Everything a program does, in order. `barrier_before[i]` says a barrier
/// precedes op `i`.
#[derive(Clone, Debug)]
pub struct Plan {
    pub ops: Vec<PlanOp>,
    /// Each op's class, for `slm stats`.
    pub class: Vec<OpClass>,
    pub barrier_before: Vec<bool>,
    /// Indices of ops with position-dependent cbufs.
    pub patch: Vec<u32>,
    pub batched: bool,
}

struct PlanBuilder {
    plan: Plan,
    /// Class of the matmuls being pushed.
    mm_class: OpClass,
    written: Vec<Buf>,
    read: Vec<Buf>,
}

impl PlanBuilder {
    fn new(batched: bool) -> Self {
        Self {
            plan: Plan { ops: Vec::new(), class: Vec::new(), barrier_before: Vec::new(), patch: Vec::new(), batched },
            mm_class: OpClass::MatmulAttn,
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
        self.plan.class.push(match op.kernel {
            Kernel::RmsNorm => OpClass::RmsNorm,
            Kernel::Rope => OpClass::Rope,
            Kernel::SwiGlu => OpClass::SwiGlu,
            Kernel::Gqa => OpClass::Gqa,
            Kernel::MmQ4k | Kernel::MmQ6k => self.mm_class,
            Kernel::Add => OpClass::Add,
            Kernel::EmbedQ4k | Kernel::EmbedQ6k => OpClass::Embed,
        });
        self.plan.ops.push(op);
    }
}

struct PlanCtx {
    simt: u32,
}

fn mm_op(c: &PlanCtx, m: &Matmul, x: Ref, out: Ref, k: u32, n: u32, reads: Buf, writes: Buf, f16: bool, batched: bool) -> PlanOp {
    PlanOp {
        kernel: m.kernel,
        batched,
        dim: [k, n, 0, 0],
        ptr: [
            x,
            Ref::Weights(m.w_off),
            out,
            m.bias_off.map(Ref::Weights).unwrap_or(Ref::Null),
            Ref::Null,
        ],
        scal: [if f16 { MM_OUT_F16 } else { 0 }, c.simt],
        reads: alloc::vec![reads],
        writes: alloc::vec![writes],
    }
}

/// Build the launches for one program shape: `batched` covers prefill (one
/// row per token, causal through the kernels' per-row positions), otherwise
/// a single row. The ops are the same; only the rows differ.
pub fn build_plan(d: &ModelDesc, a: &Arena, batched: bool, simt: bool) -> Plan {
    let dims = &d.dims;
    let hidden = dims.hidden;
    let inter = dims.intermediate;
    let nqhd = dims.n_head * dims.head_dim;
    let nkvhd = dims.n_head_kv * dims.head_dim;
    let eps = RMS_EPS.to_bits();
    let hidden_row = hidden as u64 * FP16;
    let c = PlanCtx { simt: if simt { MM_SIMT } else { 0 } };
    let mut pb = PlanBuilder::new(batched);

    let norm = |x: Ref, gamma: u64, out: Ref, reads: Buf, writes: Buf, batched: bool| PlanOp {
        kernel: Kernel::RmsNorm,
        batched,
        dim: [hidden, 0, 0, 0],
        ptr: [x, Ref::Weights(gamma), out, Ref::Null, Ref::Null],
        scal: [eps, 0],
        reads: alloc::vec![reads],
        writes: alloc::vec![writes],
    };
    let add = |y: u64, other: Buf| PlanOp {
        kernel: Kernel::Add,
        batched: true,
        dim: [hidden, 0, 0, 0],
        ptr: [Ref::Data(a.x), Ref::Data(y), Ref::Data(a.x), Ref::Null, Ref::Null],
        scal: [0, 0],
        reads: alloc::vec![Buf::X, other],
        writes: alloc::vec![Buf::X],
    };
    let pos = Ref::Step(a.step_positions);
    let rope = |vec: Ref, heads: u32, buf: Buf| PlanOp {
        kernel: Kernel::Rope,
        batched: true,
        dim: [heads, dims.head_dim, 0, 0],
        ptr: [vec, vec, Ref::Data(a.rope), pos, Ref::Null],
        scal: [0, 0],
        reads: alloc::vec![buf],
        writes: alloc::vec![buf],
    };

    pb.push(PlanOp {
        kernel: d.embed.kernel,
        batched: true,
        dim: [hidden, 0, 0, 0],
        ptr: [Ref::Step(a.step_ids), Ref::Weights(d.embed.w_off), Ref::Data(a.x), Ref::Null, Ref::Null],
        scal: [0, 0],
        reads: Vec::new(),
        writes: alloc::vec![Buf::X],
    });
    for (i, l) in d.layers.iter().enumerate() {
        let li = i as u32;
        let kv_k = a.kv_k + i as u64 * a.kv_per_layer;
        let kv_v = a.kv_v + i as u64 * a.kv_per_layer;
        let k_at = Ref::DataAtPos { off: kv_k, stride: a.kv_per_pos };
        let v_at = Ref::DataAtPos { off: kv_v, stride: a.kv_per_pos };
        let (attn_norm, ffn_norm) = d.norms[i];

        pb.mm_class = OpClass::MatmulAttn;
        pb.push(norm(Ref::Data(a.x), attn_norm, Ref::Data(a.xn), Buf::X, Buf::Xn, true));
        pb.push(mm_op(&c, &l.q, Ref::Data(a.xn), Ref::Data(a.q), hidden, nqhd, Buf::Xn, Buf::Q, true, true));
        pb.push(mm_op(&c, &l.k, Ref::Data(a.xn), k_at, hidden, nkvhd, Buf::Xn, Buf::KvK(li), true, true));
        pb.push(mm_op(&c, &l.v, Ref::Data(a.xn), v_at, hidden, nkvhd, Buf::Xn, Buf::KvV(li), true, true));
        pb.push(rope(Ref::Data(a.q), dims.n_head, Buf::Q));
        pb.push(rope(k_at, dims.n_head_kv, Buf::KvK(li)));
        pb.push(PlanOp {
            kernel: Kernel::Gqa,
            batched: true,
            dim: [dims.n_head, dims.n_head_kv, dims.head_dim, 0],
            ptr: [Ref::Data(a.q), Ref::Data(kv_k), Ref::Data(kv_v), pos, Ref::Data(a.attn)],
            scal: [0, 0],
            reads: alloc::vec![Buf::Q, Buf::KvK(li), Buf::KvV(li)],
            writes: alloc::vec![Buf::Attn],
        });
        pb.push(mm_op(&c, &l.o, Ref::Data(a.attn), Ref::Data(a.o), nqhd, hidden, Buf::Attn, Buf::O, true, true));
        pb.push(add(a.o, Buf::O));
        pb.mm_class = OpClass::MatmulMlp;
        pb.push(norm(Ref::Data(a.x), ffn_norm, Ref::Data(a.xn), Buf::X, Buf::Xn, true));
        pb.push(mm_op(&c, &l.gate, Ref::Data(a.xn), Ref::Data(a.gate), hidden, inter, Buf::Xn, Buf::Gate, true, true));
        pb.push(mm_op(&c, &l.up, Ref::Data(a.xn), Ref::Data(a.up), hidden, inter, Buf::Xn, Buf::Up, true, true));
        pb.push(PlanOp {
            kernel: Kernel::SwiGlu,
            batched: true,
            dim: [inter, 0, 0, 0],
            ptr: [Ref::Data(a.gate), Ref::Data(a.up), Ref::Data(a.silu), Ref::Null, Ref::Null],
            scal: [0, 0],
            reads: alloc::vec![Buf::Gate, Buf::Up],
            writes: alloc::vec![Buf::Silu],
        });
        pb.push(mm_op(&c, &l.down, Ref::Data(a.silu), Ref::Data(a.m), inter, hidden, Buf::Silu, Buf::M, true, true));
        pb.push(add(a.m, Buf::M));
    }
    // Only the last position produces logits.
    pb.push(norm(
        Ref::DataLastRow { off: a.x, stride: hidden_row },
        d.out_norm,
        Ref::Data(a.xn),
        Buf::X,
        Buf::Xn,
        false,
    ));
    pb.mm_class = OpClass::LmHead;
    pb.push(mm_op(&c, &d.lm_head, Ref::Data(a.xn), Ref::Data(a.logits), hidden, dims.vocab, Buf::Xn, Buf::Logits, false, false));
    pb.plan
}

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------

/// `GA10B_PROG_BARRIER_WFI`.
const BARRIER_WFI: u32 = 2;

/// Barrier kinds and QMD options (`slm gpu opts` changes them to measure).
/// WAIT_FOR_IDLE with lean QMDs: the QMD already invalidates shader data, the
/// extra flush in the barrier added nothing on jetson-nano-2, and lean QMDs
/// save about 2 us per launch with identical p1 output.
static OPT_BARRIER: core::sync::atomic::AtomicU32 = core::sync::atomic::AtomicU32::new(BARRIER_WFI);
static OPT_FLAGS: core::sync::atomic::AtomicU32 = core::sync::atomic::AtomicU32::new(FLAG_LEAN_QMD);

/// `slm gpu inject`: the next launch's decode program fails on purpose.
/// 0 none, 1 a hang on a semaphore, 2 an MMU fault.
pub fn set_inject(kind: u32) {
    let mut f = OPT_FLAGS.load(core::sync::atomic::Ordering::Relaxed);
    f &= !(FLAG_INJECT_HANG | FLAG_INJECT_FAULT);
    f |= match kind {
        1 => FLAG_INJECT_HANG,
        2 => FLAG_INJECT_FAULT,
        _ => 0,
    };
    OPT_FLAGS.store(f, core::sync::atomic::Ordering::Relaxed);
}

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
    pub steps: u64,
    pub tokens: u64,
    decode_steps: u64,
    decode_gpu_ns: u64,
    decode_wait_us: u64,
    decode_evict_us: u64,
    decode_wall_ns: u64,
    prefill_last_gpu_ns: u64,
    prefill_last_rows: u32,
    pub gpu_ns_total: u64,
    pub wait_us_total: u64,
    pub evict_us_total: u64,
    pub weights_big: bool,
    pub weights_map_us: u64,
    pub table_pages_added: u32,
    pub launches: u32,
    pub barriers: u32,
    pub data_bytes: u64,
    /// Run decode steps on the marked pushbuffer, from `slm stats --profile`.
    pub profile: bool,
    /// The last step's figures, for the caller's telemetry.
    pub last_stats: RunStats,
    pub last_marked: bool,
    pub last_class_ns: [u64; OP_CLASSES],
    /// The class each interval between marks belongs to, last one included.
    mark_class: Vec<OpClass>,
    /// Filled by a marked run. Sized at build: a step doesn't allocate.
    mark_ns: Vec<u64>,
    simt: bool,
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
        let desc = ModelDesc::from_slm(slm, vocab).map_err(BuildError::Failed)?;
        let mut flags = OPT_FLAGS.load(core::sync::atomic::Ordering::Relaxed);
        let simt = flags & FLAG_SIMT != 0;
        let inject_fault = flags & FLAG_INJECT_FAULT != 0;
        flags &= FLAG_LEAN_QMD | FLAG_PROFILE | FLAG_INJECT_HANG;
        // SAFETY: plain FFI query.
        let max_batch = unsafe { slm_prog_max_batch(simt) };
        if max_batch == 0 {
            return Err(BuildError::Failed(err_message(ERR_NO_KERNEL)));
        }
        let arena = Arena::compute(&desc.dims, max_ctx as u64, max_batch as u64)
            .ok_or(BuildError::Failed("session arena does not fit"))?;
        let decode = build_plan(&desc, &arena, false, simt);
        let prefill = (max_batch > 1).then(|| build_plan(&desc, &arena, true, simt));

        let weights = slm.weight_bytes().ok_or(BuildError::Failed("model has no weight bytes"))?;
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
            steps: 0,
            tokens: 0,
            decode_steps: 0,
            decode_gpu_ns: 0,
            decode_wait_us: 0,
            decode_evict_us: 0,
            decode_wall_ns: 0,
            prefill_last_gpu_ns: 0,
            prefill_last_rows: 0,
            gpu_ns_total: 0,
            wait_us_total: 0,
            evict_us_total: 0,
            weights_big: out.weights_big != 0,
            weights_map_us: out.weights_map_us,
            table_pages_added: out.table_pages_added,
            launches: 0,
            barriers: 0,
            data_bytes: arena.data_bytes,
            profile: false,
            last_stats: RunStats::default(),
            last_marked: false,
            last_class_ns: [0; OP_CLASSES],
            mark_class: Vec::new(),
            mark_ns: Vec::new(),
            simt,
        });
        // From here on, Drop closes the session.
        p.fill_constants(rope_table).map_err(BuildError::Failed)?;
        p.emit(SHAPE_DECODE).map_err(|rc| BuildError::Failed(err_message(rc)))?;
        if p.prefill.is_some() {
            p.emit(SHAPE_PREFILL).map_err(|rc| BuildError::Failed(err_message(rc)))?;
        }
        if inject_fault {
            let s = StepArgs { pos: 0, rows: 1 };
            let mut op = p.decode.ops[1].materialize(&p.bases, &s);
            op.ptr[2] = UNMAPPED_VA;
            // SAFETY: the handle is open and `op` outlives the call.
            let rc = unsafe { slm_prog_patch(p.h, SHAPE_DECODE, 1, &op) };
            if rc < 0 {
                return Err(BuildError::Failed(err_message(rc)));
            }
        }
        Ok(p)
    }

    pub fn dims(&self) -> Dims {
        self.dims
    }

    /// The RoPE table, then clean. The layout comes from [`Arena`].
    fn fill_constants(&mut self, rope_table: &[f32]) -> Result<(), &'static str> {
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
            slm_prog_clean(self.h, (self.data_cpu + self.arena.rope as usize) as *const u8, rope_bytes);
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
        let mut mark_class = Vec::new();
        let mut cur = plan.class.first().copied().unwrap_or(OpClass::Embed);
        for (i, (op, &bar)) in plan.ops.iter().zip(plan.barrier_before.iter()).enumerate() {
            if bar {
                // SAFETY: the handle is ours and open.
                let rc = unsafe { slm_prog_barrier(h, shape) };
                if rc < 0 {
                    return Err(rc);
                }
                barriers += 1;
                // Op-class boundaries with a barrier get a timestamp in the
                // marked pushbuffer. Barriers are the only places that time
                // means something: launches between them overlap.
                if shape == SHAPE_DECODE && plan.class[i] != cur {
                    // SAFETY: as above.
                    let rc = unsafe { slm_prog_mark(h, shape) };
                    if rc < 0 {
                        return Err(rc);
                    }
                    mark_class.push(cur);
                    cur = plan.class[i];
                }
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
            mark_class.push(cur);
            self.mark_ns = alloc::vec![0; mark_class.len()];
            self.mark_class = mark_class;
        } else {
            self.prefill_rows = self.max_batch;
        }
        Ok(())
    }

    /// Write `tokens` and their positions into the step page.
    fn stage_tokens(&mut self, tokens: &[u32], pos: usize) -> Result<(), &'static str> {
        if tokens.is_empty() || tokens.len() > self.max_batch as usize {
            return Err("batch larger than the program");
        }
        if tokens.iter().any(|&t| t >= self.dims.vocab) {
            return Err("token id out of range");
        }
        let ids = (self.step_cpu + self.arena.step_ids as usize) as *mut u32;
        let poss = (self.step_cpu + self.arena.step_positions as usize) as *mut u32;
        for (r, &t) in tokens.iter().enumerate() {
            // SAFETY: both arrays hold max_batch u32s, aligned to 256, and only
            // this task writes them between steps.
            unsafe {
                ids.add(r).write(t);
                poss.add(r).write((pos + r) as u32);
            }
        }
        // SAFETY: the range is the step page.
        unsafe {
            slm_prog_clean(self.h, self.step_cpu as *const u8, self.arena.step_bytes as usize);
        }
        Ok(())
    }

    /// One step: `tokens` at positions `pos..`. `out` receives the last row's
    /// logits. Waits with IRQs on, so no spinlock may be held across this
    /// call.
    pub fn run(&mut self, tokens: &[u32], pos: usize, out: &mut Vec<f32>) -> Result<(), &'static str> {
        let t0 = crate::kernel_ffi::get_time_ns();
        self.stage_tokens(tokens, pos)?;
        let rows = tokens.len() as u32;
        let shape = if rows > 1 { SHAPE_PREFILL } else { SHAPE_DECODE };
        if rows > 1 && self.prefill.is_none() {
            return Err("batched step without a prefill program");
        }
        let s = StepArgs { pos: pos as u32, rows };
        self.patch_step(shape, &s).map_err(err_message)?;

        let mut stats = RunStats::default();
        let marked = self.profile && shape == SHAPE_DECODE && !self.mark_class.is_empty();
        // SAFETY: the handle is ours; stats and mark_ns outlive the call.
        let rc = unsafe {
            if marked {
                let cap = self.mark_ns.len() as u32;
                slm_prog_run_marked(self.h, shape, &mut stats, self.mark_ns.as_mut_ptr(), cap)
            } else {
                slm_prog_run(self.h, shape, &mut stats)
            }
        };
        if rc != 0 {
            return Err(err_message(rc));
        }
        self.last_stats = stats;
        self.last_marked = marked;
        if marked {
            self.last_class_ns = [0; OP_CLASSES];
            for (class, ns) in self.mark_class.iter().zip(self.mark_ns.iter()) {
                self.last_class_ns[*class as usize] += *ns;
            }
        }
        self.read_logits(out);
        if shape == SHAPE_DECODE {
            self.decode_steps += 1;
            self.decode_gpu_ns = self.decode_gpu_ns.wrapping_add(stats.gpu_ns);
            self.decode_wait_us = self.decode_wait_us.wrapping_add(stats.wait_us);
            self.decode_evict_us = self.decode_evict_us.wrapping_add(stats.evict_us);
            let t = crate::kernel_ffi::get_time_ns().wrapping_sub(t0);
            self.decode_wall_ns = self.decode_wall_ns.wrapping_add(t);
        } else {
            self.prefill_last_gpu_ns = stats.gpu_ns;
            self.prefill_last_rows = rows;
        }
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

    /// The same step on the CPU over `mem`, with this program's VAs: what the
    /// GPU should have produced.
    fn reference_step(&self, mem: &reference::Mem, tokens: &[u32], pos: usize, out: &mut Vec<f32>) {
        let rows = tokens.len() as u32;
        let shape = if rows > 1 { SHAPE_PREFILL } else { SHAPE_DECODE };
        let s = StepArgs { pos: pos as u32, rows };
        for (r, &t) in tokens.iter().enumerate() {
            // SAFETY: the step buffer has the arena's layout.
            unsafe {
                ((mem.step + self.arena.step_ids as usize) as *mut u32).add(r).write(t);
                ((mem.step + self.arena.step_positions as usize) as *mut u32).add(r).write((pos + r) as u32);
            }
        }
        for op in &self.plan(shape).ops {
            reference::exec(mem, &op.materialize(&self.bases, &s));
        }
        let n = self.dims.vocab as usize;
        out.clear();
        // SAFETY: the data buffer has the arena's layout.
        out.extend_from_slice(unsafe {
            core::slice::from_raw_parts((mem.data + self.arena.logits as usize) as *const f32, n)
        });
    }

    /// Time per decode step, and with `slm gpu opts ... profile` what each
    /// kernel took in the last one, to the console.
    pub fn print_profile(&self) {
        let n = self.decode_steps.max(1);
        let mut line = String::new();
        let _ = write!(
            line,
            "[slm] decode {} steps: gpu {} us, wait {} us, evict {} us, whole step {} us; last prefill {} rows gpu {} us\n\0",
            self.decode_steps,
            self.decode_gpu_ns / 1000 / n,
            self.decode_wait_us / n,
            self.decode_evict_us / n,
            self.decode_wall_ns / 1000 / n,
            self.prefill_last_rows,
            self.prefill_last_gpu_ns / 1000
        );
        crate::kernel_ffi::print(line.as_bytes());
        // One line per (kernel, output width), summed over the layers.
        let mut rows: Vec<(Kernel, u32, u32, u64)> = Vec::new();
        for (i, op) in self.decode.ops.iter().enumerate() {
            // SAFETY: the handle is open; returns 0 unless opened with profile.
            let ns = unsafe { slm_prog_profile_ns(self.h, SHAPE_DECODE, i as u32) };
            let w = if matches!(op.kernel, Kernel::MmQ4k | Kernel::MmQ6k) { op.dim[1] } else { 0 };
            match rows.iter_mut().find(|r| r.0 == op.kernel && r.1 == w) {
                Some(r) => {
                    r.2 += 1;
                    r.3 += ns;
                }
                None => rows.push((op.kernel, w, 1, ns)),
            }
        }
        for (k, w, count, ns) in rows {
            let mut l = String::new();
            let _ = write!(l, "[slm]   {k:?} N={w}: {count} launches, {} us\n\0", ns / 1000);
            crate::kernel_ffi::print(l.as_bytes());
        }
    }

    /// What a decode token runs, for `slm stats --gpu`.
    pub fn plan_facts(&self) -> PlanFacts {
        let mut f = PlanFacts {
            launches: self.launches,
            barriers: self.barriers,
            marks: self.mark_class.len().saturating_sub(1) as u32,
            arena_bytes: self.data_bytes,
            ..PlanFacts::default()
        };
        f.class_tier = [Tier::Simt as u32; OP_CLASSES];
        for (op, class) in self.decode.ops.iter().zip(self.decode.class.iter()) {
            f.class_launches[*class as usize] += 1;
            let block_bytes = match op.kernel {
                Kernel::MmQ4k => 144u64,
                Kernel::MmQ6k => 210,
                _ => continue,
            };
            let (k, n) = (op.dim[0] as u64, op.dim[1] as u64);
            let blocks = k / 256;
            f.weight_bytes += blocks * block_bytes * n;
            if self.simt {
                f.macs_simt += k * n;
            } else {
                f.class_tier[*class as usize] = Tier::Hmma as u32;
                f.macs_tc += k * n;
                // scripts/cuda/harness/slm_forward.cu: 16 mma per weight block
                // per tile, one row. A tile holds MM_TILE_ROWS weight rows.
                f.mma += n.div_ceil(MM_TILE_ROWS) * blocks * 16;
            }
        }
        let d = &self.dims;
        let (l, nq, nkv, hd) = (d.layers as u64, d.n_head as u64, d.n_head_kv as u64, d.head_dim as u64);
        f.gqa_macs_per_pos = 2 * l * nq * hd;
        f.kv_bytes_per_pos = 2 * l * nkv * hd * FP16;
        f
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
// Smoke (`slm gpu smoke`)
// ---------------------------------------------------------------------------

/// Keep in step with `SlmGpuSmokeReport` in slm_ffi.h.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct SmokeReport {
    pub launches: u32,
    /// Elements off by more than 1%, compared with the CPU's result.
    pub bad_elements: u32,
    pub gpu_ns: u64,
    pub wait_us: u64,
    pub evict_us: u64,
}

/// A chain of `n` dependent RMSNorm launches over one program: x -> y -> x.
/// Needs only a kernel that ships today, so it checks the launch, barrier,
/// semaphore and evict machinery without any model. Each launch's input is
/// the previous one's output, so a missing barrier shows up in the result.
///
/// `width` is the row length, a multiple of 32: a small one leaves mostly the
/// launch and barrier cost in the per-launch time.
pub fn smoke(n: u32, width: u32, rep: &mut SmokeReport) -> Result<(), &'static str> {
    #[allow(non_snake_case)]
    let N = width as usize;
    if n == 0 {
        return Err("no launches");
    }
    if N == 0 || N % 32 != 0 {
        return Err("width is not a multiple of 32");
    }
    let data_bytes = (2 * N * 2 + N * 4 + 4096) as u64;
    let args = OpenArgs {
        weights: core::ptr::null(),
        weights_bytes: 0,
        data_bytes,
        step_bytes: 256,
        max_launches: [n, 0],
        flags: OPT_FLAGS.load(core::sync::atomic::Ordering::Relaxed) & (FLAG_LEAN_QMD | FLAG_PROFILE),
        barrier: OPT_BARRIER.load(core::sync::atomic::Ordering::Relaxed),
    };
    let mut out = OpenOut::default();
    // SAFETY: both pointers are to live locals of the declared types.
    let rc = unsafe { slm_prog_open(&args, &mut out) };
    if rc != 0 {
        return Err(err_message(rc));
    }
    struct Close(i32);
    impl Drop for Close {
        fn drop(&mut self) {
            // SAFETY: opened above, closed once.
            unsafe { slm_prog_close(self.0) };
        }
    }
    let h = out.handle;
    let _close = Close(h);

    let mut x = alloc::vec![0u16; N];
    let mut gamma = alloc::vec![0u16; N];
    let mut gamma32 = alloc::vec![0f32; N];
    for i in 0..N {
        x[i] = f32_to_f16(((i % 97) as f32 - 48.0) / 16.0);
        // Exact in FP16, so the kernel's F32 gamma and the reference agree.
        gamma32[i] = 1.0 + (i % 5) as f32 / 8.0;
        gamma[i] = f32_to_f16(gamma32[i]);
    }
    let (a_off, b_off, g_off) = (0usize, N * 2, 2 * N * 2);
    // SAFETY: the data region holds data_bytes, more than these three rows.
    unsafe {
        core::ptr::copy_nonoverlapping(x.as_ptr() as *const u8, (out.data_cpu + a_off) as *mut u8, N * 2);
        core::ptr::copy_nonoverlapping(gamma32.as_ptr() as *const u8, (out.data_cpu + g_off) as *mut u8, N * 4);
        slm_prog_clean(h, out.data_cpu as *const u8, data_bytes as usize);
    }
    for i in 0..n {
        let (from, to) = if i % 2 == 0 { (a_off, b_off) } else { (b_off, a_off) };
        let op = ProgOp {
            kernel: Kernel::RmsNorm as u32,
            rows: 1,
            dim: [N as u32, 0, 0, 0],
            ptr: [
                out.data_gpu_va + from as u64,
                out.data_gpu_va + g_off as u64,
                out.data_gpu_va + to as u64,
                0,
                0,
            ],
            scal: [RMS_EPS.to_bits(), 0],
        };
        if i > 0 {
            // SAFETY: the handle is open.
            let rc = unsafe { slm_prog_barrier(h, 0) };
            if rc < 0 {
                return Err(err_message(rc));
            }
        }
        // SAFETY: `op` outlives the call.
        let rc = unsafe { slm_prog_add(h, 0, &op) };
        if rc < 0 {
            return Err(err_message(rc));
        }
    }
    // SAFETY: the handle is open.
    let rc = unsafe { slm_prog_finish(h, 0) };
    if rc < 0 {
        return Err(err_message(rc));
    }
    let mut st = RunStats::default();
    // SAFETY: as above; `st` outlives the call.
    let rc = unsafe { slm_prog_run(h, 0, &mut st) };
    if rc != 0 {
        return Err(err_message(rc));
    }

    let result_off = if n % 2 == 1 { b_off } else { a_off };
    let p = (out.data_cpu + result_off) as *const u8;
    // SAFETY: the row is in the data region and the GPU has finished.
    let got = unsafe {
        slm_prog_invalidate(h, p, N * 2);
        core::slice::from_raw_parts(p as *const u16, N)
    };
    // CPU reference: the same chain.
    let mut cur = x;
    let mut next = alloc::vec![0u16; N];
    for _ in 0..n {
        crate::inference::ops_transformer::rmsnorm(&cur, &gamma, RMS_EPS, &mut next)
            .ok_or("reference rmsnorm failed")?;
        core::mem::swap(&mut cur, &mut next);
    }
    let bad = got
        .iter()
        .zip(cur.iter())
        .filter(|(g, c)| {
            let (gv, cv) = (f16_to_f32(**g), f16_to_f32(**c));
            !((gv - cv).abs() <= 0.01 * cv.abs() + 1e-3)
        })
        .count();
    rep.launches = n;
    rep.bad_elements = bad as u32;
    rep.gpu_ns = st.gpu_ns;
    rep.wait_us = st.wait_us;
    rep.evict_us = st.evict_us;
    Ok(())
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
    /// First step with a logit off by more than [`GOLDEN_LLAMA_TOLERANCE`], or -1.
    pub first_bad_step: i32,
    pub first_bad_index: i32,
    /// Thousandths, since kernel C has no floating point.
    pub first_bad_got_milli: i32,
    pub first_bad_want_milli: i32,
    /// Largest error over all steps, in thousandths.
    pub max_err_milli: i32,
    /// Steps where the top logit is the token llama.cpp's top logit is.
    pub argmax_matches: u32,
    /// First step over [`GOLDEN_EXACT_TOLERANCE`] from the exact reference, or -1.
    pub exact_first_bad_step: i32,
    /// Largest error against the exact reference, in thousandths.
    pub exact_max_err_milli: i32,
    pub gpu_us_per_token: u64,
}

/// Run the fixture's prompt and greedy continuation on a program and compare
/// every step's logits with llama.cpp's. With `batched`, the prompt goes in
/// as one prefill step, so only its last row has a reference.
pub fn golden_run(model: u32, rec: &[u8], batched: bool, rep: &mut GoldenReport) -> Result<(), &'static str> {
    use crate::slm::golden::Records;
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

    // The exact CPU reference, in memory of its own with the arena's layout.
    let weights = crate::slm::registry::with_loaded_slm(model as usize, |slm| {
        slm.weight_bytes().map(|b| (b.as_ptr() as usize, b.len()))
    })
    .flatten()
    .ok_or("model has no weight bytes")?;
    let mut ref_data = alloc::vec![0u64; prog.arena.data_bytes as usize / 8 + 1];
    let mut ref_step = alloc::vec![0u64; prog.arena.step_bytes as usize / 8 + 1];
    let mem = reference::Mem {
        weights_va: prog.bases.weights,
        weights: weights.0,
        weights_len: weights.1,
        data_va: prog.bases.data,
        data: ref_data.as_mut_ptr() as usize,
        data_len: prog.arena.data_bytes as usize,
        step_va: prog.bases.step,
        step: ref_step.as_mut_ptr() as usize,
        step_len: prog.arena.step_bytes as usize,
        exact: true,
    };
    // SAFETY: the arena reserves room for the table at `rope`.
    unsafe {
        core::ptr::copy_nonoverlapping(
            rope.cos_sin.as_ptr() as *const u8,
            (mem.data + prog.arena.rope as usize) as *mut u8,
            rope.cos_sin.len() * 4,
        );
    }

    let steps = logits.len() / n_vocab;
    let mut out = Vec::new();
    let mut exact = Vec::new();
    rep.first_bad_step = -1;
    rep.first_bad_index = -1;
    rep.exact_first_bad_step = -1;
    rep.max_err_milli = 0;
    rep.exact_max_err_milli = 0;
    rep.argmax_matches = 0;
    rep.steps = 0;
    let (mut max_err, mut exact_max) = (0.0f32, 0.0f32);
    let mut step = 0usize;
    let mut compare = |step: usize, out: &[f32], exact: &[f32], rep: &mut GoldenReport| {
        let want = &logits[step * n_vocab..(step + 1) * n_vocab];
        let top = |v: &[f32]| {
            let mut b = 0usize;
            for i in 0..v.len() {
                if v[i] > v[b] {
                    b = i;
                }
            }
            b
        };
        if top(out) == top(want) {
            rep.argmax_matches += 1;
        }
        let (mut step_max, mut step_exact) = (0.0f32, 0.0f32);
        for i in 0..n_vocab {
            let e = (out[i] - want[i]).abs();
            if !(e <= step_max) {
                step_max = e;
            }
            if !(e <= GOLDEN_LLAMA_TOLERANCE) && rep.first_bad_step < 0 {
                rep.first_bad_step = step as i32;
                rep.first_bad_index = i as i32;
                rep.first_bad_got_milli = (out[i] * 1000.0) as i32;
                rep.first_bad_want_milli = (want[i] * 1000.0) as i32;
            }
            let x = (out[i] - exact[i]).abs();
            if !(x <= step_exact) {
                step_exact = x;
            }
            if !(x <= GOLDEN_EXACT_TOLERANCE) && rep.exact_first_bad_step < 0 {
                rep.exact_first_bad_step = step as i32;
            }
        }
        max_err = max_err.max(step_max);
        exact_max = exact_max.max(step_exact);
        rep.max_err_milli = (max_err * 1000.0) as i32;
        rep.exact_max_err_milli = (exact_max * 1000.0) as i32;
        rep.steps += 1;
        let mut line = String::new();
        let _ = write!(
            line,
            "[slm] golden step {step}: vs llama.cpp {} milli, vs exact {} milli\n\0",
            (step_max * 1000.0) as i32,
            (step_exact * 1000.0) as i32
        );
        crate::kernel_ffi::print(line.as_bytes());
    };
    let mut pos = 0usize;
    if batched && ids.len() >= 2 && (prog.max_batch as usize) >= ids.len() {
        prog.run(&ids, 0, &mut out)?;
        prog.reference_step(&mem, &ids, 0, &mut exact);
        pos = ids.len();
        step = ids.len() - 1;
        compare(step, &out, &exact, rep);
        step += 1;
    }
    while step < steps {
        let t = if step < ids.len() {
            ids[step]
        } else {
            *tokens.get(step - ids.len()).ok_or("record file is short")?
        };
        let toks = [t];
        prog.run(&toks, pos, &mut out)?;
        prog.reference_step(&mem, &toks, pos, &mut exact);
        compare(step, &out, &exact, rep);
        pos += 1;
        step += 1;
    }
    rep.gpu_us_per_token = prog.gpu_us_per_token();
    Ok(())
}

// ---------------------------------------------------------------------------
// CPU reference of the kernels
// ---------------------------------------------------------------------------

/// What each kernel in `scripts/cuda/slm/` must compute, on the CPU, driven by
/// the same `ProgOp` the GPU gets. The software GPU in the host tests runs on
/// it, and `slm gpu opcheck` compares it with the real kernels.
pub mod reference {
    use super::*;
    use crate::inference::ops_transformer::{gqa_decode_step, matmul_quant_rows, swiglu_elementwise};
    use crate::inference::quant::dequantize_row_any;

    /// Where GPU VAs land for the CPU.
    pub struct Mem {
        pub weights_va: u64,
        pub weights: usize,
        pub weights_len: usize,
        pub data_va: u64,
        pub data: usize,
        pub data_len: usize,
        pub step_va: u64,
        pub step: usize,
        pub step_len: usize,
        /// Matmuls as exact dot products over dequantized weights, instead of
        /// the CPU path's Q8_K rounding of activations for Q4_K.
        pub exact: bool,
    }

    fn at(s: &Mem, va: u64) -> *mut u8 {
        let inside = |base: u64, len: usize| va >= base && va < base + len as u64;
        if inside(s.step_va, s.step_len) {
            (s.step + (va - s.step_va) as usize) as *mut u8
        } else if inside(s.data_va, s.data_len) {
            (s.data + (va - s.data_va) as usize) as *mut u8
        } else if inside(s.weights_va, s.weights_len) {
            (s.weights + (va - s.weights_va) as usize) as *mut u8
        } else {
            panic!("reference: unmapped VA {:#x}", va);
        }
    }

    fn u16s<'a>(p: *const u8, n: usize) -> &'a [u16] {
        unsafe { core::slice::from_raw_parts(p as *const u16, n) }
    }
    fn u16s_mut<'a>(p: *mut u8, n: usize) -> &'a mut [u16] {
        unsafe { core::slice::from_raw_parts_mut(p as *mut u16, n) }
    }

    pub fn exec(s: &Mem, op: &ProgOp) {
        let rows = op.rows as usize;
        let [a, b, c, d, e] = op.ptr;
        match op.kernel {
            x if x == Kernel::EmbedQ4k as u32 || x == Kernel::EmbedQ6k as u32 => {
                let quant = if x == Kernel::EmbedQ4k as u32 { GgmlType::Q4_K } else { GgmlType::Q6_K };
                let n = op.dim[0] as usize;
                let row_bytes = crate::inference::quant::quant_row_bytes(quant, n).expect("row bytes");
                let (ip, tp, op_) = (at(s, a), at(s, b), at(s, c));
                let mut f = alloc::vec![0.0f32; n + 256];
                for r in 0..rows {
                    let id = unsafe { *(ip as *const u32).add(r) } as usize;
                    let w = unsafe { core::slice::from_raw_parts(tp.add(id * row_bytes), row_bytes) };
                    dequantize_row_any(quant, w, &mut f[..n]).expect("dequant");
                    let o = u16s_mut(unsafe { op_.add(r * n * 2) }, n);
                    for j in 0..n {
                        o[j] = f32_to_f16(f[j]);
                    }
                }
            }
            x if x == Kernel::RmsNorm as u32 => {
                // As slm_rmsnorm: F32 gamma, F32 accumulator.
                let n = op.dim[0] as usize;
                let eps = f32::from_bits(op.scal[0]);
                let (xp, gp, op_) = (at(s, a), at(s, b), at(s, c));
                for r in 0..rows {
                    let x = u16s(unsafe { xp.add(r * n * 2) }, n).to_vec();
                    let o = u16s_mut(unsafe { op_.add(r * n * 2) }, n);
                    let mut acc = 0.0f32;
                    for &v in &x {
                        let f = f16_to_f32(v);
                        acc += f * f;
                    }
                    let inv = 1.0 / crate::inference::mathf::sqrtf(acc / n as f32 + eps);
                    for j in 0..n {
                        let g = unsafe { core::ptr::read_unaligned((gp as *const f32).add(j)) };
                        o[j] = f32_to_f16(f16_to_f32(x[j]) * inv * g);
                    }
                }
            }
            x if x == Kernel::Rope as u32 => {
                // ptr: x, out, table, pos (in place in this plan).
                let heads = op.dim[0] as usize;
                let hd = op.dim[1] as usize;
                let half = hd / 2;
                assert_eq!(a, b, "the plan runs RoPE in place");
                let (vp, lp, pp) = (at(s, a), at(s, c), at(s, d));
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
            x if x == Kernel::Gqa as u32 => {
                // Query row r sees positions 0..=pos[r].
                let (nq, nkv, hd) = (op.dim[0] as usize, op.dim[1] as usize, op.dim[2] as usize);
                let (qp, kp, vp, pp, op_) = (at(s, a), at(s, b), at(s, c), at(s, d), at(s, e));
                for r in 0..rows {
                    let seq = unsafe { *(pp as *const u32).add(r) } as usize + 1;
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
                    if s.exact {
                        for (a, &h) in af.iter_mut().zip(acts.iter()) {
                            *a = f16_to_f32(h);
                        }
                        for j in 0..n {
                            dequantize_row_any(quant, &w[j * row_bytes..(j + 1) * row_bytes], &mut dq[..kk])
                                .expect("dequant");
                            let mut acc = 0.0f64;
                            for t in 0..kk {
                                acc += dq[t] as f64 * af[t] as f64;
                            }
                            res[j] = acc as f32;
                        }
                    } else {
                        matmul_quant_rows(quant, w, n, kk, acts, &mut q8, &mut dq, &mut af, &mut res)
                            .expect("matmul");
                    }
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

}

// ---------------------------------------------------------------------------
// Kernel check against the CPU reference (`slm gpu opcheck`)
// ---------------------------------------------------------------------------

/// Deterministic FP16 activations in about +-4.
struct Lfsr(u32);
impl Lfsr {
    fn next_f16(&mut self) -> u16 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 17;
        self.0 ^= self.0 << 5;
        let v = (self.0 >> 8) as i32 % 8192;
        f32_to_f16(v as f32 / 2048.0)
    }
}

struct OpCheck {
    h: i32,
    mem: reference::Mem,
    data_cpu: usize,
    data_bytes: usize,
    step_cpu: usize,
    step_bytes: usize,
    d: u64,
    s: u64,
    bad: u32,
}

impl OpCheck {
    fn write_u16s(&self, off: usize, n: usize, seed: u32) {
        let mut r = Lfsr(seed | 1);
        let p = (self.data_cpu + off) as *mut u16;
        for i in 0..n {
            // SAFETY: callers keep off + n * 2 inside the data region.
            unsafe { p.add(i).write(r.next_f16()) };
        }
    }

    /// GPU, then reference, on identical inputs. `fill` writes the inputs.
    fn one(&mut self, name: &str, op: &ProgOp, out_off: usize, out_n: usize, f32_out: bool, fill: &dyn Fn(&Self)) {
        self.mem.exact = true;
        self.run_one(name, op, out_off, out_n, f32_out, fill, true);
    }

    /// A matmul, against both references. The exact dot product decides; the
    /// CPU path's Q8_K one is printed for information.
    fn matmul(&mut self, name: &str, op: &ProgOp, out_off: usize, out_n: usize, f32_out: bool, fill: &dyn Fn(&Self)) {
        self.mem.exact = true;
        self.run_one(name, op, out_off, out_n, f32_out, fill, true);
        if op.kernel == Kernel::MmQ4k as u32 && op.rows == 1 {
            self.mem.exact = false;
            let mut label = String::from(name);
            label.push_str(" vs q8k");
            self.run_one(&label, op, out_off, out_n, f32_out, fill, false);
        }
    }

    #[allow(clippy::too_many_arguments)]
    fn run_one(
        &mut self,
        name: &str,
        op: &ProgOp,
        out_off: usize,
        out_n: usize,
        f32_out: bool,
        fill: &dyn Fn(&Self),
        count: bool,
    ) {
        let esz = if f32_out { 4 } else { 2 };
        // SAFETY: both regions are ours and the GPU is idle between checks.
        unsafe {
            core::ptr::write_bytes(self.data_cpu as *mut u8, 0, self.data_bytes);
        }
        fill(self);
        // SAFETY: as above. Everything the GPU reads must be in DRAM.
        unsafe {
            slm_prog_clean(self.h, self.data_cpu as *const u8, self.data_bytes);
            slm_prog_clean(self.h, self.step_cpu as *const u8, self.step_bytes);
        }
        let mut st = RunStats::default();
        // SAFETY: `op` and `st` outlive the calls; the handle is open.
        let rc = unsafe {
            let u = slm_prog_update(self.h, 0, 0, op);
            if u < 0 { u } else { slm_prog_run(self.h, 0, &mut st) }
        };
        if rc != 0 {
            self.report(name, op, 0, 0, 0, st.gpu_ns, err_message(rc));
            self.bad += 1;
            return;
        }
        let outp = (self.data_cpu + out_off) as *const u8;
        // SAFETY: the output lies in the data region and the GPU has finished.
        let gpu: Vec<u8> = unsafe {
            slm_prog_invalidate(self.h, outp, out_n * esz);
            core::slice::from_raw_parts(outp, out_n * esz).to_vec()
        };
        // The reference gets freshly written inputs: some kernels work in place.
        // SAFETY: as above.
        unsafe {
            slm_prog_invalidate(self.h, self.data_cpu as *const u8, self.data_bytes);
            core::ptr::write_bytes(self.data_cpu as *mut u8, 0, self.data_bytes);
        }
        fill(self);
        reference::exec(&self.mem, op);
        // SAFETY: as above.
        let cpu = unsafe { core::slice::from_raw_parts(outp, out_n * esz) };
        let (mut max_err, mut sq, mut max_val, mut over) = (0.0f32, 0.0f32, 0.0f32, 0u32);
        for i in 0..out_n {
            let (g, c) = if f32_out {
                let j = i * 4;
                (
                    f32::from_le_bytes([gpu[j], gpu[j + 1], gpu[j + 2], gpu[j + 3]]),
                    f32::from_le_bytes([cpu[j], cpu[j + 1], cpu[j + 2], cpu[j + 3]]),
                )
            } else {
                let j = i * 2;
                (
                    f16_to_f32(u16::from_le_bytes([gpu[j], gpu[j + 1]])),
                    f16_to_f32(u16::from_le_bytes([cpu[j], cpu[j + 1]])),
                )
            };
            let e = (g - c).abs();
            if !(e <= max_err) {
                max_err = e;
            }
            sq += e * e;
            max_val = max_val.max(c.abs());
            if !(e <= 0.002 * c.abs() + 0.03) {
                over += 1;
            }
        }
        let rms = crate::inference::mathf::sqrtf(sq / out_n as f32);
        let verdict = if over == 0 { "ok" } else { "BAD" };
        if over != 0 && count {
            self.bad += 1;
        }
        self.report(name, op, (max_err * 1000.0) as i32, (rms * 1000.0) as i32, over, st.gpu_ns, verdict);
        let _ = max_val;
    }

    fn report(&self, name: &str, op: &ProgOp, max_m: i32, rms_m: i32, over: u32, gpu_ns: u64, verdict: &str) {
        let mut line = String::new();
        let _ = write!(
            line,
            "[opcheck] {name} rows={} {verdict}: max {max_m} rms {rms_m} milli, {over} over, gpu {} us\n\0",
            op.rows,
            gpu_ns / 1000
        );
        crate::kernel_ffi::print(line.as_bytes());
    }
}

/// Every kernel, on the real shapes and tensors of model `model`, against the
/// CPU reference with random activations. Returns how many checks failed.
pub fn opcheck(model: u32, simt: bool) -> Result<u32, &'static str> {
    let info = crate::slm::registry::get_info(model as usize).ok_or("model not loaded")?;
    let (weights, wlen, d, rope_base, arch) = crate::slm::registry::with_loaded_slm(model as usize, |slm| {
        let w = slm.weight_bytes().map(|b| (b.as_ptr() as usize, b.len()));
        let d = ModelDesc::from_slm(slm, info.vocab_size);
        (w, d, slm.arch().rope_freq_base, slm.arch().architecture)
    })
    .map(|(w, d, r, a)| (w.map_or(0, |x| x.0), w.map_or(0, |x| x.1), d, r, a))
    .ok_or("model not loaded")?;
    let d = d?;
    let dims = d.dims;
    let (hidden, inter, nq, nkv, hd) = (
        dims.hidden as usize,
        dims.intermediate as usize,
        dims.n_head as usize,
        dims.n_head_kv as usize,
        dims.head_dim as usize,
    );
    const POSITIONS: usize = 16;
    const ROWS: usize = 64;
    let rope = crate::inference::ops_transformer::RopeTable::new(
        hd,
        POSITIONS + ROWS,
        rope_base,
        crate::inference::ops_transformer::RopePairing::for_arch(arch),
    );
    // Two activation buffers, one output, all big enough for ROWS rows of the
    // widest vector, the RoPE table and the logits.
    let row_max = hidden.max(inter).max(nq * hd);
    let a_off = 0usize;
    let b_off = align256((ROWS * row_max * 2) as u64) as usize;
    let o_off = 2 * b_off;
    let k_off = o_off + b_off.max(dims.vocab as usize * 4);
    let kv_bytes = align256(((POSITIONS + ROWS) * nkv * hd * 2) as u64) as usize;
    let v_off = k_off + kv_bytes;
    let rope_off = v_off + kv_bytes;
    let data_bytes = rope_off + rope.cos_sin.len() * 4 + 4096;
    let args = OpenArgs {
        weights: weights as *const u8,
        weights_bytes: wlen as u64,
        data_bytes: data_bytes as u64,
        step_bytes: 4096,
        max_launches: [1, 0],
        flags: OPT_FLAGS.load(core::sync::atomic::Ordering::Relaxed) & FLAG_LEAN_QMD,
        barrier: OPT_BARRIER.load(core::sync::atomic::Ordering::Relaxed),
    };
    let mut out = OpenOut::default();
    // SAFETY: both pointers are to live locals of the declared types.
    let rc = unsafe { slm_prog_open(&args, &mut out) };
    if rc != 0 {
        return Err(err_message(rc));
    }
    struct Close(i32);
    impl Drop for Close {
        fn drop(&mut self) {
            // SAFETY: opened above, closed once.
            unsafe { slm_prog_close(self.0) };
        }
    }
    let _close = Close(out.handle);
    let (da, sa, wa) = (out.data_gpu_va, out.step_gpu_va, out.weights_gpu_va);
    let mut c = OpCheck {
        h: out.handle,
        mem: reference::Mem {
            weights_va: wa,
            weights,
            weights_len: wlen,
            data_va: da,
            data: out.data_cpu,
            data_len: data_bytes,
            step_va: sa,
            step: out.step_cpu,
            step_len: 4096,
            exact: true,
        },
        data_cpu: out.data_cpu,
        data_bytes,
        step_cpu: out.step_cpu,
        step_bytes: 4096,
        d: da,
        s: sa,
        bad: 0,
    };
    // Positions and token ids, read by RoPE, GQA and the embedding.
    let (ids_off, pos_off) = (0usize, 512usize);
    for r in 0..ROWS {
        // SAFETY: both arrays fit in the 4096 B step page.
        unsafe {
            ((out.step_cpu + ids_off) as *mut u32).add(r).write((17 + 13 * r as u32) % dims.vocab);
            ((out.step_cpu + pos_off) as *mut u32).add(r).write((POSITIONS + r) as u32);
        }
    }
    let (da_a, da_b, da_o) = (c.d + a_off as u64, c.d + b_off as u64, c.d + o_off as u64);
    let sa_pos = c.s + pos_off as u64;

    // A one-launch program, rewritten for each check.
    let add = ProgOp {
        kernel: Kernel::Add as u32,
        rows: 1,
        dim: [32, 0, 0, 0],
        ptr: [da_a, da_b, da_o, 0, 0],
        scal: [0, 0],
    };
    // SAFETY: the handle is open and `add` outlives the calls.
    unsafe {
        let r = slm_prog_add(c.h, 0, &add);
        if r < 0 {
            return Err(err_message(r));
        }
        let r = slm_prog_finish(c.h, 0);
        if r < 0 {
            return Err(err_message(r));
        }
    }
    let simt_flag = if simt { MM_SIMT } else { 0 };

    let l0 = d.layers[0];
    let fill_a = |n: usize| move |c: &OpCheck| c.write_u16s(a_off, n, 0x1234);

    for rows in [1usize, 2, 9, 63, ROWS] {
        // Matmuls: both quant types, biased and not, FP16 and FP32 out.
        let mm = |c: &mut OpCheck, name: &str, m: &Matmul, k: usize, n: usize, f16: bool, r: usize| {
            let op = ProgOp {
                kernel: m.kernel as u32,
                rows: r as u32,
                dim: [k as u32, n as u32, 0, 0],
                ptr: [da_a, wa + m.w_off, da_o, m.bias_off.map_or(0, |b| wa + b), 0],
                scal: [if f16 { MM_OUT_F16 } else { 0 }, simt_flag],
            };
            c.matmul(name, &op, o_off, r * n, !f16, &fill_a(r * k));
        };
        mm(&mut c, "mm_q", &l0.q, hidden, nq * hd, true, rows);
        mm(&mut c, "mm_k", &l0.k, hidden, nkv * hd, true, rows);
        mm(&mut c, "mm_gate", &l0.gate, hidden, inter, true, rows);
        mm(&mut c, "mm_down", &l0.down, inter, hidden, true, rows);
        mm(&mut c, "mm_o", &l0.o, nq * hd, hidden, true, rows);

        let op = ProgOp {
            kernel: d.embed.kernel as u32,
            rows: rows as u32,
            dim: [hidden as u32, 0, 0, 0],
            ptr: [c.s + ids_off as u64, wa + d.embed.w_off, da_o, 0, 0],
            scal: [0, 0],
        };
        c.one("embed", &op, o_off, rows * hidden, false, &|_| {});

        let op = ProgOp {
            kernel: Kernel::RmsNorm as u32,
            rows: rows as u32,
            dim: [hidden as u32, 0, 0, 0],
            ptr: [da_a, wa + d.norms[0].0, da_o, 0, 0],
            scal: [RMS_EPS.to_bits(), 0],
        };
        c.one("rmsnorm", &op, o_off, rows * hidden, false, &fill_a(rows * hidden));

        let op = ProgOp {
            kernel: Kernel::Add as u32,
            rows: rows as u32,
            dim: [hidden as u32, 0, 0, 0],
            ptr: [da_a, da_b, da_o, 0, 0],
            scal: [0, 0],
        };
        c.one("add", &op, o_off, rows * hidden, false, &|c: &OpCheck| {
            c.write_u16s(a_off, rows * hidden, 0x1234);
            c.write_u16s(b_off, rows * hidden, 0x777);
        });

        let op = ProgOp {
            kernel: Kernel::SwiGlu as u32,
            rows: rows as u32,
            dim: [inter as u32, 0, 0, 0],
            ptr: [da_a, da_b, da_o, 0, 0],
            scal: [0, 0],
        };
        c.one("swiglu", &op, o_off, rows * inter, false, &|c: &OpCheck| {
            c.write_u16s(a_off, rows * inter, 0x1234);
            c.write_u16s(b_off, rows * inter, 0x777);
        });

        // RoPE at positions above 0 (position 0 is the identity), in place.
        let table = rope.cos_sin.as_ptr() as usize;
        let table_len = rope.cos_sin.len() * 4;
        let fill_rope = move |c: &OpCheck| {
            c.write_u16s(a_off, rows * nq * hd, 0x1234);
            // SAFETY: rope_off + table_len is inside the data region.
            unsafe {
                core::ptr::copy_nonoverlapping(table as *const u8, (c.data_cpu + rope_off) as *mut u8, table_len);
            }
        };
        let op = ProgOp {
            kernel: Kernel::Rope as u32,
            rows: rows as u32,
            dim: [nq as u32, hd as u32, 0, 0],
            ptr: [da_a, da_a, c.d + rope_off as u64, sa_pos, 0],
            scal: [0, 0],
        };
        c.one("rope", &op, a_off, rows * nq * hd, false, &fill_rope);

        // Attention over POSITIONS + r positions: the mask and GQA grouping.
        let n_pos = POSITIONS + rows;
        let op = ProgOp {
            kernel: Kernel::Gqa as u32,
            rows: rows as u32,
            dim: [nq as u32, nkv as u32, hd as u32, 0],
            ptr: [da_a, c.d + k_off as u64, c.d + v_off as u64, sa_pos, da_o],
            scal: [0, 0],
        };
        c.one("gqa", &op, o_off, rows * nq * hd, false, &|c: &OpCheck| {
            c.write_u16s(a_off, rows * nq * hd, 0x1234);
            c.write_u16s(k_off, n_pos * nkv * hd, 0x5555);
            c.write_u16s(v_off, n_pos * nkv * hd, 0x2468);
        });
    }
    // The LM head: one row, FP32 logits.
    let lm = d.lm_head;
    let op = ProgOp {
        kernel: lm.kernel as u32,
        rows: 1,
        dim: [hidden as u32, dims.vocab, 0, 0],
        ptr: [da_a, wa + lm.w_off, da_o, 0, 0],
        scal: [0, simt_flag],
    };
    c.matmul("lm_head", &op, o_off, dims.vocab as usize, true, &fill_a(hidden));
    Ok(c.bad)
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
        pub max_batch: u32,
        /// Fail run number N (0-based) with this rc.
        pub fail_run: Option<(u32, i32)>,
        pub closes: u32,
    }

    pub static STATE: Mutex<Option<Soft>> = Mutex::new(None);
    pub static MARKED_RUNS: core::sync::atomic::AtomicU32 = core::sync::atomic::AtomicU32::new(0);
    pub static KNOBS: Mutex<Knobs> = Mutex::new(Knobs {
        no_backend: false,
        max_batch: 64,
        fail_run: None,
        closes: 0,
    });

    pub fn reset_knobs() {
        let mut k = KNOBS.lock().unwrap();
        *k = Knobs { no_backend: false, max_batch: 64, fail_run: None, closes: 0 };
    }

    fn mem_of(s: &mut Soft) -> reference::Mem {
        reference::Mem {
            weights_va: W_BASE,
            weights: s.weights,
            weights_len: s.weights_len,
            data_va: D_BASE,
            data: s.data.as_mut_ptr() as usize,
            data_len: s.data.len(),
            step_va: S_BASE,
            step: s.step.as_mut_ptr() as usize,
            step_len: s.step.len(),
            // What the kernels compute: no Q8_K in them.
            exact: true,
        }
    }

    fn exec(s: &mut Soft, op: &ProgOp) {
        reference::exec(&mem_of(s), op);
    }

    #[no_mangle]
    extern "C" fn slm_prog_available() -> i32 {
        !KNOBS.lock().unwrap().no_backend as i32
    }
    #[no_mangle]
    extern "C" fn slm_prog_open(a: *const OpenArgs, out: *mut OpenOut) -> i32 {
        let k = KNOBS.lock().unwrap();
        let _ = &k;
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
    extern "C" fn slm_prog_mark(_h: i32, _shape: u32) -> i32 {
        0
    }
    /// One microsecond per interval, so a test can add up what reaches a class.
    #[no_mangle]
    extern "C" fn slm_prog_run_marked(h: i32, shape: u32, stats: *mut RunStats, ns: *mut u64, cap: u32) -> i32 {
        MARKED_RUNS.fetch_add(1, core::sync::atomic::Ordering::Relaxed);
        let rc = slm_prog_run(h, shape, stats);
        for i in 0..cap as usize {
            // SAFETY: the caller's array holds `cap` values.
            unsafe { *ns.add(i) = 1000 };
        }
        rc
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
    extern "C" fn slm_prog_profile_ns(_h: i32, _shape: u32, _idx: u32) -> u64 {
        0
    }
    #[no_mangle]
    extern "C" fn slm_prog_max_batch(_simt: bool) -> u32 {
        KNOBS.lock().unwrap().max_batch
    }
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use crate::slm::golden::Records;
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
        let embed = Matmul { kernel: Kernel::EmbedQ6k, ..m };
        ModelDesc {
            dims: d,
            layers: alloc::vec![l; d.layers as usize],
            lm_head: m,
            embed,
            norms: alloc::vec![(0x2000, 0x3000); d.layers as usize],
            out_norm: 0x4000,
        }
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
        assert_eq!(b.step_positions, align256(64 * 4));
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
        let p = build_plan(&d, &a, false, false);
        // The embedding, 15 launches a layer, the final norm, the LM head.
        assert_eq!(p.ops.len(), 1 + 28 * 15 + 2);
        assert!(p.ops.len() <= 1000);
        assert_eq!(p.ops[0].kernel, Kernel::EmbedQ6k);
        let n_rope = p.ops.iter().filter(|o| o.kernel == Kernel::Rope).count();
        assert_eq!(n_rope, 2 * 28);
        // Exactly the ops that write the KV slot of this position.
        assert_eq!(p.patch.len(), 3 * 28);

        // Layer 0, after the embedding: norm | Q K V | RoPE RoPE | GQA | O |
        // add | norm | gate up | swiglu | down | add.
        let want = [
            true, true, false, false, true, false, true, true, true, true, true, false, true,
            true, true,
        ];
        assert_eq!(&p.barrier_before[1..16], &want);
        // The next layer's norm reads x, written by the last add.
        assert!(p.barrier_before[16]);
        // The LM head reads the final norm's output.
        assert!(p.barrier_before[p.ops.len() - 1]);
        // Only the final norm and LM head are single-row regardless of batch.
        let last = &p.ops[p.ops.len() - 1];
        assert!(!last.batched);
        assert_eq!(last.dim[1], 151936);
        assert_eq!(last.scal[0], 0, "logits stay FP32");
        // The SIMT reference only changes the matmuls' flag.
        let s = build_plan(&d, &a, false, true);
        assert_eq!(s.ops.len(), p.ops.len());
        assert_eq!(s.ops[2].scal[1], MM_SIMT);
        assert_eq!(p.ops[2].scal[1], 0);
    }

    #[test]
    fn every_dependency_has_a_barrier() {
        // Independent check of the builder's rule, on the finished plan.
        let d = fake_desc(QWEN_1_5B);
        let a = Arena::compute(&d.dims, 4096, 64).unwrap();
        for batched in [false, true] {
            let p = build_plan(&d, &a, batched, false);
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
        let p = build_plan(&d, &a, false, false);
        let s = StepArgs { pos: 7, rows: 1 };
        // ops[0] is the embedding, each layer is 15 after that.
        let l1 = 1 + 15;
        // Layer 1's K projection writes position 7 of layer 1.
        let k1 = p.ops[l1 + 2].materialize(&bases, &s);
        assert_eq!(k1.ptr[2], bases.data + a.kv_k + a.kv_per_layer + 7 * a.kv_per_pos);
        assert_eq!(k1.rows, 1);
        // RoPE on K rotates that slot in place.
        let r1 = p.ops[l1 + 5].materialize(&bases, &s);
        assert_eq!(r1.ptr[0], k1.ptr[2]);
        assert_eq!(r1.ptr[1], k1.ptr[2]);
        assert_eq!(r1.ptr[3], bases.step + a.step_positions);
        // Layer 1's attention reads the cache from its base and the positions.
        let g1 = p.ops[l1 + 6].materialize(&bases, &s);
        assert_eq!(g1.kernel, Kernel::Gqa as u32);
        assert_eq!(g1.ptr[1], bases.data + a.kv_k + a.kv_per_layer);
        assert_eq!(g1.ptr[3], bases.step + a.step_positions);
        // Everything reads x; only the embedding reads the ids.
        assert_eq!(p.ops[0].materialize(&bases, &s).ptr[0], bases.step + a.step_ids);
        assert_eq!(p.ops[0].materialize(&bases, &s).ptr[2], bases.data + a.x);
        assert_eq!(p.ops[1].materialize(&bases, &s).ptr[0], bases.data + a.x);
        // Gammas come straight from the file.
        assert_eq!(p.ops[1].materialize(&bases, &s).ptr[1], bases.weights + 0x2000);

        // Prefill: rows come from the step, and the final norm takes the last
        // row of x.
        let pb = build_plan(&d, &a, true, false);
        let s = StepArgs { pos: 3, rows: 5 };
        let gb = pb.ops[7].materialize(&bases, &s);
        assert_eq!(gb.kernel, Kernel::Gqa as u32);
        assert_eq!(gb.rows, 5);
        let fin = pb.ops[pb.ops.len() - 2].materialize(&bases, &s);
        assert_eq!(fin.rows, 1);
        assert_eq!(fin.ptr[0], bases.data + a.x + 4 * 1536 * 2);
    }

    fn compare_logits_for_tests(got: &[f32], want: &[f32], step: usize) {
        assert_eq!(got.len(), want.len());
        for (i, (g, w)) in got.iter().zip(want.iter()).enumerate() {
            assert!(
                (g - w).abs() <= GOLDEN_LLAMA_TOLERANCE,
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

    fn step_prog(p: &mut GpuProgram, _model: u32, toks: &[u32], pos: usize, out: &mut Vec<f32>) {
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

    /// Every launch is in a class; a class boundary always has a barrier, so
    /// the marks can attribute all the time; and the plan's figures add up.
    #[test]
    fn plan_facts_add_up_and_marks_cover_every_boundary() {
        soft::reset_knobs();
        let model = load_tiny();
        let sid = open_session(model);
        assert_eq!(crate::rust_slm_session_configure(sid, 2, 4), 0);
        let f = session::with_session(sid as usize, |s| {
            let p = s.gpu.as_ref().unwrap();
            let boundaries = (1..p.decode.ops.len())
                .filter(|&i| p.decode.class[i] != p.decode.class[i - 1])
                .inspect(|&i| assert!(p.decode.barrier_before[i], "class change at {i} has no barrier"))
                .count() as u32;
            assert_eq!(p.mark_class.len() as u32, boundaries + 1);
            assert_eq!(p.mark_ns.len(), p.mark_class.len());
            p.plan_facts()
        })
        .unwrap();
        assert_eq!(f.class_launches.iter().sum::<u32>(), f.launches);
        assert!(f.marks > 0 && f.marks <= f.barriers);
        assert!(f.mma > 0 && f.macs_tc > 0 && f.weight_bytes > 0);
        assert_eq!(f.macs_simt, 0);
        // Only matmuls are on tensor cores.
        for (i, t) in f.class_tier.iter().enumerate() {
            let mm = i == OpClass::MatmulAttn as usize || i == OpClass::MatmulMlp as usize || i == OpClass::LmHead as usize;
            assert_eq!(*t, if mm { Tier::Hmma as u32 } else { Tier::Simt as u32 }, "class {i}");
        }
        // What the same layer count looks like on the CPU path.
        let d = session::with_session(sid as usize, |s| s.gpu.as_ref().unwrap().dims()).unwrap();
        assert_eq!(f.launches, 15 * d.layers + 3);
        assert_eq!(crate::rust_slm_session_close(sid), 0);
        reset_for_tests();
    }

    /// Off: only plain runs. On: every decode step uses marks and each class
    /// gets the intervals that belong to it. Prefill stays plain either way.
    #[test]
    fn stats_profile_chooses_the_marked_program_between_steps() {
        use core::sync::atomic::Ordering::Relaxed;
        soft::reset_knobs();
        let model = load_tiny();
        let r = rec();
        let sid = open_session(model);
        assert_eq!(crate::rust_slm_session_configure(sid, 2, 4), 0);
        let tel = session::telemetry(sid as usize).unwrap();

        soft::MARKED_RUNS.store(0, Relaxed);
        let _ = prompt_on_session(sid, r.get("prompt.text").unwrap(), 3);
        assert_eq!(soft::MARKED_RUNS.load(Relaxed), 0, "stats disabled must not touch the marks");
        let s = tel.snapshot(0);
        assert_eq!(s.profiled_tokens, 0);
        assert!(s.gpu_steps > 0, "GPU busy has a source without --profile");
        assert_eq!(s.tc_issue_bp, crate::slm::telemetry::NA, "no matmul time measured");
        assert_ne!(s.tc_mac_share_bp, crate::slm::telemetry::NA);
        assert_eq!(s.path, session::PATH_PROGRAM);
        assert_eq!(s.ops_cpu, 0);

        assert_eq!(crate::rust_slm_session_reset(sid), 0);
        tel.set_profile(true);
        let _ = prompt_on_session(sid, r.get("prompt.text").unwrap(), 3);
        let s = tel.snapshot(crate::slm::telemetry::MMA_RATE_GPC_HZ);
        let runs = soft::MARKED_RUNS.load(Relaxed) as u64;
        assert!(runs >= 2);
        assert_eq!(s.profiled_tokens, runs);
        assert_ne!(s.tc_issue_bp, crate::slm::telemetry::NA);
        // The soft GPU reports 1 us per interval.
        let total: u64 = s.classes.iter().map(|c| c.total_ns).sum();
        let marks = session::with_session(sid as usize, |x| x.gpu.as_ref().unwrap().plan_facts().marks).unwrap();
        assert_eq!(total, runs * (marks as u64 + 1) * 1000);
        for c in [OpClass::Embed, OpClass::Gqa, OpClass::MatmulMlp, OpClass::LmHead] {
            assert!(s.classes[c as usize].last_ns > 0, "{c:?}");
        }
        assert_eq!(crate::rust_slm_session_close(sid), 0);
        reset_for_tests();
    }

    /// A session the GPU gave up on stops claiming GPU figures.
    #[test]
    fn fallback_moves_the_telemetry_to_the_cpu_too() {
        soft::reset_knobs();
        let model = load_tiny();
        let r = rec();
        let sid = open_session(model);
        assert_eq!(crate::rust_slm_session_configure(sid, 2, 4), 0);
        soft::KNOBS.lock().unwrap().fail_run = Some((2, -6));
        let _ = prompt_on_session(sid, r.get("prompt.text").unwrap(), 4);
        let s = session::telemetry(sid as usize).unwrap().snapshot(1);
        assert_eq!(s.path, session::PATH_CPU);
        assert_eq!((s.ops_tier1, s.ops_tier2, s.launches), (0, 0, 0));
        assert_eq!(s.tc_mac_share_bp, crate::slm::telemetry::NA);
        assert_eq!(s.tc_issue_bp, crate::slm::telemetry::NA);
        assert_eq!(crate::rust_slm_session_close(sid), 0);
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
        soft::KNOBS.lock().unwrap().max_batch = 0;
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

    /// The software GPU is the reference, so every check agrees with itself;
    /// this pins that the harness covers all kernels and reports failure.
    #[test]
    fn opcheck_runs_on_the_soft_gpu() {
        soft::reset_knobs();
        let model = load_tiny();
        assert_eq!(opcheck(model, false), Ok(0));
        soft::KNOBS.lock().unwrap().fail_run = Some((2, -6));
        assert_eq!(opcheck(model, false), Ok(1));
        soft::reset_knobs();
        reset_for_tests();
    }

    /// The smoke chain against its own CPU reference, and a run that fails.
    #[test]
    fn smoke_chain_agrees_with_the_cpu() {
        soft::reset_knobs();
        let mut rep = SmokeReport::default();
        smoke(7, 1536, &mut rep).unwrap();
        assert_eq!(rep.launches, 7);
        assert_eq!(rep.bad_elements, 0);
        assert_eq!(smoke(0, 1536, &mut rep), Err("no launches"));
        assert_eq!(smoke(1, 33, &mut rep), Err("width is not a multiple of 32"));
        soft::KNOBS.lock().unwrap().fail_run = Some((0, -6));
        assert_eq!(smoke(3, 1536, &mut rep), Err("GPU step timed out"));
        soft::reset_knobs();
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
        assert_eq!(rep.argmax_matches, rep.steps);
        // A plan that's wrong misses by far more than the tolerance.
        assert!(rep.max_err_milli > 0 && rep.max_err_milli < 1500, "max_err {}", rep.max_err_milli);
        assert_eq!(rep.exact_first_bad_step, -1, "{rep:?}");
        assert!(rep.exact_max_err_milli < 100, "{rep:?}");
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

    const LOGIT_CPU: f32 = crate::slm::golden::LOGIT_TOLERANCE;

    /// The program rounds the attention output to FP16 before the residual
    /// and uses FP32 gammas, where `forward_one` has them the other way
    /// round, so the two aren't bit-identical.
    /// A wrong plan still can't hide inside the 1.0 tolerance against
    /// llama.cpp: the program must be about as close to llama.cpp as
    /// `forward_one` is, and close to `forward_one` in absolute terms.
    #[test]
    fn soft_gpu_tracks_forward_one_tightly() {
        use crate::slm::forward::{forward_one, ForwardScratch};
        use crate::slm::sampler::Sampler;
        use crate::slm::session::Session;
        soft::reset_knobs();
        let model = load_tiny();
        let r = rec();
        let ids = r.u32s("prompt.ids").unwrap();
        let tokens = r.u32s("greedy.tokens").unwrap();
        let logits = r.f32s("greedy.logits").unwrap();
        let n_vocab = r.u32s("greedy.n_vocab").unwrap()[0] as usize;
        let mut cpu = Session::open(model, 64, Sampler::Greedy, 0).unwrap();
        cpu.path = ForwardPath::Cpu;
        let mut sc = ForwardScratch::new(&cpu.arch, cpu.max_ctx, cpu.vocab_size as usize);
        let mut p = build_tiny(model, 64);
        let mut out = Vec::new();
        let (mut d_cpu, mut d_prog, mut d_both) = (0.0f32, 0.0f32, 0.0f32);
        for step in 0..logits.len() / n_vocab {
            let t = if step < ids.len() { ids[step] } else { tokens[step - ids.len()] };
            with_loaded_slm(model as usize, |slm| forward_one(&mut cpu, slm, t, &mut sc)).unwrap().unwrap();
            step_prog(&mut p, model, &[t], step, &mut out);
            let want = &logits[step * n_vocab..(step + 1) * n_vocab];
            for i in 0..n_vocab {
                d_cpu = d_cpu.max((sc.logits[i] - want[i]).abs());
                d_prog = d_prog.max((out[i] - want[i]).abs());
                d_both = d_both.max((out[i] - sc.logits[i]).abs());
            }
        }
        std::eprintln!("max |cpu-llama|={d_cpu} |prog-llama|={d_prog} |prog-cpu|={d_both}");
        std::eprintln!("(forward_one rounds through Q8_K, the kernels don't)");
        assert!(d_cpu <= LOGIT_CPU, "forward_one {d_cpu}");
        assert!(d_prog < GOLDEN_LLAMA_TOLERANCE, "program {d_prog}");
        assert!(d_both < 2.0, "program and forward_one differ by {d_both}");
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
