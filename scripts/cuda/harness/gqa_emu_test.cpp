/*
 * gqa_emu_test.cpp - the HMMA GQA kernels on the host, lane by lane (#1167).
 *
 * Runs the decode body (B * group <= 16) and the prefill body (every case) of
 * scripts/cuda/slm/slm_gqa_hmma.cu, each with split P (the default
 * kernels) and FP16 P (the _p16 kernels), through gqa_emu.h on every case of
 * gqa_check.h against the exact reference. Split-P kernels also run with
 * warps serialised between barriers, prefill with cp.async landing both at
 * issue and at the wait (see gqa_emu.h). Every buffer ends at a PROT_NONE
 * page, so a read past the end of K, V, q, pos or out faults.
 *
 *   g++ -O2 -std=c++17 -fopenmp -I. -I../slm gqa_emu_test.cpp -o gqa_emu_test
 *   ./gqa_emu_test [--quick]
 *
 * -DGQA_KERNEL_SRC='"file.cu"' runs another copy of the kernel source (the
 * mutation check in gqa-emu-test.sh). Exit status 0 when every check passes,
 * 1 otherwise.
 */
#include <sys/mman.h>
#include <unistd.h>

#include "gqa_emu.h"
#define SLM_GQA_EMU 1
#ifndef GQA_KERNEL_SRC
#define GQA_KERNEL_SRC "slm_gqa_hmma.cu"
#endif
#include GQA_KERNEL_SRC
#include "gqa_check.h"

/* n bytes whose end touches a PROT_NONE page. */
template <typename T> static T *guarded(const std::vector<T> &init, size_t n_elems)
{
    const size_t pg = (size_t)sysconf(_SC_PAGESIZE);
    const size_t bytes = n_elems * sizeof(T);
    const size_t span = (bytes + pg - 1) / pg * pg;
    uint8_t *base = (uint8_t *)mmap(nullptr, span + pg, PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) {
        perror("mmap");
        exit(3);
    }
    mprotect(base + span, pg, PROT_NONE);
    T *p = (T *)(base + span - bytes);
    if (!init.empty()) {
        memcpy(p, init.data(), bytes);
    }
    return p;
}

template <typename T> static void unguard(T *p, size_t n_elems)
{
    const size_t pg = (size_t)sysconf(_SC_PAGESIZE);
    const size_t bytes = n_elems * sizeof(T);
    const size_t span = (bytes + pg - 1) / pg * pg;
    munmap((uint8_t *)p + bytes - span, span + pg);
}

struct Bufs {
    uint16_t *q, *k, *v, *out;
    uint32_t *pos;
    size_t nq, nkv;
    int B;
};

static Bufs upload(const GqaData &d)
{
    Bufs b;
    b.B = d.B;
    b.nq = d.q.size();
    b.nkv = d.k.size();
    b.q = guarded(d.q, b.nq);
    b.k = guarded(d.k, b.nkv);
    b.v = guarded(d.v, b.nkv);
    b.pos = guarded(d.pos, d.pos.size());
    b.out = guarded(std::vector<uint16_t>(b.nq, 0x7E00), b.nq);
    return b;
}

static void release(Bufs &b)
{
    unguard(b.q, b.nq);
    unguard(b.k, b.nkv);
    unguard(b.v, b.nkv);
    unguard(b.pos, (size_t)b.B);
    unguard(b.out, b.nq);
}

template <bool PX> static std::vector<uint16_t> run_dec(const GqaShape &S, Bufs &b)
{
    static gq_dec_smem sm;
    for (int hkv = 0; hkv < S.nhkv; hkv++) {
        for (int sl = 0; sl < GQ_HD / GQ_DEC_DS; sl++) {
            gqemu::run_cta(GQ_DEC_WARPS, &sm, sizeof(sm), [&](int lane, int w) {
                gqa_dec_body<PX>(b.q, b.k, b.v, b.pos, b.out, S.nhq, S.nhkv, S.hd, b.B, lane, w,
                                 hkv, sl, &sm);
            });
        }
    }
    return std::vector<uint16_t>(b.out, b.out + b.nq);
}

