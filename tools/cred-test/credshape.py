#!/usr/bin/env python3
"""Show only the SHAPE of a decrypted Wine credential blob (encoding, JSON keys, value lengths).
Never prints values. Usage: credshape.py user.reg <substring-of-target>"""
import sys, json, hashlib, re, base64
sys.path.insert(0, __import__('os').path.dirname(__file__))
from credhash import parse, regval_bytes, rc4, mask

def shape(obj, depth=0):
    pad = '  ' * depth
    if isinstance(obj, dict):
        for k, v in obj.items():
            if isinstance(v, (dict, list)):
                print('%s%s: %s' % (pad, k, type(v).__name__)); shape(v, depth+1)
            elif isinstance(v, str):
                cls = 'b64url' if re.fullmatch(r'[A-Za-z0-9_\-]+', v) else ('b64' if re.fullmatch(r'[A-Za-z0-9+/=]+', v) else 'text')
                extra = ''
                if k.lower() in ('kty','crv','alg','use','type','version'):
                    extra = ' value=%r' % v if len(v) < 12 else ''
                print('%s%s: str len=%d class=%s sha=%s%s' % (pad, k, len(v), cls, hashlib.sha256(v.encode()).hexdigest()[:12], extra))
            else:
                print('%s%s: %s %s' % (pad, k, type(v).__name__, 'num' if isinstance(v,(int,float)) else ''))
    elif isinstance(obj, list):
        print('%slist len=%d' % (pad, len(obj)))
        for i, v in enumerate(obj[:3]): shape(v, depth+1)

keys = parse(sys.argv[1])
base = r'Software\\Wine\\Credential Manager'
enc = regval_bytes(keys[base]['_vals']['EncryptionKey'])
for k, d in keys.items():
    if not k.startswith(base + r'\\') or sys.argv[2] not in k: continue
    pw = rc4(enc, regval_bytes(d['_vals']['Password']))
    print('==', mask(k[len(base)+2:]), 'len', len(pw), 'plain_sha', hashlib.sha256(pw).hexdigest()[:16])
    txt = None
    for e in ('utf-8', 'utf-16-le'):
        try:
            t = pw.decode(e)
            if all(c.isprintable() or c in '\r\n\t' for c in t.rstrip('\0')):
                txt = t.rstrip('\0'); print('encoding', e, 'nul_term', pw.endswith(b'\0')); break
        except Exception: pass
    if txt is None:
        print('binary; magic4_is_ECS2?', pw[:4] in (b'ECS2', b'ECK2', b'ECS1', b'ECK1'), 'printable_ratio', sum(32<=b<127 for b in pw)/len(pw)); continue
    try:
        shape(json.loads(txt))
    except Exception as ex:
        parts = txt.split('.')
        print('not json; len', len(txt), 'dot-parts', [len(p) for p in parts][:5], 'charset', ''.join(sorted(set(re.sub(r'[A-Za-z0-9]', '', txt))))[:20])
