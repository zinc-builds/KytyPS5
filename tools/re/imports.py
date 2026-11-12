import struct,sys
img=open(sys.argv[1],'rb').read()
dyn=0x10211a00; tags={}
for i in range(0x1110//16):
    t,v=struct.unpack_from('<qQ',img,dyn+i*16)
    if t==0: break
    tags.setdefault(t,[]).append(v)
for t,v in tags.items(): print(hex(t),[hex(x) for x in v][:4],file=sys.stderr)
jmprel=tags[0x17][0]; pltsz=tags[2][0]; symtab=tags[6][0]; strtab=tags[5][0]
out=[]
for i in range(pltsz//24):
    off,info,add=struct.unpack_from('<QQq',img,jmprel+i*24)
    si=info>>32
    nm,=struct.unpack_from('<I',img,symtab+si*24)
    e=img.index(b'\0',strtab+nm); name=img[strtab+nm:e].decode()
    out.append((i,hex(off),name))
for o in out: print(*o)
