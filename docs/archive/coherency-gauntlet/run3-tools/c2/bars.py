#!/usr/bin/env python3
"""Coherency run 3, C2: the numbers each bar clause names, from files only.

bars.py q1 --head SHA <files...>
    Question 1 (cells C, D, E). Prints CPUs 2 and 3's median share, the
    best thief's median share and 60% of it, and every run in which a
    non-owner CPU had 0 successes. The bar as written in the brief:
    "pre-existing" if CPUs 2 and 3's median share is no better than 60%
    of the best thief's, or any run has a non-owner CPU at 0 successes
    over the full 2.5 s; otherwise "exposed by the removal". Each clause
    is printed separately; the reading is left to the report.

bars.py q2 --head-a SHA --head-b SHA --a <files...> --b <files...>
    Question 2 (B against A). Prints, for CPUs 2 and 3 (and 4 and 5 for
    reference): median share in B and the largest gap to CPUs 4 and 5;
    pooled f_pd / att in A and B; first-success median in A and B; and
    A's window data per thief (share of attempts that started inside
    CPU 1's window, share within 200 us of CPU 1's tick against the
    uniform expectation, tick offset to CPU 1).

Owner CPU 1, thieves 0, 2, 3, 4, 5. Files failing analyze.py's checks
(banner, one line per CPU, chk, att == diag_att, complete v2 fields) are
listed and left out.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import analyze as A  # noqa: E402

OWNER = 1


def load(files, head):
    recs, bad = [], []
    for fn in sorted(files):
        r = A.parse_file(fn)
        if head and (r['banner'] is None or not (
                r['banner'].startswith(head) or head.startswith(r['banner']))):
            r['problems'].append(f'banner {r["banner"]} != head {head}')
        (bad if r['problems'] else recs).append(r)
    return recs, bad


def per_cpu(recs, cpus):
    out = {}
    for c in range(cpus):
        shares, fo, att, fpd, ok0 = [], [], 0, 0, []
        for r in recs:
            tot = sum(A.num(r['cpu'][k].get('exec')) or 0 for k in range(cpus))
            kv = r['cpu'][c]
            shares.append(100.0 * (A.num(kv.get('exec')) or 0) / tot
                          if tot else 0.0)
            f = A.num(kv.get('first_ok_us'))
            if f is not None:
                fo.append(f)
            att += A.num(kv.get('att')) or 0
            fpd += A.num(kv.get('f_pd')) or 0
            if kv.get('ok') == '0':
                ok0.append(r['file'])
        out[c] = {'share': A.med(shares), 'fo_med': A.med(fo), 'fo_n': len(fo),
                  'att': att, 'f_pd': fpd, 'ok0': ok0}
    return out


def window_view(recs, cpus):
    w = OWNER
    per_us, _ = A.watched_period(recs, w)
    uni = A.uniform_ph(per_us or 10000.0)
    out = {}
    for c in range(cpus):
        if c == w:
            continue
        att = inside = 0
        ph = [0] * 11
        offs = []
        for r in recs:
            kv = r['cpu'][c]
            att += A.num(kv.get('att')) or 0
            inside += sum(A.ints(kv.get('w_in')) or []) + (
                A.num(kv.get('w_in_unk')) or 0)
            v = A.ints(kv.get('ph_w'))
            if v:
                ph = [x + y for x, y in zip(ph, v)]
            kw = r['cpu'][w]
            tn, tf, tl = (A.num(kw.get('tick_n')), A.num(kw.get('tick_first_us')),
                          A.num(kw.get('tick_last_us')))
            tfc = A.num(kv.get('tick_first_us'))
            if tn and tn >= 2 and None not in (tf, tl, tfc):
                offs.append((tfc - tf) % ((tl - tf) / (tn - 1)))
        out[c] = {'att': att, 'inside': inside, 'ph': ph, 'offs': offs}
    return out, uni, per_us


def header(label, recs, bad):
    names = [r['file'] for r in recs]
    print(f'{label}: counted {A.frange(names)}')
    for r in bad:
        print(f'  left out `{r["file"]}`: ' + '; '.join(r['problems']))


def q1(a):
    recs, bad = load(a.files, a.head)
    header('Q1', recs, bad)
    if not recs:
        return
    cpus = int(recs[0]['hdr']['cpus'])
    p = per_cpu(recs, cpus)
    thieves = [c for c in range(cpus) if c != OWNER]
    best = max(thieves, key=lambda c: p[c]['share'])
    lim = 0.6 * p[best]['share']
    print(f'  best thief CPU {best}: median share {p[best]["share"]:.2f}%; '
          f'60% of it = {lim:.2f}%')
    for c in (2, 3):
        s = p[c]['share']
        print(f'  CPU {c}: median share {s:.2f}% = '
              f'{s / p[best]["share"]:.3f} of best -> '
              f'{"no better than 60%" if s <= lim else "better than 60%"}')
    zero = sorted({f for c in thieves for f in p[c]['ok0']})
    print(f'  runs with a non-owner CPU at ok=0: {len(zero)} of {len(recs)}'
          + (f': {", ".join(zero)}' if zero else ''))
    for c in thieves:
        if p[c]['ok0']:
            print(f'    CPU {c}: {", ".join(p[c]["ok0"])}')


def q2(a):
    ra, bad_a = load(a.a, a.head_a)
    rb, bad_b = load(a.b, a.head_b)
    header('A', ra, bad_a)
    header('B', rb, bad_b)
    if not ra or not rb:
        return
    cpus = int(ra[0]['hdr']['cpus'])
    if cpus < 6 or int(rb[0]['hdr']['cpus']) < 6:
        print('  needs 6 CPUs (Jetson) in both cells; not evaluated')
        return
    pa, pb = per_cpu(ra, cpus), per_cpu(rb, cpus)
    print('  clause 1 (B: CPUs 2, 3 within 10 pp of CPUs 4, 5, median share):')
    gap = 0.0
    for i in (2, 3):
        for j in (4, 5):
            g = abs(pb[i]['share'] - pb[j]['share'])
            gap = max(gap, g)
            print(f'    CPU {i} {pb[i]["share"]:.2f}% vs CPU {j} '
                  f'{pb[j]["share"]:.2f}%: {g:.2f} pp')
    print(f'    largest gap {gap:.2f} pp -> {"holds" if gap <= 10 else "fails"}')
    print('  clause 2 (f_pd share of attempts, A about 75% -> B under 10%):')
    ok2 = True
    for c in (2, 3, 4, 5):
        fa = 100.0 * pa[c]['f_pd'] / pa[c]['att'] if pa[c]['att'] else None
        fb = 100.0 * pb[c]['f_pd'] / pb[c]['att'] if pb[c]['att'] else None
        print(f'    CPU {c}: A {pa[c]["f_pd"]}/{pa[c]["att"]} = '
              f'{fa if fa is None else round(fa, 1)}%, B {pb[c]["f_pd"]}/'
              f'{pb[c]["att"]} = {fb if fb is None else round(fb, 1)}%')
        if c in (2, 3) and (fb is None or fb >= 10.0):
            ok2 = False
    print(f'    B under 10% on CPUs 2 and 3 -> {"holds" if ok2 else "fails"}'
          ' (A values above are for the "about 75%" context)')
    print('  clause 3 (first-success median, A above 200 ms -> B under 1 ms):')
    ok3 = True
    for c in (2, 3, 4, 5):
        ma, mb = pa[c]['fo_med'], pb[c]['fo_med']
        print(f'    CPU {c}: A {A.fmt_ms(ma)} ms ({pa[c]["fo_n"]} runs), '
              f'B {A.fmt_ms(mb)} ms ({pb[c]["fo_n"]} runs)')
        if c in (2, 3) and not (ma is not None and ma > 200000 and
                                mb is not None and mb < 1000):
            ok3 = False
    print(f'    CPUs 2 and 3 -> {"holds" if ok3 else "fails"}')
    if ra[0]['fmt'] == 'v1':
        print('  clause 4: A files are format v1 (no window data); not '
              'evaluable from them')
        return
    wv, uni, per = window_view(ra, cpus)
    print(f'  clause 4 (A window data; CPU 1 tick period {per or 0:.1f} us; '
          f'uniform phase puts {sum(uni[:5]):.1f}% within 200 us):')
    for c in sorted(wv):
        v = wv[c]
        t = sum(v['ph'])
        o = v['offs']
        print(f'    CPU {c}: inside window {v["inside"]}/{v["att"]} = '
              f'{A.pct(v["inside"], v["att"])}; within 200 us of CPU 1 tick '
              f'{sum(v["ph"][:5])}/{t} = {A.pct(sum(v["ph"][:5]), t)}; '
              f'modal ph_w bin {A.PH_LABELS[v["ph"].index(max(v["ph"]))]} '
              f'({A.pct(max(v["ph"]), t)}); tick offset to CPU 1 median '
              f'{A.med(o) if o else float("nan"):.0f} us '
              f'[{min(o) if o else 0:.0f}, {max(o) if o else 0:.0f}]')
    print('    (clustered vs spread is read from these rows in the report)')


def main():
    ap = argparse.ArgumentParser()
    sp = ap.add_subparsers(dest='q', required=True)
    p1 = sp.add_parser('q1')
    p1.add_argument('--head')
    p1.add_argument('files', nargs='+')
    p2 = sp.add_parser('q2')
    p2.add_argument('--head-a')
    p2.add_argument('--head-b')
    p2.add_argument('--a', nargs='+', required=True)
    p2.add_argument('--b', nargs='+', required=True)
    a = ap.parse_args()
    q1(a) if a.q == 'q1' else q2(a)


if __name__ == '__main__':
    main()
