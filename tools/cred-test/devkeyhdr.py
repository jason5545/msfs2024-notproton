#!/usr/bin/env python3
"""DeviceKey: print only Id hash, Key header (magic name, cbKey) and validity; never key bytes."""
import sys, json, hashlib, base64, struct, os
sys.path.insert(0, os.path.dirname(__file__))
from credhash import parse, regval_bytes, rc4
MAG = {0x31534345:'ECDSA_PRIVATE_P256(ECS1?)',0x32534345:'BCRYPT_ECDSA_PRIVATE_P256_MAGIC(ECS2)',0x31534345:'ECS1',0x32424345:'ECDH?'}
keys = parse(sys.argv[1]); base = r'Software\\Wine\\Credential Manager'
enc = regval_bytes(keys[base]['_vals']['EncryptionKey'])
pw = rc4(enc, regval_bytes(keys[base + r'\\Generic: Xbl|DeviceKey']['_vals']['Password']))
j = json.loads(pw)
kb = base64.b64decode(j['Key'])
magic, cb = struct.unpack('<II', kb[:8])
print('Id_sha=%s Key_sha=%s keybytes=%d magic=%s(0x%08x) cbKey=%d expected_len=%d' % (
    hashlib.sha256(j['Id'].encode()).hexdigest()[:12], hashlib.sha256(kb).hexdigest()[:12], len(kb),
    MAG.get(magic,'?'), magic, cb, 8+3*cb))
# check d < n and point on curve (no printing)
p=0xffffffff00000001000000000000000000000000ffffffffffffffffffffffff
a=p-3; b=0x5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b
n=0xffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551
x=int.from_bytes(kb[8:40],'big'); y=int.from_bytes(kb[40:72],'big'); d=int.from_bytes(kb[72:104],'big')
print('on_curve=%s 0<d<n=%s' % ((y*y - (x*x*x + a*x + b)) % p == 0, 0 < d < n))
# Q == d*G ?
Gx=0x6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296; Gy=0x4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5
def add(P,Q):
    if P is None: return Q
    if Q is None: return P
    if P[0]==Q[0] and (P[1]+Q[1])%p==0: return None
    l = ((3*P[0]*P[0]+a)*pow(2*P[1],-1,p) if P==Q else (Q[1]-P[1])*pow(Q[0]-P[0],-1,p)) % p
    xr=(l*l-P[0]-Q[0])%p; return (xr,(l*(P[0]-xr)-P[1])%p)
R=None; Pt=(Gx,Gy); k=d
while k:
    if k&1: R=add(R,Pt)
    Pt=add(Pt,Pt); k>>=1
print('pub_matches_priv=%s' % (R==(x,y)))