template <bool PX> static std::vector<uint16_t> run_pf(const GqaShape &S, Bufs &b)
{
    static gq_pf_smem sm;
    const int mt = (b.B * (S.nhq / S.nhkv) + 15) / 16;
    const int gy = (mt + GQ_PF_WARPS - 1) / GQ_PF_WARPS;
    for (int hkv = 0; hkv < S.nhkv; hkv++) {
        for (int y = 0; y < gy; y++) {
            gqemu::run_cta(GQ_PF_WARPS, &sm, sizeof(sm), [&](int lane, int w) {
                gqa_pf_body<PX>(b.q, b.k, b.v, b.pos, b.out, S.nhq, S.nhkv, S.hd, b.B, lane, w,
                                hkv, y, &sm);
            });
        }
    }
    return std::vector<uint16_t>(b.out, b.out + b.nq);
}

int main(int argc, char **argv)
{
    bool quick = false;
    std::string only;
    for (int i = 1; i < argc; i++) {
        if (std::string(argv[i]) == "--quick") {
            quick = true;
        } else if (std::string(argv[i]) == "--only" && i + 1 < argc) {
            only = argv[++i]; /* cases whose name contains this */
        }
    }
    const GqaShape S{};
    const int group = S.nhq / S.nhkv;
    int fails = 0, checks = 0;
    printf("gqa_emu_test: %s, nhq %d nhkv %d hd %d%s\n", GQA_KERNEL_SRC, S.nhq, S.nhkv, S.hd,
           quick ? ", quick (kv <= 600)" : "");
    for (const GqaCase &c : gqa_cases(!quick)) {
        if ((quick && c.cap > 600) || c.name.find(only) == std::string::npos) {
            continue;
        }
        const GqaData d = gqa_make(S, c, gqa_seed(c.name));
        std::vector<double> ref;
        gqa_ref_exact(S, d, ref);
        Bufs b = upload(d);
        auto check = [&](const char *name, const std::vector<uint16_t> &got, bool ulp_rule) {
            const GqaErr e = gqa_compare(got, ref, ulp_rule);
            gqa_report(name, c, e, ulp_rule);
            fails += !e.pass;
            checks++;
            std::fill(b.out, b.out + b.nq, (uint16_t)0x7E00);
        };
        /* Suffix: scheduling (lockstep, serial, reverse serial) and cp.async
         * landing (late, early), see gqa_emu.h. */
        auto mode = [](gqemu::Sched s, gqemu::CpMode m) {
            gqemu::sched_mode() = s;
            gqemu::cp_mode() = m;
        };
        mode(gqemu::GQ_EMU_LOCKSTEP, gqemu::GQ_EMU_CP_LATE);
        if ((int)c.pos.size() * group <= 16) {
            check("hmma_dec", run_dec<true>(S, b), true);
            check("hmma_dec_p16", run_dec<false>(S, b), false);
            mode(gqemu::GQ_EMU_SERIAL_REV, gqemu::GQ_EMU_CP_LATE);
            check("hmma_dec/R", run_dec<true>(S, b), true);
            mode(gqemu::GQ_EMU_LOCKSTEP, gqemu::GQ_EMU_CP_LATE);
        }
        check("hmma_pf", run_pf<true>(S, b), true);
        check("hmma_pf_p16", run_pf<false>(S, b), false);
        mode(gqemu::GQ_EMU_SERIAL, gqemu::GQ_EMU_CP_EARLY);
        check("hmma_pf/SE", run_pf<true>(S, b), true);
        mode(gqemu::GQ_EMU_SERIAL_REV, gqemu::GQ_EMU_CP_LATE);
        check("hmma_pf/RL", run_pf<true>(S, b), true);
        release(b);
        fflush(stdout);
    }
    printf("gqa_emu_test: %d of %d checks failed (%ld mma emulated)\n", fails, checks,
           gqemu::n_mma());
    return fails ? 1 : 0;
}
