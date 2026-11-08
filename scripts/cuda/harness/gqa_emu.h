/*
 * gqa_emu.h - run scripts/cuda/slm/slm_gqa_hmma.cu lane by lane on the CPU (#1167).
 *
 * The host has no GPU, so the kernel bodies are checked here before they reach
 * the board: every lane is a ucontext coroutine; warp collectives (mma.sync,
 * ldmatrix, shuffles, votes) and __syncthreads complete when all lanes that
 * must take part have arrived, exactly as on the GPU, and a lane that arrives
 * at a different collective than its warp is reported as divergence.
 *
 * Register layouts follow the PTX ISA (mma.m16n8k16 as pinned on hardware by
 * slm_forward's mma_layout test; ldmatrix .trans). mma products are exact and
 * summed in double, then rounded to FP32; exp2 is exp2f with subnormal results
 * flushed (ex2.approx.ftz); FP16 conversions round to nearest even (_Float16).
 *
 * Scheduling: GQ_EMU_LOCKSTEP (default) advances every warp by one
 * collective per pass; GQ_EMU_SERIAL and GQ_EMU_SERIAL_REV run each warp, in
 * order or in reverse, on to its next __syncthreads before the next warp
 * starts, the most skew between warps that the barriers allow.
 *
 * cp.async: in mode GQ_EMU_CP_LATE (default) a copy lands only when the
 * issuing lane waits for its group, so a read that runs ahead of the wait and
 * barrier sees stale data; in GQ_EMU_CP_EARLY it lands at issue, so with
 * serial scheduling a refill that overtakes a warp still reading the old tile
 * corrupts it. A correct kernel passes every combination. Shared memory
 * starts filled with NaN bytes.
 *
 * Include this, then define SLM_GQA_EMU and include the kernel source.
 */
#ifndef GQA_EMU_H
#define GQA_EMU_H

#include <ucontext.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <vector>

#define GQ_FN static inline

struct alignas(16) gq_u4 {
    uint32_t x, y, z, w;
};

