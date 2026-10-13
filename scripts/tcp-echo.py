#!/usr/bin/env python3
"""QEMU user-net TCP peer. No special host privileges required."""
import socket
import threading

def echo(conn):
    with conn:
        while True:
            data = conn.recv(8192)
            if not data:
                break  # orderly shutdown sends host FIN after guest EOF
            conn.sendall(data)
            print(f'tcp echo bytes={len(data)}', flush=True)

server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind(('127.0.0.1', 9998))
server.listen(8)
print('CapMix TCP echo listening 127.0.0.1:9998', flush=True)
while True:
    conn, peer = server.accept()
    threading.Thread(target=echo, args=(conn,), daemon=True).start()
