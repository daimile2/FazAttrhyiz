#!/usr/bin/env python3
"""QEMU user-net UDP echo peer; run on the host before the test VM."""
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(('127.0.0.1', 9999))
print('CapMix UDP echo listening 127.0.0.1:9999', flush=True)
while True:
    payload, peer = s.recvfrom(65535)
    s.sendto(payload, peer)
    print(f'udp echo bytes={len(payload)} peer={peer}', flush=True)
