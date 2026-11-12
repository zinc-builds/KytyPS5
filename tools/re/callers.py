import sys,re
import numpy as np
img=np.memmap(sys.argv[1],dtype=np.uint8,mode='r')
text=img[:0x74bd57c]
targets=[int(x,0) for x in sys.argv[2:]]
for op in (0xe8,0xe9):
  pos=np.nonzero(text[:-5]==op)[0]
  rel=(text[pos+1].astype(np.int64)|(text[pos+2].astype(np.int64)<<8)|(text[pos+3].astype(np.int64)<<16)|(text[pos+4].astype(np.int64)<<24))
  rel=np.where(rel>=2**31,rel-2**32,rel)
  dst=pos+5+rel
  for t in targets:
    for p in pos[dst==t]: print(hex(op),hex(t),hex(p))
