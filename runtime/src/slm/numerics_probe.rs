//! Test-only forward pass with every rounding step switchable, used to find
//! which numeric layer makes the greedy tokens diverge from llama.cpp (#475).
//! With all [`Rounding`] flags set it computes exactly what `forward_one` does,
//! which `numerics_probe_all_flags_matches_forward_one` pins;
//! `golden_real_model_numerics` drives it on the real model. Results are in
//! "Numeric fidelity against llama.cpp", `docs/design/slm-integration.md`.

#![cfg(test)]

use alloc::format;
use alloc::string::String;
use alloc::vec;
use alloc::vec::Vec;

use crate::inference::mathf::sqrtf;
use crate::inference::quant::{
    dequantize_row_any, q8_k_byte_size, quant_row_bytes, quantize_row_q8_k, vec_dot_q4_k_q8_k,
};
use crate::slm::gguf::{f16_to_f32, f32_to_f16, GgmlType};
use crate::slm::registry::LoadedSlm;

/// Where to round through FP16. K, V and Q after RoPE are FP16 in llama.cpp
/// too (its KV cache is F16 and flash attention converts Q), so they always are.
#[derive(Clone, Copy, Default, Debug)]
pub struct Rounding {
    /// Embedding output and both residual sums.
    pub x: bool,
    /// RMSNorm output.
    pub norm: bool,
    /// RMSNorm's learned scale.
    pub gamma: bool,
    /// Q/K/V before RoPE.
    pub pre_rope: bool,
    /// Attention output.
    pub attn: bool,
    /// Gate, up, their product and the FFN output.
    pub mlp: bool,
    /// Q6_K weights dequantized and dotted in F32. Unset: through Q8_K, as ggml.
    pub q6_f32: bool,
    /// RMS sum accumulated in F32. Unset: double, as ggml.
    pub rms_f32: bool,
}

impl Rounding {
    /// `all`, or a comma-separated flag list; a leading `-` removes from `all`.
    pub fn parse(spec: &str) -> Self {
        let all = Self {
            x: true,
            norm: true,
            gamma: true,
            pre_rope: true,
            attn: true,
            mlp: true,
            q6_f32: true,
            rms_f32: true,
        };
        if spec == "all" {
            return all;
        }
        let (mut r, value) = if spec.starts_with('-') { (all, false) } else { (Self::default(), true) };
        for flag in spec.trim_start_matches('-').split(',').filter(|f| !f.is_empty() && *f != "none") {
            match flag {
                "x" => r.x = value,
                "norm" => r.norm = value,
                "gamma" => r.gamma = value,
                "pre_rope" => r.pre_rope = value,
                "attn" => r.attn = value,
                "mlp" => r.mlp = value,
                "q6_f32" => r.q6_f32 = value,
                "rms_f32" => r.rms_f32 = value,
                other => panic!("flag {other}"),
            }
        }
        r
    }
}

#[inline]
fn fp16(v: f32) -> f32 {
    f16_to_f32(f32_to_f16(v))
}

fn round_if(on: bool, v: &mut [f32]) {
    if on {
        for x in v.iter_mut() {
            *x = fp16(*x);
        }
    }
}

/// ggml_vec_dot_q6_K_q8_K in plain Rust: exact integer sum per super-block.
pub fn vec_dot_q6_k_q8_k(w: &[u8], a: &[u8]) -> f32 {
    const W: usize = 210;
    const A: usize = 292;
    let mut total = 0.0f32;
    for (wb, ab) in w.chunks_exact(W).zip(a.chunks_exact(A)) {
        let d_w = f16_to_f32(u16::from_le_bytes([wb[208], wb[209]]));
        let d_a = f32::from_le_bytes([ab[0], ab[1], ab[2], ab[3]]);
        let qs = &ab[4..260];
        let mut sum = 0i64;
        for half in 0..2 {
            let ql = &wb[half * 64..half * 64 + 64];
            let qh = &wb[128 + half * 32..128 + half * 32 + 32];
            let sc = &wb[192 + half * 8..192 + half * 8 + 8];
            let q = &qs[half * 128..half * 128 + 128];
            for l in 0..32 {
                let is = l / 16;
                let q1 = ((ql[l] & 0x0F) | ((qh[l] & 3) << 4)) as i32 - 32;
                let q2 = ((ql[l + 32] & 0x0F) | (((qh[l] >> 2) & 3) << 4)) as i32 - 32;
                let q3 = ((ql[l] >> 4) | (((qh[l] >> 4) & 3) << 4)) as i32 - 32;
                let q4 = ((ql[l + 32] >> 4) | (((qh[l] >> 6) & 3) << 4)) as i32 - 32;
                sum += (sc[is] as i8 as i32 * q1 * q[l] as i8 as i32) as i64;
                sum += (sc[is + 2] as i8 as i32 * q2 * q[l + 32] as i8 as i32) as i64;
                sum += (sc[is + 4] as i8 as i32 * q3 * q[l + 64] as i8 as i32) as i64;
                sum += (sc[is + 6] as i8 as i32 * q4 * q[l + 96] as i8 as i32) as i64;
            }
        }
        total += d_w * d_a * sum as f32;
    }
    total
}

