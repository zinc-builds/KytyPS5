import sys,re,glob,os
rows=[]
for f in glob.glob('/tmp/bench-*.log'):
    L=open(f,errors='replace').read().splitlines()
    perf=[];ev={}
    for i,l in enumerate(L):
        m=re.search(r'^PERF fps=([\d.]+).*frames=(\d+) t=([\d.]+)',l)
        if m: perf.append((i,int(m[2]),float(m[3]),float(m[1])))
        elif 'StartLevel aerial_garden' in l: ev.setdefault('start',i)
        elif 'veg_chibi' in l and '6->7' in l: ev.setdefault('chibi',i)
    if 'start' not in ev or 'chibi' not in ev or not perf: continue
    def at(i):
        b=[p for p in perf if p[0]<i]; a=[p for p in perf if p[0]>i]
        if not a or not b: return None
        b=b[-1];a=a[0]; return ((b[1]+a[1])/2,(b[2]+a[2])/2,a[1]-b[1])
    s=at(ev['start']); c=at(ev['chibi'])
    if not s or not c: continue
    fps=[p[3] for p in perf if s[1]<p[2]<c[1]]
    rows.append((os.path.basename(f), c[0]-s[0], c[1]-s[1], sum(fps)/max(1,len(fps)), os.path.getmtime(f)))
for r in sorted(rows,key=lambda r:r[3]): print('%-40s dframes=%6.0f dt=%6.1fs meanfps=%5.1f'%r[:4])
