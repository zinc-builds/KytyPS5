import sys,struct
import numpy as np
img=np.memmap(sys.argv[1],dtype=np.uint8,mode='r')
pats={'f1/60':struct.pack('<f',1/60),'d1/60':struct.pack('<d',1/60),'f60':struct.pack('<f',60.0),'d60':struct.pack('<d',60.0),'f1/30':struct.pack('<f',1/30),'d1/30':struct.pack('<d',1/30),'f1/120':struct.pack('<f',1/120)}
ro=bytes(img[0x74c0000:0x88e0000]); 
text=img[:0x74bd57c].astype(np.int64)
d=(text[:-4]|(text[1:-3]<<8)|(text[2:-2]<<16)|(text[3:-1]<<24)); d=np.where(d>=2**31,d-2**32,d)
tgt=np.arange(len(d))+4+d
for k,p in pats.items():
    locs=[]; i=ro.find(p)
    while i>=0:
        a=0x74c0000+i
        if a%len(p)==0: locs.append(a)
        i=ro.find(p,i+1)
    refs=[]
    for a in locs: refs+= [ (a,int(x)) for x in np.nonzero(tgt==a)[0]]
    print(k,len(locs),'locs',len(refs),'refs')
    for a,x in refs[:200]: print('  ',k,hex(a),hex(x))
