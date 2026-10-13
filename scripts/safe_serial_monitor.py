import sys
import time
import argparse
import serial
import io
import os
from PIL import Image

def get_safe_serial(port='COM11', baud=115200, timeout=2.0):
    """
    ALWAYS configure dtr=False and rts=False BEFORE opening.
    Never use bare serial.Serial(port) which asserts DTR/RTS and resets ESP32.
    """
    s = serial.Serial()
    s.port = port
    s.baudrate = baud
    s.timeout = timeout
    s.dtr = False
    s.rts = False
    s.open()
    try:
        s.dtr = False
        s.rts = False
    except Exception:
        pass
    return s

def send_command(cmd, port='COM11', baud=115200, wait=0.5):
    s = get_safe_serial(port=port, baud=baud, timeout=0.5)
    full_cmd = (cmd.strip() + "\r\n").encode('utf-8')
    s.write(full_cmd)
    time.sleep(wait)
    output = []
    while s.in_waiting:
        line = s.readline().decode('utf-8', errors='ignore').strip()
        if line:
            output.append(line)
    s.close()
    return output

def take_screenshot(out_file, port='COM11', baud=115200, rotate_deg=0):
    s = get_safe_serial(port=port, baud=baud, timeout=2.0)
    s.write(b"screenshot\r\n")
    
    buf = bytearray()
    start_time = time.time()
    file_size = 0
    found_start = False

    while time.time() - start_time < 15.0:
        chunk = s.read(s.in_waiting or 1024)
        if chunk:
            buf.extend(chunk)
            if not found_start:
                idx = buf.find(b"===BEGIN_BMP:")
                if idx != -1:
                    end_idx = buf.find(b"===", idx + 13)
                    if end_idx != -1:
                        file_size = int(buf[idx+13:end_idx].decode('utf-8', errors='ignore').strip())
                        post_hdr = end_idx + 3
                        if post_hdr < len(buf) and buf[post_hdr] == ord('\r'): post_hdr += 1
                        if post_hdr < len(buf) and buf[post_hdr] == ord('\n'): post_hdr += 1
                        buf = buf[post_hdr:]
                        found_start = True
            else:
                if len(buf) >= file_size or b"===END_BMP===" in buf:
                    break
        else:
            time.sleep(0.02)
    s.close()

    if not found_start:
        print("[ERROR] Screenshot start marker not received.")
        return False

    if len(buf) < file_size:
        buf.extend(b'\x00' * (file_size - len(buf)))

    bmp_bytes = bytes(buf[:file_size])
    img = Image.open(io.BytesIO(bmp_bytes))
    if rotate_deg != 0:
        img = img.rotate(rotate_deg)

    os.makedirs(os.path.dirname(os.path.abspath(out_file)), exist_ok=True)
    img.save(out_file)
    print(f"[SUCCESS] Saved screenshot ({img.size[0]}x{img.size[1]}) to {out_file}")
    return True

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description="Unified Safe Non-Resetting CLI & Screenshot Utility for TuneBar")
    parser.add_argument('--port', '-p', default='COM11', help='COM port')
    parser.add_argument('--baud', '-b', type=int, default=115200, help='Baud rate')
    subparsers = parser.add_subparsers(dest='action', required=True)

    # Subcommand: cmd
    cmd_p = subparsers.add_parser('cmd', help='Send CLI command without reset')
    cmd_p.add_argument('command', help='Command to send (e.g. "menu", "status", "tap 100 100")')
    cmd_p.add_argument('--wait', '-w', type=float, default=0.5, help='Wait time in seconds')

    # Subcommand: snap
    snap_p = subparsers.add_parser('snap', help='Capture screenshot without reset')
    snap_p.add_argument('output', help='Output file path (.png or .bmp)')
    snap_p.add_argument('--rotate', '-r', type=int, default=0, help='Rotation angle (0, 90, 180, 270)')

    # Subcommand: listen
    listen_p = subparsers.add_parser('listen', help='Listen to logs without reset')
    listen_p.add_argument('--duration', '-d', type=float, default=10.0, help='Duration in seconds')
    listen_p.add_argument('--filter', '-f', default=None, help='Filter string')

    args = parser.parse_args()

    if args.action == 'cmd':
        out = send_command(args.command, port=args.port, baud=args.baud, wait=args.wait)
        for l in out:
            print(l)
    elif args.action == 'snap':
        ok = take_screenshot(args.output, port=args.port, baud=args.baud, rotate_deg=args.rotate)
        sys.exit(0 if ok else 1)
    elif args.action == 'listen':
        s = get_safe_serial(port=args.port, baud=args.baud, timeout=0.1)
        print(f"[SAFE-SERIAL] Listening on {args.port} for {args.duration}s...", flush=True)
        t0 = time.time()
        while time.time() - t0 < args.duration:
            line = s.readline().decode('utf-8', errors='ignore').strip()
            if line:
                if args.filter is None or args.filter.lower() in line.lower():
                    print(line, flush=True)
        s.close()
