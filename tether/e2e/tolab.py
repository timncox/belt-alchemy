import sys,re
# keyboard sim log "<ms> ms: B0 B1 B2 -> USB" -> "<ms rel> B0 B1 B2", USB lines only
src,dst=sys.argv[1],sys.argv[2]
ev=[]
for l in open(src):
    m=re.match(r"(\d+) ms: (\w\w) (\w\w) (\w\w) ->(.*)",l)
    if not m or 'USB' not in m.group(5) or 'USBH' in m.group(5): continue
    ev.append((int(m.group(1)),m.group(2),m.group(3),m.group(4)))
t0=ev[0][0]
with open(dst,'w') as f:
    for t,a,b,c in ev: f.write(f"{t-t0} {a} {b} {c}\n")
print(len(ev),"events over",ev[-1][0]-t0,"ms")