pub struct Probe {
    pub flags: Rounding,
    q8: Vec<u8>,
    dq: Vec<f32>,
    x: Vec<f32>,
    res: Vec<f32>,
    xn: Vec<f32>,
    gamma: Vec<f32>,
    q: Vec<f32>,
    k: Vec<f32>,
    v: Vec<f32>,
    attn: Vec<f32>,
    proj: Vec<f32>,
    gate: Vec<f32>,
    up: Vec<f32>,
    /// Per layer, per position: `n_head_kv * head_dim` entries, FP16 values.
    kv_k: Vec<Vec<f32>>,
    kv_v: Vec<Vec<f32>>,
    pos: usize,
    scratch: Vec<f32>,
    pub logits: Vec<f32>,
    cos_sin: Vec<f32>,
    name: String,
    /// Set to dump named activations, for comparing with a llama.cpp `cb_eval` dump.
    pub trace: Option<std::fs::File>,
    /// Move every hidden element by -1, 0 or +1 F32 ULP after each residual sum,
    /// the size of difference measured against llama.cpp at those points. Off
    /// at 0; otherwise the xorshift state.
    pub dither: u32,
}

fn dither(state: &mut u32, x: &mut [f32]) {
    if *state == 0 {
        return;
    }
    for v in x.iter_mut() {
        *state ^= *state << 13;
        *state ^= *state >> 17;
        *state ^= *state << 5;
        let bits = v.to_bits();
        *v = f32::from_bits(match *state % 3 {
            0 => bits.wrapping_add(1),
            1 => bits.wrapping_sub(1),
            _ => bits,
        });
    }
}

fn trace(file: &mut Option<std::fs::File>, name: &str, data: &[f32]) {
    use std::io::Write as _;
    if let Some(f) = file.as_mut() {
        f.write_all(&(name.len() as u32).to_le_bytes()).unwrap();
        f.write_all(name.as_bytes()).unwrap();
        f.write_all(&(data.len() as u32 * 4).to_le_bytes()).unwrap();
        for v in data {
            f.write_all(&v.to_le_bytes()).unwrap();
        }
    }
}

impl Probe {
    pub fn new(slm: &LoadedSlm, flags: Rounding, max_pos: usize, vocab: usize) -> Self {
        let a = slm.arch();
        let hidden = a.embedding_length as usize;
        let ff = a.feed_forward_length as usize;
        let head_dim = a.head_dim as usize;
        let q_total = a.head_count as usize * head_dim;
        let kv_total = a.head_count_kv as usize * head_dim;
        let table = crate::inference::ops_transformer::RopeTable::new(
            head_dim,
            max_pos,
            a.rope_freq_base,
            crate::inference::ops_transformer::RopePairing::for_arch(a.architecture),
        );
        Self {
            flags,
            q8: vec![0; q8_k_byte_size(hidden.max(ff)).unwrap()],
            dq: vec![0.0; hidden.max(ff)],
            x: vec![0.0; hidden],
            res: vec![0.0; hidden],
            xn: vec![0.0; hidden],
            gamma: vec![0.0; hidden],
            q: vec![0.0; q_total],
            k: vec![0.0; kv_total],
            v: vec![0.0; kv_total],
            attn: vec![0.0; q_total],
            proj: vec![0.0; hidden.max(ff).max(kv_total).max(q_total)],
            gate: vec![0.0; ff],
            up: vec![0.0; ff],
            kv_k: vec![Vec::new(); a.block_count as usize],
            kv_v: vec![Vec::new(); a.block_count as usize],
            pos: 0,
            scratch: vec![0.0; max_pos],
            logits: vec![0.0; vocab],
            cos_sin: table.cos_sin,
            name: String::new(),
            trace: None,
            dither: 0,
        }
    }

