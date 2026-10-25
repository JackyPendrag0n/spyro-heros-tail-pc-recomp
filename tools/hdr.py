import struct,sys
def arrays(d):
    out=[]
    for o in range(0x5c,0xd4,8):
        a,rel=struct.unpack_from('<Ii',d,o)
        out.append((o,a&0xffff,(a>>16)-0x10000 if a>>31 else a>>16,o+4+rel))
    return out
def dump(d,o,n):
    for k in range(0,n,0x10):
        print('   %06x'%(o+k),' '.join('%08x'%x for x in struct.unpack_from('<4I',d,o+k)))
if __name__=='__main__':
    d=open(sys.argv[1],'rb').read()
    A=arrays(d)
    for i,(o,c,h,p) in enumerate(A):
        nxt=sorted(set(q for _,_,_,q in A if q>p)+[0x800]) if False else None
        print('%02x cnt=%d hi=%d -> %x'%(o,c,h,p))
        if c and len(sys.argv)>2: dump(d,p,min(0x40,16*c))
