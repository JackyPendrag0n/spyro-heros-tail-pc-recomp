# eng__streamdata.sfx stream N -> 22050 Hz mono WAV (Eurocom 32-byte IMA blocks, as loader/redvoice.c)
import sys, struct, wave
STEP=[7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767]
IDX=[-1,-1,-1,-1,2,4,6,8]
def decode(d):
    out=[]
    for b in range(0,len(d)-31,32):
        pred=struct.unpack_from('<h',d,b)[0]; idx=min(d[b+2],88)
        for i in range(4,32):
            for h in (0,1):
                nib=(d[b+i]&15) if h else d[b+i]>>4; st=STEP[idx]; diff=st>>3
                if nib&1: diff+=st>>2
                if nib&2: diff+=st>>1
                if nib&4: diff+=st
                pred+= -diff if nib&8 else diff
                pred=max(-32768,min(32767,pred)); idx=max(0,min(88,idx+IDX[nib&7])); out.append(pred)
    return out
f=open(sys.argv[1],'rb'); base=int(sys.argv[2],16); n=int(sys.argv[3])
f.seek(base); hdr=f.read(0x2000)
off=struct.unpack_from('<I',hdr,0x800+4*n)[0]
f.seek(base+0x2000+off); h=f.read(0x1000); size=struct.unpack_from('<I',h,8)[0]
pcm=decode(f.read(size))
w=wave.open(sys.argv[4],'wb'); w.setnchannels(1); w.setsampwidth(2); w.setframerate(22050)
w.writeframes(struct.pack('<%dh'%len(pcm),*pcm)); w.close()
print(sys.argv[4], '%.2f s'%(len(pcm)/22050))