    fn matmul(&mut self, slm: &LoadedSlm, name: &str, acts: &[f32], rows: usize, out: &mut [f32]) {
        let (info, w) = slm.tensor_info_and_bytes(name).unwrap_or_else(|| panic!("{name}"));
        let ty = GgmlType(info.ggml_type);
        let cols = acts.len();
        let row_bytes = quant_row_bytes(ty, cols).unwrap();
        let q8_len = q8_k_byte_size(cols).unwrap();
        if ty == GgmlType::Q6_K && self.flags.q6_f32 {
            for (r, row) in w.chunks_exact(row_bytes).take(rows).enumerate() {
                dequantize_row_any(ty, row, &mut self.dq[..cols]).unwrap();
                let mut acc = 0.0f32;
                for i in 0..cols {
                    acc += self.dq[i] * acts[i];
                }
                out[r] = acc;
            }
            return;
        }
        quantize_row_q8_k(acts, &mut self.q8[..q8_len]).unwrap();
        let q8 = &self.q8[..q8_len];
        match ty {
            GgmlType::Q4_K => {
                for (r, row) in w.chunks_exact(row_bytes).take(rows).enumerate() {
                    out[r] = vec_dot_q4_k_q8_k(row, q8).unwrap();
                }
            }
            GgmlType::Q6_K => {
                for (r, row) in w.chunks_exact(row_bytes).take(rows).enumerate() {
                    out[r] = vec_dot_q6_k_q8_k(row, q8);
                }
            }
            other => panic!("{name}: {other:?}"),
        }
    }

    fn norm(&mut self, slm: &LoadedSlm, name: &str) {
        let bytes = slm.tensor_bytes(name).unwrap();
        for (g, c) in self.gamma.iter_mut().zip(bytes.chunks_exact(4)) {
            let v = f32::from_le_bytes([c[0], c[1], c[2], c[3]]);
            *g = if self.flags.gamma { fp16(v) } else { v };
        }
        let n = self.x.len();
        let mean = if self.flags.rms_f32 {
            let mut s = 0.0f32;
            for &v in &self.x {
                s += v * v;
            }
            s / n as f32
        } else {
            let mut s = 0.0f64;
            for &v in &self.x {
                s += (v * v) as f64;
            }
            (s / n as f64) as f32
        };
        let inv = 1.0 / sqrtf(mean + 1e-6);
        for i in 0..n {
            self.xn[i] = self.x[i] * inv * self.gamma[i];
        }
        round_if(self.flags.norm, &mut self.xn);
    }

    fn project(&mut self, slm: &LoadedSlm, layer: usize, which: &str, rows: usize) -> Vec<f32> {
        let acts = core::mem::take(&mut self.xn);
        let mut out = vec![0.0f32; rows];
        let w = format!("blk.{layer}.attn_{which}.weight");
        self.matmul(slm, &w, &acts, rows, &mut out);
        self.xn = acts;
        let b = format!("blk.{layer}.attn_{which}.bias");
        if let Some(bytes) = slm.tensor_bytes(&b) {
            for (o, c) in out.iter_mut().zip(bytes.chunks_exact(4)) {
                *o += f32::from_le_bytes([c[0], c[1], c[2], c[3]]);
            }
        }
        out
    }

