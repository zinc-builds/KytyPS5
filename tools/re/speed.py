import sys,re
for f in sys.argv[1:]:
    L=open(f,errors='replace').read().splitlines()
    perf=[];fs=[];ev={}
    for i,l in enumerate(L):
        m=re.search(r'^PERF fps=([\d.]+).*?draws_frame=(\d+).*frames=(\d+) t=([\d.]+)',l)
        if m: perf.append((i,int(m[3]),float(m[4]),float(m[1]),int(m[2]))); continue
        m=re.search(r'^FRAMESKIP flips_s=(\d+) rendered_s=(\d+) skipped_s=(\d+) skipped_draws_s=(\d+) cmd_ms_rendered=([\d.]+) cmd_ms_skipped=([\d.]+) debt_ms=([\d.]+)',l)
        if m: fs.append((i,)+tuple(float(x) for x in m.groups())); continue
        if 'StartLevel aerial_garden' in l: ev.setdefault('start',i)
        if 'veg_chibi' in l and '6->7' in l: ev.setdefault('chibi',i)
    def at(i):
        b=[p for p in perf if p[0]<i]; a=[p for p in perf if p[0]>i]
        if not a or not b: return None
        b=b[-1];a=a[0]; return ((b[1]+a[1])/2,(b[2]+a[2])/2)
    print('==',f)
    if 'start' in ev and 'chibi' in ev:
        s=at(ev['start']);c=at(ev['chibi'])
        if s and c:
            df=c[0]-s[0]; dt=c[1]-s[1]
            print('  chibi event: %.0f frames, %.1f s after StartLevel -> %.1f flips/s; game speed vs 60Hz hw: %.2fx (hw: 850/60=14.2 s)'%(df,dt,df/dt,(df/60)/dt))
    if 'start' in ev:
        s0=ev['start']; P=[p for p in perf if p[0]>s0]
        w=[p for p in P if 1000<=p[1]-P[0][1]<=1600]
        if w: print('  frames 1000-1600: flips/s %.1f draws/frame %.0f'%(sum(p[3] for p in w)/len(w), sum(p[4] for p in w)/len(w)))
        F=[x for x in fs if x[0]>s0]
        if len(F)>12:
            F=F[10:]
            n=len(F); avg=lambda k:sum(x[k] for x in F)/n
            print('  FRAMESKIP (after 10 s in level, %d s): flips/s %.1f rendered/s %.1f skipped/s %.1f skipped_draws/s %.0f cmd_ms rendered %.2f skipped %.2f debt %.1f'%(n,avg(1),avg(2),avg(3),avg(4),avg(5),avg(6),avg(7)))
