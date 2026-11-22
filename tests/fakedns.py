# Tiny DNS server for testing: answers A queries for a few test names with 10.0.2.2
import socket, struct, sys
names = {b'example.com': '10.0.2.2', b'myos.test': '10.0.2.2', b'www.myos.test': '10.0.2.2'}
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.bind(('127.0.0.1', int(sys.argv[1]) if len(sys.argv) > 1 else 53))
while True:
    q, addr = s.recvfrom(512)
    i = 12; labels = []
    while q[i]: labels.append(q[i+1:i+1+q[i]]); i += 1 + q[i]
    name = b'.'.join(labels).lower(); qend = i + 5
    ip = names.get(name)
    flags = 0x8180 if ip else 0x8183
    resp = q[:2] + struct.pack('>HHHHH', flags, 1, 1 if ip else 0, 0, 0) + q[12:qend]
    if ip:
        resp += b'\xc0\x0c' + struct.pack('>HHIH', 5, 1, 60, 6) + b'\x03www\xc0\x0c'   # a CNAME first, to test skipping
        resp = resp[:6] + struct.pack('>H', 2) + resp[8:]
        resp += b'\xc0' + bytes([qend]) + struct.pack('>HHIH', 1, 1, 60, 4) + socket.inet_aton(ip)
    s.sendto(resp, addr); print('answered', name, ip, flush=True)
