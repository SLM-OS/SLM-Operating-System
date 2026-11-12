#!/usr/bin/env python3
# critic's own classifier; reads raw console text, not the builder's meta result
import re,sys,glob,os
def cls(path):
    t=open(path,errors='replace').read()
    L=t.split('\n')
    d={}
    m=re.search(r'SLM-OS v\S+ build \d+ \((\w+)\)',"\n".join(l for l in L if not l.startswith('#')))
    d['banner']=m.group(1) if m else 'NONE'
    m=re.search(r'elf[ _]sha256[ =](\w{8})',L[0]); d['elf']=m.group(1) if m else '?'
    m=re.search(r'rc=(\d+)',L[0]); d['rc']=m.group(1) if m else '?'
    m=re.search(r'load_start=\[([\d.]+)',L[0]) or re.search(r'load_before=\[([\d.]+)',L[0]); d['load']=m.group(1) if m else '?'
    fails=[l.strip() for l in L if re.match(r'\s*\[FAIL\] \S',l) and 'test(s) failed' not in l and 'suite' not in l.lower()]
    d['fails']=[re.sub(r'^\[FAIL\] ','',f)[:60] for f in fails]
    st=[l.strip() for l in L if 'batch: stress workload' in l and not l.startswith('#')]
    d['stress']='PASS' if any('[PASS]' in s for s in st) else ('FAIL' if any('[FAIL]' in s for s in st) else 'ABSENT')
    d['panic']='KERNEL PANIC' in t
    pm=re.search(r'KERNEL PANIC.*?\n(?:.*\n){0,6}?.*?((?:PANIC|panic|Reason|Message|task_exit)[^\n]*)',t)
    sig=re.findall(r'terminating on signal (\d+) from pid \d+ \(([^)]*)\)',t)
    d['sig']=sig
    d['allpass']='[PASS] All test suites passed!' in t
    d['semi']='Exiting via semihosting' in t
    d['litmus']=any('litmus' in f for f in d['fails'])
    ext=[s for s in sig if 'timeout' not in s[1]]
    if ext: c='INVALID(ext-kill %s)'%ext
    elif d['allpass'] and d['rc']=='0' and not d['fails'] and not d['panic']:
        c='PASS' if d['stress']=='PASS' else 'PASS?stress-'+d['stress']
    else:
        k=[]
        if d['stress']=='FAIL': k.append('A-stress')
        if d['panic']: k.append('B-panic')
        if d['litmus']: k.append('C-litmus')
        oth=[f for f in d['fails'] if 'litmus' not in f and 'stress workload' not in f]
        if oth: k.append('other-fail(%d)'%len(oth))
        if sig: k.append('timeout')
        if not k: k.append('UNCLASSIFIED')
        c='FAIL:'+'+'.join(k)
    d['cls']=c
    return d
if __name__=='__main__':
    for pat in sys.argv[1:]:
        fs=sorted(glob.glob(pat)); cnt={}
        for f in fs:
            d=cls(f); cnt[d['cls'].split('(')[0]]=cnt.get(d['cls'].split('(')[0],0)+1
            print(os.path.basename(f),d['banner'],d['elf'],'rc='+d['rc'],'load='+d['load'],'stress='+d['stress'],d['cls'],'|',';'.join(d['fails'])[:150])
        print('  TOTAL',len(fs),cnt,'\n')
