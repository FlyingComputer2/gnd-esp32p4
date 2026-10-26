#!/usr/bin/env python3
"""Exercise RGP4 v2 video/upstream channels with PyUSB (pip install pyusb)."""
import argparse
from collections import deque
import json
import struct
import time

HEADER = struct.Struct('<4sBBHHHII')

def main():
    import usb.core
    import usb.util
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--channel', type=int, default=0)
    p.add_argument('--seconds', type=float, default=10)
    p.add_argument('--send-hex', help='One upstream application datagram, encoded as hex')
    args = p.parse_args()
    if not 0 <= args.channel <= 65535:
        p.error('channel must be u16')
    device = usb.core.find(idVendor=0x303a, idProduct=0x4001)
    if device is None:
        raise SystemExit('P4 not found; use High Speed USB / USB3')
    usb.util.claim_interface(device, 0)
    try:
        session, = struct.unpack('<I', bytes(device.ctrl_transfer(0xc0, 1, 0, 0, 4, timeout=2000)))
        pending = bytearray()
        records = deque()
        def send(kind, channel, sequence, payload):
            frame = HEADER.pack(b'RGP4', 2, kind, 0, channel, len(payload), session, sequence) + payload
            if device.write(1, frame, timeout=2000) != len(frame):
                raise RuntimeError('Incomplete write; reset session before retry')
        def read():
            if records:
                return records.popleft()
            try:
                pending.extend(device.read(0x81, 8192, timeout=200))
            except usb.core.USBTimeoutError:
                return None
            while len(pending) >= 20:
                magic, version, kind, flags, channel, length, sid, seq = HEADER.unpack_from(pending)
                if magic != b'RGP4' or sid != session:
                    del pending[0]
                    continue
                if version != 2 or flags or length > 2048:
                    raise RuntimeError('Invalid v2 record')
                if len(pending) < 20 + length:
                    break
                records.append((kind, channel, seq, bytes(pending[20:20 + length])))
                del pending[:20 + length]
            return records.popleft() if records else None
        def request(seq, **command):
            send(1, 0, seq, json.dumps(command).encode())
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                packet = read()
                if packet and packet[0] == 2 and packet[2] == seq:
                    reply = json.loads(packet[3])
                    if not reply.get('ok'):
                        raise RuntimeError(reply)
                    return reply
            raise RuntimeError('Management request timed out')
        print(request(1, op='hello', version=2))
        print(request(2, op='list', offset=0, limit=4))
        opened = request(3, op='open', channel=args.channel, rx_window=8)
        print(opened)
        if args.send_hex:
            payload = bytes.fromhex(args.send_hex)
            if not 1 <= len(payload) <= opened['max_payload']:
                raise ValueError('datagram exceeds negotiated channel MTU')
            send(4, args.channel, 1, payload)
        count = total = 0
        end = time.monotonic() + args.seconds
        while time.monotonic() < end:
            packet = read()
            if packet and packet[0] == 4:
                _, channel, _, payload = packet
                if channel != args.channel:
                    raise RuntimeError('Unexpected channel')
                count += 1
                total += len(payload)
                send(5, channel, 0, struct.pack('<H', 1))
        print(f'channel={args.channel} datagrams={count} payload_bytes={total}')
    finally:
        usb.util.release_interface(device, 0)
        usb.util.dispose_resources(device)

if __name__ == '__main__':
    main()
