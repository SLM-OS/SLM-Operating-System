#!/usr/bin/env python3
"""Summarize run-llama.sh results: python3 -I summarize-llama.py <dir> <label>"""
import glob,re,statistics as st,sys,os,collections
d,l=sys.argv[1],sys.argv[2]; rows=collections.defaultdict(list)
for f in sorted(glob.glob(f"{d}/{l}-ngl*-p*-r*-*.err")):
    s=open(f,errors='replace').read(); n=os.path.basename(f)
    m=re.search(r'ngl(\d+)-p(\d)-r(\d)-(\w+)\.err',n)
    pe=re.search(r'prompt eval time =\s*([\d.]+) ms /\s*(\d+) tokens',s); e=re.search(r'\beval time =\s*([\d.]+) ms /\s*(\d+) runs',s)
    if not (pe and e): print("MISSING",n); continue
    rows[(m[2],m[4])].append((float(pe[1]),int(pe[2]),int(e[2])*1000/float(e[1]),int(e[2]),m[3]))
print("prompt mode   n_prompt  prefill_ms(med)  decode_tok/s(med) [min-max]  gen_tokens")
for k,v in sorted(rows.items()):
    print(k[0],k[1].ljust(8),v[0][1],f"{st.median(x[0] for x in v):.1f}",f"{st.median(x[2] for x in v):.2f} [{min(x[2] for x in v):.2f}-{max(x[2] for x in v):.2f}]",sorted({x[3] for x in v}))
