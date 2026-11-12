import struct,sys
d=open(sys.argv[1],'rb').read()
n=struct.unpack_from('<H',d,0x18)[0]
ents=[struct.unpack_from('<QQQQ',d,0x20+i*32) for i in range(n)]
eo=0x20+n*32
assert d[eo:eo+4]==b'\x7fELF'
phoff,=struct.unpack_from('<Q',d,eo+0x20); phnum,=struct.unpack_from('<H',d,eo+0x38)
ph=[struct.unpack_from('<IIQQQQQQ',d,eo+phoff+i*56) for i in range(phnum)]
segdata={}
for p,o,fs,ms in ents:
    if p&0x800: segdata[(p>>20)&0xfff]=(o,fs)
img=bytearray(); maxv=0
for i,(t,f,off,va,pa,fsz,msz,al) in enumerate(ph):
    print(i,hex(t),hex(f),'off',hex(off),'va',hex(va),'fsz',hex(fsz),'msz',hex(msz), 'self' , segdata.get(i))
    if t==1 or t==0x61000010:
        if i not in segdata: continue
        o,fs=segdata[i]
        end=va+msz
        if len(img)<end: img.extend(b'\0'*(end-len(img)))
        img[va:va+min(fs,fsz)]=d[o:o+min(fs,fsz)]
open(sys.argv[2],'wb').write(img)
