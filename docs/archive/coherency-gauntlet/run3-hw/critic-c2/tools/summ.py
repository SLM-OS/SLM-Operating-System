import json, sys, os
J = sys.argv[1]
UNI = [0.1, 0.1, 0.3, 0.5, 1, 3, 5, 10, 30, 50, 0]  # uniform phase over a 10 ms tick


def pct(v):
    s = sum(v) or 1
    return [round(100.0 * x / s, 1) for x in v]


def fmt_us(x):
    if x is None:
        return "-"
    if x >= 10**12:
        return "inf"
    return "%d" % x


for cell in sys.argv[2:]:
    o = json.load(open(os.path.join(J, cell + ".json")))
    print("=" * 30, cell, "n=%d" % o["n"], o["files"][0], "..", o["files"][-1])
    print("problems:", o["problems"] or "none")
    print("chk/att-diag nonzero:", o["chk"] or "none")
    P = o["per"]
    print("cpu | share med [min,max] | att sum (med) | ok sum | stale% pd% empty% other% iso% | pd% run-med | 1st-ok med/max excl (us) | med cens | zero-ok runs")
    for c in map(str, range(6)):
        p = P[c]
        f = p["frac"]
        print("%s | %.1f [%.1f,%.1f] | %d (%s) | %d | %.1f %.1f %.1f %.1f %.1f | %s | %s / %s | %s | %s" % (
            c, p["share_med"], p["share_min"], p["share_max"], p["att_sum"], p["att_med"], p["ok_sum"],
            f["f_stale"], f["f_pd"], f["f_empty"], f["f_other"], f["iso"],
            "%.1f" % p["fpd_run_med"] if p["fpd_run_med"] is not None else "-",
            fmt_us(p["first_ok_med_excl"]), fmt_us(p["first_ok_max_excl"]), fmt_us(p["first_ok_med_cens"]),
            p["zero_runs"]))
    print("cpu | w_in T,C,P,O,B (unk) | w_in%% pooled / run-med | w_off%% <1,<2,<4,<8,<16,<32,<64,>=64us")
    for c in map(str, range(6)):
        p = P[c]
        print("%s | %s (%d) | %.1f / %s | %s" % (c, p["w_in"], p["w_in_unk"], p["w_in_pct"],
              "%.1f" % p["w_in_run_med"] if p["w_in_run_med"] is not None else "-", pct(p["w_off"]) if sum(p["w_off"]) else "-"))
    print("phase %% bins <10,<20,<50,<100,<200,<500us,<1,<2,<5,<10,>=10ms ; uniform ref", UNI)
    for k in ("ph_w", "ph_w_pd", "ph_own"):
        for c in map(str, range(6)):
            v = P[c][k]
            if sum(v):
                print("  %-7s cpu%s n=%-6d %s" % (k, c, sum(v), pct(v)))
    p1 = P["1"]
    print("cpu1 windows: win_n T,C,P,O,B", p1["win_n"], "win_sum_us", p1["win_sum_us"], "win_max_us", p1["win_max_us"])
    print("cpu1 wph %", pct(p1["wph"]), "n=", sum(p1["wph"]))
    print("cpu1 wlog (n, med ns, max ns) by reason:", o["wlog"])
    # Q1
    thieves = ["0", "2", "3", "4", "5"]
    best = max(thieves, key=lambda c: P[c]["share_med"])
    b = P[best]["share_med"]
    s2, s3 = P["2"]["share_med"], P["3"]["share_med"]
    zero = {c: P[c]["zero_runs"] for c in thieves if P[c]["zero_runs"]}
    share_clause = s2 <= 0.6 * b and s3 <= 0.6 * b
    print("Q1: best thief cpu%s med %.2f; 0.6x=%.2f; cpu2 %.2f (%.2f of best), cpu3 %.2f (%.2f of best); share clause %s; zero-ok non-owner runs %s -> %s" % (
        best, b, 0.6 * b, s2, s2 / b, s3, s3 / b, share_clause, zero or "none",
        "pre-existing" if (share_clause or zero) else "exposed by the removal"))
    print("Q2 B-side: cpu2/3 share med %.2f/%.2f vs cpu4/5 %.2f/%.2f; pd%% pooled cpu2 %.1f cpu3 %.1f; 1st-ok med excl cpu2 %s cpu3 %s; cens %s %s" % (
        s2, s3, P["4"]["share_med"], P["5"]["share_med"], P["2"]["frac"]["f_pd"], P["3"]["frac"]["f_pd"],
        fmt_us(P["2"]["first_ok_med_excl"]), fmt_us(P["3"]["first_ok_med_excl"]),
        fmt_us(P["2"]["first_ok_med_cens"]), fmt_us(P["3"]["first_ok_med_cens"])))
