#!/usr/bin/env python3
"""Parse Wine user.reg Credential Manager entries; print masked names + hashes only.
Never prints secret values. Usage: credhash.py user.reg [--plain]"""
import sys, re, hashlib, datetime, json

LONG = re.compile(r'[A-Za-z0-9_\-]{20,}')
def mask(s):
    return LONG.sub(lambda m: '<%d:%s>' % (len(m.group()), hashlib.sha256(m.group().encode()).hexdigest()[:8]), s)

def parse(path):
    txt = open(path, 'r', encoding='utf-8', errors='surrogateescape').read()
    # join continuation lines
    txt = re.sub(r'\\\n\s*', '', txt)
    keys = {}
    cur = None
    for line in txt.split('\n'):
        m = re.match(r'^\[(.*)\] (\d+)$', line)
        if m:
            cur = m.group(1); keys[cur] = {'_ts': int(m.group(2)), '_vals': {}}
            continue
        if cur is None: continue
        if line.startswith('#time='):
            keys[cur]['_time'] = int(line[6:], 16); continue
        m = re.match(r'^(@|"(?:[^"\\]|\\.)*")=(.*)$', line)
        if m:
            name = m.group(1).strip('"')
            keys[cur]['_vals'][name] = m.group(2)
    return keys

def regval_bytes(v):
    if v.startswith('hex:'):
        h = v[4:].replace(',', '').strip()
        return bytes.fromhex(h)
    if v.startswith('dword:'):
        return int(v[6:], 16).to_bytes(4, 'little')
    if v.startswith('"'):
        return v.encode('utf-8', 'surrogateescape')
    m = re.match(r'hex\((\w+)\):(.*)', v)
    if m:
        return bytes.fromhex(m.group(2).replace(',', ''))
    return v.encode()

def rc4(key, data):
    S = list(range(256)); j = 0
    for i in range(256):
        j = (j + S[i] + key[i % len(key)]) & 255; S[i], S[j] = S[j], S[i]
    i = j = 0; out = bytearray()
    for b in data:
        i = (i + 1) & 255; j = (j + S[i]) & 255; S[i], S[j] = S[j], S[i]
        out.append(b ^ S[(S[i] + S[j]) & 255])
    return bytes(out)

def ft2str(ft):
    return (datetime.datetime(1601,1,1) + datetime.timedelta(microseconds=ft//10)).replace(tzinfo=datetime.timezone.utc).astimezone().strftime('%m-%d %H:%M:%S')

def main():
    path = sys.argv[1]
    keys = parse(path)
    base = r'Software\\Wine\\Credential Manager'
    mgr = keys.get(base)
    enc = None
    if mgr:
        print('mgr key ts=%s' % datetime.datetime.fromtimestamp(mgr['_ts']).strftime('%m-%d %H:%M:%S'),
              'EncryptionKey sha256=%s' % (hashlib.sha256(regval_bytes(mgr['_vals']['EncryptionKey'])).hexdigest()[:16] if 'EncryptionKey' in mgr['_vals'] else 'none'))
        if 'EncryptionKey' in mgr['_vals']: enc = regval_bytes(mgr['_vals']['EncryptionKey'])
    for k in sorted(keys):
        if not k.startswith(base + r'\\'): continue
        d = keys[k]; v = d['_vals']
        name = k[len(base)+2:]
        pw = regval_bytes(v['Password']) if 'Password' in v else b''
        lw = int.from_bytes(regval_bytes(v['LastWritten']), 'little') if 'LastWritten' in v else 0
        persist = v.get('Persist'); flags = v.get('Flags'); typ = v.get('Type')
        cm = regval_bytes(v['Comment']) if 'Comment' in v else b''
        line = '%s | keyts=%s LastWritten=%s | Persist=%s Flags=%s Type=%s | pwlen=%d pw_sha=%s | comment_len=%d comment_sha=%s' % (
            mask(name), datetime.datetime.fromtimestamp(d['_ts']).strftime('%H:%M:%S'), ft2str(lw) if lw else '-',
            persist and int(persist[6:],16), flags and int(flags[6:],16), typ and int(typ[6:],16),
            len(pw), hashlib.sha256(pw).hexdigest()[:16], len(cm), hashlib.sha256(cm).hexdigest()[:8])
        extra = [x for x in v if x not in ('@','Password','LastWritten','Persist','Flags','Type','Comment')]
        if extra: line += ' | extra=' + ','.join(extra)
        print(line)

if __name__ == '__main__':
    main()
