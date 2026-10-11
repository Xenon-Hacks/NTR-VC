#!/usr/bin/env python3
"""Sends a moving test picture to the NTR-VC viewer's DS mode receiver.

Use it to check that DS mode works on your PC without a console:

    python ds_test_sender.py                 # viewer on this PC
    python ds_test_sender.py 192.168.1.20    # viewer on another PC

The top screen shows colour bars with a white bar sweeping across, the bottom
screen a scrolling checkerboard. Stop with Ctrl+C.
"""
import argparse
import socket
import struct
import time

W, H = 256, 192
LINES_PER_PACKET = 2


def bgr555(r, g, b):
    return (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10)


BARS = [bgr555(*c) for c in ((255, 255, 255), (255, 255, 0), (0, 255, 255), (0, 255, 0),
                             (255, 0, 255), (255, 0, 0), (0, 0, 255), (0, 0, 0))]
WHITE = bgr555(255, 255, 255)
GREY = bgr555(96, 96, 96)


def top_line(frame, y):
    sweep = frame * 2 % W
    row = [BARS[x * len(BARS) // W] for x in range(W)]
    for x in range(sweep, min(sweep + 8, W)):
        row[x] = WHITE
    return struct.pack('<%dH' % W, *row)


def bottom_line(frame, y):
    off = frame % 32
    return struct.pack('<%dH' % W, *[WHITE if ((x + off) // 16 + (y + off) // 16) % 2 else GREY
                                     for x in range(W)])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('host', nargs='?', default='127.0.0.1', help='PC running the viewer (default: this PC)')
    ap.add_argument('--port', type=int, default=8300, help='UDP port (default 8300)')
    ap.add_argument('--fps', type=float, default=60.0)
    ap.add_argument('--frames', type=int, default=0, help='stop after this many frames (default: run until Ctrl+C)')
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    dest = (args.host, args.port)
    print('Sending DS test frames to %s:%d, Ctrl+C to stop' % dest)
    frame = 0
    next_t = time.perf_counter()
    try:
        while not args.frames or frame < args.frames:
            fid = frame & 0xFFFF
            for screen, make in ((0, top_line), (1, bottom_line)):
                for y0 in range(0, H, LINES_PER_PACKET):
                    hdr = b'NDS1' + struct.pack('<HBBBBH', fid, screen, 0, y0, LINES_PER_PACKET, 0)
                    sock.sendto(hdr + b''.join(make(frame, y) for y in range(y0, y0 + LINES_PER_PACKET)), dest)
            frame += 1
            next_t += 1.0 / args.fps
            delay = next_t - time.perf_counter()
            if delay > 0:
                time.sleep(delay)
            else:
                next_t = time.perf_counter()
    except KeyboardInterrupt:
        pass
    print('Sent %d frames' % frame)


if __name__ == '__main__':
    main()