namespace gqemu {

enum Kind { K_NONE, K_MMA, K_LDSM, K_SHFL, K_ANY, K_SYNCWARP, K_SYNCTHREADS };
enum CpMode { GQ_EMU_CP_LATE, GQ_EMU_CP_EARLY };
enum Sched { GQ_EMU_LOCKSTEP, GQ_EMU_SERIAL, GQ_EMU_SERIAL_REV };

struct Copy {
    void *dst;
    const void *src;
    bool ok;
};

struct Lane {
    ucontext_t ctx;
    std::vector<char> stack;
    Kind wait = K_NONE;
    bool done = false;
    uint32_t a[4], b[2], r[4];
    float c[4];
    const void *addr;
    float fv;
    int fm;
    bool pv;
    std::vector<Copy> open;
    std::deque<std::vector<Copy>> groups;
};

struct Cta {
    int nw = 0;
    std::vector<Lane> lanes;
    ucontext_t sched;
    int cur = -1;
    std::function<void(int, int)> body;
};

inline Cta *&cta()
{
    static Cta *p = nullptr;
    return p;
}
inline CpMode &cp_mode()
{
    static CpMode m = GQ_EMU_CP_LATE;
    return m;
}
inline Sched &sched_mode()
{
    static Sched s = GQ_EMU_LOCKSTEP;
    return s;
}
inline long &n_mma()
{
    static long n = 0;
    return n;
}

[[noreturn]] inline void die(const char *why)
{
    fprintf(stderr, "gqa_emu: %s\n", why);
    exit(4);
}

inline Lane &me() { return cta()->lanes[cta()->cur]; }

inline void arrive(Kind k)
{
    Lane &l = me();
    l.wait = k;
    swapcontext(&l.ctx, &cta()->sched);
}

inline float h2f(uint16_t h)
{
    _Float16 v;
    memcpy(&v, &h, 2);
    return (float)v;
}
inline uint16_t f2h(float f)
{
    const _Float16 v = (_Float16)f;
    uint16_t h;
    memcpy(&h, &v, 2);
    return h;
}

inline void apply(const std::vector<Copy> &g)
{
    for (const Copy &c : g) {
        if (c.ok) {
            memcpy(c.dst, c.src, 16);
        } else {
            memset(c.dst, 0, 16);
        }
    }
}

inline void resolve_warp(Cta &C, int w, Kind k)
{
    Lane *L = &C.lanes[32 * w];
    switch (k) {
    case K_MMA: {
        double A[16][16], B[16][8], D[16][8];
        for (int t = 0; t < 32; t++) {
            const int g = t >> 2, c = t & 3;
            const uint32_t *a = L[t].a;
            A[g][2 * c] = h2f(a[0] & 0xFFFF);
            A[g][2 * c + 1] = h2f(a[0] >> 16);
            A[g + 8][2 * c] = h2f(a[1] & 0xFFFF);
            A[g + 8][2 * c + 1] = h2f(a[1] >> 16);
            A[g][2 * c + 8] = h2f(a[2] & 0xFFFF);
            A[g][2 * c + 9] = h2f(a[2] >> 16);
            A[g + 8][2 * c + 8] = h2f(a[3] & 0xFFFF);
            A[g + 8][2 * c + 9] = h2f(a[3] >> 16);
            B[2 * c][g] = h2f(L[t].b[0] & 0xFFFF);
            B[2 * c + 1][g] = h2f(L[t].b[0] >> 16);
            B[2 * c + 8][g] = h2f(L[t].b[1] & 0xFFFF);
            B[2 * c + 9][g] = h2f(L[t].b[1] >> 16);
            D[g][2 * c] = L[t].c[0];
            D[g][2 * c + 1] = L[t].c[1];
            D[g + 8][2 * c] = L[t].c[2];
            D[g + 8][2 * c + 1] = L[t].c[3];
        }
        float R[16][8];
        for (int i = 0; i < 16; i++) {
            for (int j = 0; j < 8; j++) {
                double s = D[i][j];
                for (int kk = 0; kk < 16; kk++) {
                    s += A[i][kk] * B[kk][j];
                }
                R[i][j] = (float)s;
            }
        }
        for (int t = 0; t < 32; t++) {
            const int g = t >> 2, c = t & 3;
            L[t].c[0] = R[g][2 * c];
            L[t].c[1] = R[g][2 * c + 1];
            L[t].c[2] = R[g + 8][2 * c];
            L[t].c[3] = R[g + 8][2 * c + 1];
        }
        n_mma()++;
        break;
    }
    case K_LDSM:
        for (int t = 0; t < 32; t++) {
            for (int i = 0; i < 4; i++) {
                const uint16_t *r0 = (const uint16_t *)L[8 * i + 2 * (t & 3)].addr;
                const uint16_t *r1 = (const uint16_t *)L[8 * i + 2 * (t & 3) + 1].addr;
                L[t].r[i] = (uint32_t)r0[t >> 2] | ((uint32_t)r1[t >> 2] << 16);
            }
        }
        break;
    case K_SHFL: {
        float v[32];
        for (int t = 0; t < 32; t++) {
            v[t] = L[t].fv;
        }
        for (int t = 0; t < 32; t++) {
            L[t].fv = v[t ^ L[t].fm];
        }
        break;
    }
    case K_ANY: {
        bool any = false;
        for (int t = 0; t < 32; t++) {
            any = any || L[t].pv;
        }
        for (int t = 0; t < 32; t++) {
            L[t].pv = any;
        }
        break;
    }
    default:
        break;
    }
    for (int t = 0; t < 32; t++) {
        L[t].wait = K_NONE;
    }
}

inline void entry()
{
    Cta &C = *cta();
    const int i = C.cur;
    C.body(i & 31, i >> 5);
    C.lanes[i].done = true;
    swapcontext(&C.lanes[i].ctx, &C.sched);
}

/* Run one CTA of nw warps; body(lane, warp). smem (bytes) is filled with NaN. */
inline void run_cta(int nw, void *smem, size_t smem_bytes, std::function<void(int, int)> body)
{
    memset(smem, 0xFF, smem_bytes);
    Cta C;
    C.nw = nw;
    C.lanes.resize(32 * nw);
    C.body = std::move(body);
    cta() = &C;
    for (int i = 0; i < 32 * nw; i++) {
        Lane &l = C.lanes[i];
        l.stack.resize(256 * 1024);
        getcontext(&l.ctx);
        l.ctx.uc_stack.ss_sp = l.stack.data();
        l.ctx.uc_stack.ss_size = l.stack.size();
        l.ctx.uc_link = nullptr;
        makecontext(&l.ctx, (void (*)())entry, 0);
    }
    /* Run warp w's lanes until each waits or exits. */
    auto run_lanes = [&C](int w) {
        for (int i = 32 * w; i < 32 * w + 32; i++) {
            Lane &l = C.lanes[i];
            if (!l.done && l.wait == K_NONE) {
                C.cur = i;
                swapcontext(&C.sched, &l.ctx);
            }
        }
    };
    /* Complete warp w's collective if all its lanes are at one. */
    auto resolve = [&C](int w) {
        const Lane *L = &C.lanes[32 * w];
        int wd = 0, wb = 0;
        for (int t = 0; t < 32; t++) {
            wd += L[t].done;
            wb += L[t].wait == K_SYNCTHREADS;
        }
        if (wd + wb == 32) {
            return false; /* nothing at a warp collective */
        }
        if (wd != 0 || wb != 0) {
            die("warp collective missing lanes (exited or at __syncthreads)");
        }
        const Kind k = L[0].wait;
        for (int t = 1; t < 32; t++) {
            if (L[t].wait != k) {
                die("lanes of one warp at different collectives");
            }
        }
        resolve_warp(C, w, k);
        return true;
    };
    for (;;) {
        bool moved = false;
        if (sched_mode() == GQ_EMU_LOCKSTEP) {
            for (int w = 0; w < nw; w++) {
                run_lanes(w);
            }
            for (int w = 0; w < nw; w++) {
                moved = resolve(w) || moved;
            }
        } else {
            /* Each warp runs on to its next __syncthreads before the next
             * warp starts: the most skew a barrier allows. */
            for (int j = 0; j < nw; j++) {
                const int w = sched_mode() == GQ_EMU_SERIAL ? j : nw - 1 - j;
                for (;;) {
                    run_lanes(w);
                    if (!resolve(w)) {
                        break;
                    }
                    moved = true;
                }
            }
        }
        int done = 0;
        int at_bar = 0;
        for (const Lane &l : C.lanes) {
            done += l.done;
            at_bar += l.wait == K_SYNCTHREADS;
        }
        if (done == 32 * nw) {
            break;
        }
        if (!moved) {
            if (done != 0) {
                die("__syncthreads with some threads exited");
            }
            if (at_bar != 32 * nw) {
                die("deadlock");
            }
            for (Lane &l : C.lanes) {
                l.wait = K_NONE;
            }
        }
    }
    for (Lane &l : C.lanes) {
        if (!l.open.empty() || !l.groups.empty()) {
            die("cp.async left pending at exit");
        }
    }
    cta() = nullptr;
}

} // namespace gqemu