    /// One decode step at the next position.
    pub fn step(&mut self, slm: &LoadedSlm, token: u32) {
        let a = slm.arch();
        let hidden = a.embedding_length as usize;
        let ff = a.feed_forward_length as usize;
        let head_dim = a.head_dim as usize;
        let n_q = a.head_count as usize;
        let n_kv = a.head_count_kv as usize;
        let halfd = head_dim / 2;
        let f = self.flags;
        let pos = self.pos;

        let (info, table) = slm.tensor_info_and_bytes("token_embd.weight").unwrap();
        let ty = GgmlType(info.ggml_type);
        let rb = quant_row_bytes(ty, hidden).unwrap();
        let row = &table[token as usize * rb..(token as usize + 1) * rb];
        dequantize_row_any(ty, row, &mut self.dq[..hidden]).unwrap();
        self.x.copy_from_slice(&self.dq[..hidden]);
        round_if(f.x, &mut self.x);
        trace(&mut self.trace, "inp_embd", &self.x);

        let rot = pos * head_dim;
        for layer in 0..a.block_count as usize {
            self.res.copy_from_slice(&self.x);
            self.name = format!("blk.{layer}.attn_norm.weight");
            let nm = core::mem::take(&mut self.name);
            self.norm(slm, &nm);
            self.name = nm;
            trace(&mut self.trace, &format!("attn_norm-{layer}"), &self.xn);

            self.q = self.project(slm, layer, "q", n_q * head_dim);
            self.k = self.project(slm, layer, "k", n_kv * head_dim);
            self.v = self.project(slm, layer, "v", n_kv * head_dim);
            round_if(f.pre_rope, &mut self.q);
            round_if(f.pre_rope, &mut self.k);
            round_if(true, &mut self.v);
            for buf in [&mut self.q, &mut self.k] {
                for vec in buf.chunks_exact_mut(head_dim) {
                    for i in 0..halfd {
                        let c = self.cos_sin[rot + 2 * i];
                        let s = self.cos_sin[rot + 2 * i + 1];
                        let (v0, v1) = (vec[i], vec[i + halfd]);
                        vec[i] = fp16(v0 * c - v1 * s);
                        vec[i + halfd] = fp16(v0 * s + v1 * c);
                    }
                }
            }
            trace(&mut self.trace, &format!("Qcur-{layer}"), &self.q);
            trace(&mut self.trace, &format!("Kcur-{layer}"), &self.k);
            trace(&mut self.trace, &format!("Vcur-{layer}"), &self.v);
            self.kv_k[layer].extend_from_slice(&self.k);
            self.kv_v[layer].extend_from_slice(&self.v);

            let seq = pos + 1;
            let per_pos = n_kv * head_dim;
            let group = n_q / n_kv;
            let inv = 1.0 / sqrtf(head_dim as f32);
            for hq in 0..n_q {
                let hkv = hq / group;
                let qr = &self.q[hq * head_dim..(hq + 1) * head_dim];
                let mut max = f32::NEG_INFINITY;
                for t in 0..seq {
                    let k = &self.kv_k[layer][t * per_pos + hkv * head_dim..][..head_dim];
                    let mut acc = 0.0f32;
                    for d in 0..head_dim {
                        acc += qr[d] * k[d];
                    }
                    let l = acc * inv;
                    self.scratch[t] = l;
                    max = max.max(l);
                }
                let mut denom = 0.0f32;
                for t in 0..seq {
                    let e = libm::expf(self.scratch[t] - max);
                    self.scratch[t] = e;
                    denom += e;
                }
                for t in 0..seq {
                    self.scratch[t] /= denom;
                }
                for d in 0..head_dim {
                    let mut acc = 0.0f32;
                    for t in 0..seq {
                        acc += self.scratch[t] * self.kv_v[layer][t * per_pos + hkv * head_dim + d];
                    }
                    self.attn[hq * head_dim + d] = acc;
                }
            }
            round_if(f.attn, &mut self.attn);
            trace(&mut self.trace, &format!("kqv_out-{layer}"), &self.attn);

            let attn = core::mem::take(&mut self.attn);
            let mut proj = core::mem::take(&mut self.proj);
            self.matmul(slm, &format!("blk.{layer}.attn_output.weight"), &attn, hidden, &mut proj[..hidden]);
            self.attn = attn;
            for i in 0..hidden {
                self.x[i] = self.res[i] + proj[i];
            }
            dither(&mut self.dither, &mut self.x);
            round_if(f.x, &mut self.x);

            trace(&mut self.trace, &format!("ffn_inp-{layer}"), &self.x);
            self.res.copy_from_slice(&self.x);
            self.norm(slm, &format!("blk.{layer}.ffn_norm.weight"));
            trace(&mut self.trace, &format!("ffn_norm-{layer}"), &self.xn);
            let acts = core::mem::take(&mut self.xn);
            let mut gate = core::mem::take(&mut self.gate);
            let mut up = core::mem::take(&mut self.up);
            self.matmul(slm, &format!("blk.{layer}.ffn_gate.weight"), &acts, ff, &mut gate);
            self.matmul(slm, &format!("blk.{layer}.ffn_up.weight"), &acts, ff, &mut up);
            self.xn = acts;
            trace(&mut self.trace, &format!("ffn_gate-{layer}"), &gate);
            trace(&mut self.trace, &format!("ffn_up-{layer}"), &up);
            round_if(f.mlp, &mut gate);
            round_if(f.mlp, &mut up);
            for i in 0..ff {
                let g = gate[i];
                let mut s = g / (1.0 + libm::expf(-g));
                if f.mlp {
                    s = fp16(s);
                }
                gate[i] = s * up[i];
            }
            round_if(f.mlp, &mut gate);
            self.matmul(slm, &format!("blk.{layer}.ffn_down.weight"), &gate, hidden, &mut proj[..hidden]);
            round_if(f.mlp, &mut proj[..hidden]);
            trace(&mut self.trace, &format!("ffn_out-{layer}"), &proj[..hidden]);
            self.gate = gate;
            self.up = up;
            for i in 0..hidden {
                self.x[i] = self.res[i] + proj[i];
            }
            dither(&mut self.dither, &mut self.x);
            round_if(f.x, &mut self.x);
            trace(&mut self.trace, &format!("l_out-{layer}"), &self.x);
            self.proj = proj;
        }
        self.pos += 1;
        self.norm(slm, "output_norm.weight");
        trace(&mut self.trace, "result_norm", &self.xn);
        let acts = core::mem::take(&mut self.xn);
        let mut logits = core::mem::take(&mut self.logits);
        let rows = logits.len();
        self.matmul(slm, "output.weight", &acts, rows, &mut logits);
        self.xn = acts;
        trace(&mut self.trace, "result_output", &logits);
        self.logits = logits;
    }
}
