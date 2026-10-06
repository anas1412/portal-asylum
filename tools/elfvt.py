"""Read vtables / data from OLGame.x86_64 offline. Usage: elfvt.py <vtable symbol (demangled)> [slots]"""
import sys,struct,subprocess,re,os
B=os.path.expanduser('~/.local/share/Steam/steamapps/common/Outlast/Binaries/Linux/OLGame.x86_64')
data=open(B,'rb').read()
segs=[]
for l in subprocess.run(['readelf','-lW',B],capture_output=True,text=True).stdout.splitlines():
    m=re.match(r'\s+LOAD\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+0x[0-9a-f]+\s+0x([0-9a-f]+)',l)
    if m: segs.append(tuple(int(x,16) for x in m.groups()))
def rd(va,n):
    for off,v,fs in segs:
        if v<=va<v+fs: return data[off+va-v:off+va-v+n]
addr={};name={}
for l in subprocess.run(['nm','-C',B],capture_output=True,text=True).stdout.splitlines():
    if len(l)<19 or l[16]!=' ' or l[0]==' ': continue
    a=int(l[:16],16); n=l[19:]; addr.setdefault(n,a)
    if l[17] in 'TtWw': name.setdefault(a,n)
if __name__=='__main__':
    va=addr['vtable for '+sys.argv[1]]; k=int(sys.argv[2]) if len(sys.argv)>2 else 12
    for i in range(k):
        v=struct.unpack('<Q',rd(va+16+8*i,8))[0]; print(i,hex(i*8),hex(v),name.get(v,''))