GQ_FN void gq_mma(float (&c)[4], uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t b0,
                  uint32_t b1)
{
    gqemu::Lane &l = gqemu::me();
    l.a[0] = a0, l.a[1] = a1, l.a[2] = a2, l.a[3] = a3, l.b[0] = b0, l.b[1] = b1;
    memcpy(l.c, c, sizeof(l.c));
    gqemu::arrive(gqemu::K_MMA);
    memcpy(c, gqemu::me().c, sizeof(l.c));
}
GQ_FN void gq_ldsm_x4_t(uint32_t (&r)[4], const void *row)
{
    gqemu::me().addr = row;
    gqemu::arrive(gqemu::K_LDSM);
    memcpy(r, gqemu::me().r, sizeof(r));
}
GQ_FN void gq_cp16(void *dst, const void *src, bool ok)
{
    const gqemu::Copy c{dst, src, ok};
    if (gqemu::cp_mode() == gqemu::GQ_EMU_CP_EARLY) {
        gqemu::apply({c});
    }
    /* Tracked in both modes, so a group left without a wait is reported. */
    gqemu::me().open.push_back(c);
}
GQ_FN void gq_cp_commit()
{
    gqemu::Lane &l = gqemu::me();
    l.groups.push_back(l.open);
    l.open.clear();
}
static inline void gq_cp_wait_n(size_t n)
{
    gqemu::Lane &l = gqemu::me();
    while (l.groups.size() > n) {
        if (gqemu::cp_mode() == gqemu::GQ_EMU_CP_LATE) {
            gqemu::apply(l.groups.front());
        }
        l.groups.pop_front();
    }
}
GQ_FN void gq_cp_wait1() { gq_cp_wait_n(1); }
GQ_FN void gq_cp_wait0() { gq_cp_wait_n(0); }

GQ_FN float gq_ex2(float x)
{
    const float y = exp2f(x);
    return y < 1.17549435e-38f ? 0.0f : y;
}
GQ_FN float gq_shfl_xor(float v, int m)
{
    gqemu::me().fv = v;
    gqemu::me().fm = m;
    gqemu::arrive(gqemu::K_SHFL);
    return gqemu::me().fv;
}
GQ_FN bool gq_any(bool p)
{
    gqemu::me().pv = p;
    gqemu::arrive(gqemu::K_ANY);
    return gqemu::me().pv;
}
GQ_FN void gq_syncwarp() { gqemu::arrive(gqemu::K_SYNCWARP); }
GQ_FN void gq_syncthreads() { gqemu::arrive(gqemu::K_SYNCTHREADS); }
GQ_FN gq_u4 gq_ld16(const void *p)
{
    if (((uintptr_t)p & 15) != 0) {
        gqemu::die("misaligned 16-byte load");
    }
    gq_u4 v;
    memcpy(&v, p, 16);
    return v;
}
GQ_FN uint32_t gq_pack(float lo, float hi)
{
    return (uint32_t)gqemu::f2h(lo) | ((uint32_t)gqemu::f2h(hi) << 16);
}
GQ_FN float gq_hsum(uint32_t x) { return gqemu::h2f(x & 0xFFFF) + gqemu::h2f(x >> 16); }
GQ_FN uint32_t gq_f2h(float v) { return gqemu::f2h(v); }
GQ_FN float gq_h2f(uint32_t h) { return gqemu::h2f((uint16_t)h); }

#endif /* GQA_EMU_H */
