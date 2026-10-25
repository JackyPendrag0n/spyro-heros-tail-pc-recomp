import struct,sys
def arr(d,o):
    a,rel=struct.unpack_from('<Ii',d,o); return a&0xffff, o+4+rel
def scripts(d):
    c,p=arr(d,0x7c)
    for i in range(c):
        h,idx,off=struct.unpack_from('<III',d,p+i*16)
        yield h,off
def cmds(d,off):
    hdr=struct.unpack_from('<IIHHf',d,off)
    o=off+0x44
    out=[]
    while o<len(d):
        t,sz,prev,ff=d[o],d[o+1],d[o+2],d[o+3]
        st,ln,w8=struct.unpack_from('<hHH',d,o+4)
        slot,par=d[o+0xa],d[o+0xb]
        out.append((o,t,sz,st,ln,w8,slot,par,d[o+0xc:o+sz]))
        if t==0x12 or sz==0: break
        o+=sz
    return hdr,out
if __name__=='__main__':
    d=open(sys.argv[1],'rb').read()
    only=sys.argv[2] if len(sys.argv)>2 else None
    for h,off in scripts(d):
        if only and '%08x'%h!=only: continue
        hdr,cs=cmds(d,off)
        print('SCRIPT %08x @%x flags=%x frames=%d w=%d,%d fps=%g ncmd=%d'%(h,off,hdr[0],hdr[1],hdr[2],hdr[3],hdr[4],len(cs)))
        for (o,t,sz,st,ln,w8,slot,par,pl) in cs:
            ws=' '.join('%08x'%struct.unpack_from('<I',pl,i)[0] for i in range(0,len(pl)-3,4))
            print('  %06x t=%02x sz=%02x start=%5d len=%5d w8=%04x slot=%3d par=%3d | %s'%(o,t,sz,st,ln,w8,slot,par,ws))
