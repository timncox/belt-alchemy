import sys,re
ev=[]
for l in open(sys.argv[1]):
    m=re.match(r"\s*(\d+) ms  midi out (\w\w) (\d+) (\d+)",l)
    if m: ev.append((int(m.group(1)),int(m.group(2),16),int(m.group(3)),int(m.group(4))))
t0=ev[0][0]
with open(sys.argv[2],'w') as f:
    for t,a,b,c in ev: f.write(f"{t-t0} {a:02X} {b:02X} {c:02X}\n")
print(len(ev),"events")
