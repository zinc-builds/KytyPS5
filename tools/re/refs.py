import sys
import numpy as np
img=np.memmap(sys.argv[1],dtype=np.uint8,mode='r')
text=img[:0x74bd57c].astype(np.int64)
d=(text[:-4]|(text[1:-3]<<8)|(text[2:-2]<<16)|(text[3:-1]<<24)); d=np.where(d>=2**31,d-2**32,d)
tgt=np.arange(len(d))+4+d
for t in sys.argv[2:]:
    for x in np.nonzero(tgt==int(t,0))[0]: print(t,hex(int(x)))
