#!/usr/bin/env python3
"""Check the real user ELF HTTP responder through a VM's forwarded TCP port."""
import argparse
import socket


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=18080)
    parser.add_argument('--requests', type=int, default=2)
    args = parser.parse_args()
    if not 1 <= args.requests <= 100 or not 1 <= args.port <= 65535:
        parser.error('requests must be 1..100; port must be 1..65535')
    for index in range(args.requests):
        with socket.create_connection((args.host, args.port), timeout=5) as stream:
            stream.settimeout(5)
            # Fragment the request deliberately: a TCP read is not a message.
            stream.sendall(b'GET / HTTP/1.0\r\nHost: capmix-test\r\n')
            stream.sendall(b'\r\n')
            response = bytearray()
            while True:
                part = stream.recv(1024)
                if not part:
                    break
                response.extend(part)
                if len(response) > 4096:
                    raise AssertionError('response exceeded test bound')
        headers, body = bytes(response).split(b'\r\n\r\n', 1)
        assert headers.startswith(b'HTTP/1.0 200 OK\r\n'), repr(headers)
        assert b'Content-Length: 30' in headers.split(b'\r\n'), repr(headers)
        assert body == b'hello from FazAttrhyiz httpd!\n', repr(body)
        print(f'httpd client request={index + 1} status=200 payload=25 verified EOF=ok', flush=True)
    print('httpd client: PASS', flush=True)


if __name__ == '__main__':
    main()
