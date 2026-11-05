import re,sys
rx=re.compile(r'cpufreq_gov: (\d+) s: lit (\d+) MHz(?: asked)? load (\d+)% \(avg (\d+)%, (\d+) online, (\d+) max, measured (\S+)\) \| big (\d+) MHz(?: asked)? load (\d+)% \(avg (\d+)%, (\d+) online, (\d+) max, measured (\S+)\) \| temp big (\S+) lit (\S+) C \| (\d+) changes \| boost: (\d+) touch, (\d+) frames, (\d+) raised')
seen=set();out=[]
for l in open(sys.argv[1],errors='replace'):
    m=rx.search(l)
    if not m: continue
    k=m.group(0)
    if k in seen: continue
    seen.add(k);g=m.groups()
    if int(g[0])<120: continue
    out.append((int(g[0]),int(g[9]),int(g[3]),int(g[7]),int(g[1]),int(g[17]),int(g[16])))
for r in out: print(*r)
print(len(out),file=sys.stderr)
