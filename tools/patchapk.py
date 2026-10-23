import struct, zlib, hashlib, sys, datetime
from cryptography import x509
from cryptography.x509.oid import NameOID
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa, padding
SRC, SO, OUT, KEYF = sys.argv[1:5]
OLDP, NEWP = (sys.argv[5], sys.argv[6]) if len(sys.argv) > 6 else ('', '')
d = open(SRC,'rb').read(); newso = open(SO,'rb').read()
# --- parse EOCD / central directory
e = d.rfind(b'PK\x05\x06'); _,_,_,_,n,cdsz,cdoff,cl = struct.unpack('<IHHHHIIH', d[e:e+22]); assert cl==0
cd = d[cdoff:cdoff+cdsz]; entries=[]; p=0
while p < len(cd):
    assert cd[p:p+4]==b'PK\x01\x02'
    nl,el,cm = struct.unpack('<HHH', cd[p+28:p+34]); ln=46+nl+el+cm
    entries.append((cd[p:p+ln], cd[p+46:p+46+nl].decode())); p+=ln
out = bytearray(); newcd = bytearray(); cnt=0
for raw, name in entries:
    if name.startswith('META-INF/') and name.rsplit('.',1)[-1] in ('SF','RSA','MF','DSA','EC'): continue
    c = bytearray(raw); off = struct.unpack('<I', c[42:46])[0]
    sig,ver,flag,comp,t,dt,crc,cs,us,nl,el = struct.unpack('<IHHHHHIIIHH', d[off:off+30])
    assert not flag & 8
    data = d[off+30+nl+el: off+30+nl+el+cs]; extra = d[off+30+nl: off+30+nl+el]
    mod = False
    if name == 'lib/arm64-v8a/libmpclient.so':
        assert comp == 0; data = newso; crc = zlib.crc32(newso); cs = us = len(newso); mod = True
    elif name in ('AndroidManifest.xml', 'resources.arsc') and OLDP:
        raw_ = data if comp == 0 else zlib.decompress(data, -15)
        o16, n16 = OLDP.encode('utf-16le'), NEWP.encode('utf-16le'); assert len(o16) == len(n16)
        assert raw_.count(o16) == 1, name
        raw_ = raw_.replace(o16, n16); crc = zlib.crc32(raw_); us = len(raw_)
        if comp == 0: data = raw_
        else: co = zlib.compressobj(9, zlib.DEFLATED, -15); data = co.compress(raw_) + co.flush()
        cs = len(data); mod = True
    # strip old zipalign padding, re-align stored entries
    extra = b''
    if comp == 0:
        al = 4096 if name.endswith('.so') else 4
        base = len(out)+30+nl
        pad = (-base) % al
        if pad:
            while pad < 4: pad += al
            extra = struct.pack('<HH', 0xD935, pad-4) + b'\0'*(pad-4)
    c[42:46] = struct.pack('<I', len(out)); c[30:32] = struct.pack('<H', 0)  # central extra dropped below
    # keep central extra field length consistent: rebuild central record without its extra
    nlc,elc,cmc = struct.unpack('<HHH', raw[28:34]); body = raw[46:46+nlc]
    c = bytearray(raw[:46]); c[42:46]=struct.pack('<I',len(out)); c[30:32]=struct.pack('<H',0)
    if mod:
        c[16:20]=struct.pack('<I',crc); c[20:24]=struct.pack('<I',cs); c[24:28]=struct.pack('<I',us)
    newcd += bytes(c) + body + raw[46+nlc+elc:]
    out += struct.pack('<IHHHHHIIIHH', sig,ver,flag,comp,t,dt,crc,cs,us,nl,len(extra)) + d[off+30:off+30+nl] + extra + data
    cnt += 1
cdstart = len(out)
eocd = struct.pack('<IHHHHIIH', 0x06054b50,0,0,cnt,cnt,len(newcd),cdstart,0)
# --- APK Signature Scheme v2
try: key = serialization.load_pem_private_key(open(KEYF,'rb').read(), None)
except FileNotFoundError:
    key = rsa.generate_private_key(65537, 2048); open(KEYF,'wb').write(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
name_ = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME,'mpmod')]); now = datetime.datetime(2024,1,1)
cert = x509.CertificateBuilder().subject_name(name_).issuer_name(name_).public_key(key.public_key()).serial_number(1).not_valid_before(now).not_valid_after(now.replace(year=2060)).sign(key, hashes.SHA256())
certder = cert.public_bytes(serialization.Encoding.DER); pub = key.public_key().public_bytes(serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo)
def chunks(b):
    return [hashlib.sha256(b'\xa5'+struct.pack('<I',len(b[i:i+(1<<20)]))+b[i:i+(1<<20)]).digest() for i in range(0,len(b),1<<20)]
digs = chunks(bytes(out)) + chunks(bytes(newcd)) + chunks(eocd)   # eocd already holds cd offset = start of signing block
top = hashlib.sha256(b'\x5a'+struct.pack('<I',len(digs))+b''.join(digs)).digest()
L = lambda b: struct.pack('<I',len(b))+b
signed = L(L(struct.pack('<I',0x0103)+L(top))) + L(L(certder)) + L(b'')
sig = key.sign(signed, padding.PKCS1v15(), hashes.SHA256())
signer = L(signed) + L(L(struct.pack('<I',0x0103)+L(sig))) + L(pub)
pair = struct.pack('<Q', 4+len(L(L(signer)))) + struct.pack('<I',0x7109871a) + L(L(signer))
blk = struct.pack('<Q', len(pair)+8+16) + pair + struct.pack('<Q', len(pair)+8+16) + b'APK Sig Block 42'
# padding so the central directory stays 4 KiB aligned is not required; just assemble
final = bytes(out) + blk + bytes(newcd) + eocd[:16] + struct.pack('<I', cdstart+len(blk)) + eocd[20:]
open(OUT,'wb').write(final); print('wrote', OUT, len(final), 'entries', cnt)
