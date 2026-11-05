"""Safe serial helper for DeskBar-AI device communication.

The ESP32-S3's native USB CDC (/dev/ttyACM0) can trigger a hardware reset
(rst:0x15 USB_UART_CHIP_RESET) when the host opens the port with DTR asserted.
Opening with DTR=False avoids the unintended reset pulse. The device may still
reset on first connect (USB CDC enumeration), so callers should drain any boot
output and wait briefly before sending commands.

Usage:
    from tools.deskbar_serial import DeskBarSerial
    with DeskBarSerial('/dev/ttyACM0') as dbs:
        print(dbs.send('status'))
        print(dbs.send('wifi panel'))
"""

import serial
import time
import contextlib

DEFAULT_PORT = '/dev/ttyACM0'
DEFAULT_BAUD = 115200
BOOT_WAIT_SEC = 8.0  # time to wait for the device to boot after a reset


class DeskBarSerial:
    """Context-managed serial connection to the DeskBar-AI device.

    Opens the port with DTR=False to avoid triggering an unintended hardware
    reset on connect. If the device resets on connect, boot output is drained
    during the boot wait before the first command is sent.
    """

    def __init__(self, port=DEFAULT_PORT, baud=DEFAULT_BAUD,
                 boot_wait=BOOT_WAIT_SEC, timeout=0.05):
        self.port = port
        self.baud = baud
        self.boot_wait = boot_wait
        self.timeout = timeout
        self._ser = None

    def __enter__(self):
        self.open()
        return self

    def __exit__(self, *exc):
        self.close()

    def open(self):
        """Open the serial port safely (DTR=False to avoid reset pulse)."""
        self._ser = serial.Serial(self.port, self.baud, timeout=self.timeout)
        # Deassert DTR before the port is fully active; pyserial defaults to
        # DTR=True which can assert the ESP32-S3's reset line via USB CDC.
        self._ser.dtr = False
        time.sleep(0.1)
        # Drain any boot output produced by a connect-time reset.
        deadline = time.time() + self.boot_wait
        while time.time() < deadline:
            if self._ser.in_waiting:
                self._ser.read(self._ser.in_waiting)
            else:
                time.sleep(0.1)
        # Final drain
        if self._ser.in_waiting:
            self._ser.read(self._ser.in_waiting)

    def close(self):
        if self._ser:
            self._ser.close()
            self._ser = None

    def send(self, cmd, wait=1.5):
        """Send a CLI command and return the response text."""
        if not self._ser:
            raise RuntimeError("Serial port not open")
        self._ser.reset_input_buffer()
        self._ser.write((cmd + "\n").encode())
        time.sleep(wait)
        out = b""
        deadline = time.time() + wait
        while time.time() < deadline:
            if self._ser.in_waiting:
                out += self._ser.read(self._ser.in_waiting)
                deadline = time.time() + 0.3  # extend while data keeps arriving
            else:
                time.sleep(0.05)
        return out.decode("utf-8", "replace")

    def send_and_wait(self, cmd, marker, wait=1.5, max_wait=30.0):
        """Send a command and wait until `marker` appears in the response."""
        if not self._ser:
            raise RuntimeError("Serial port not open")
        self._ser.reset_input_buffer()
        self._ser.write((cmd + "\n").encode())
        out = b""
        deadline = time.time() + max_wait
        while time.time() < deadline:
            if self._ser.in_waiting:
                chunk = self._ser.read(self._ser.in_waiting)
                out += chunk
                if marker.encode() in out:
                    return out.decode("utf-8", "replace")
            time.sleep(0.05)
        return out.decode("utf-8", "replace")

    def screenshot(self, path='/tmp/deskbar_screenshot.bmp', wait=25.0):
        """Capture a screenshot from the device and save it as a BMP file."""
        import re
        if not self._ser:
            raise RuntimeError("Serial port not open")
        self._ser.reset_input_buffer()
        self._ser.write(b'screenshot\n')
        buf = b""
        deadline = time.time() + wait
        while time.time() < deadline:
            chunk = self._ser.read(32768)
            if chunk:
                buf += chunk
                if b'===END_BMP===' in buf:
                    break
        m = re.search(rb'===BEGIN_BMP:(\d+)===', buf)
        if not m:
            raise RuntimeError(f"No BMP header found in response: {buf[:200]!r}")
        ln = int(m.group(1))
        i = buf.find(b'BM', m.end())
        if i < 0:
            raise RuntimeError("BMP header marker 'BM' not found")
        data = buf[i:i + ln]
        if len(data) < ln:
            raise RuntimeError(f"BMP truncated: {len(data)}/{ln} bytes")
        with open(path, 'wb') as f:
            f.write(data)
        return path


@contextlib.contextmanager
def safe_serial(port=DEFAULT_PORT, baud=DEFAULT_BAUD, boot_wait=BOOT_WAIT_SEC):
    """Convenience context manager for one-off serial access.

    Example:
        with safe_serial() as dbs:
            print(dbs.send('status'))
    """
    dbs = DeskBarSerial(port=port, baud=baud, boot_wait=boot_wait)
    try:
        yield dbs
    finally:
        dbs.close()


def quick_cmd(cmd, port=DEFAULT_PORT, wait=1.5):
    """Open port, send one command, print response, close port."""
    with safe_serial(port=port) as dbs:
        resp = dbs.send(cmd, wait=wait)
        print(resp, end='')
        return resp
