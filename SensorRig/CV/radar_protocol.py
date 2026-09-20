"""LD2450 V1.03 diagnostic commands. No persistent configuration writes."""
import struct
import time

COMMAND_HEADER = bytes.fromhex('FD FC FB FA')
COMMAND_FOOTER = bytes.fromhex('04 03 02 01')


def command_frame(command, value=b''):
    body = struct.pack('<H', command) + value
    return COMMAND_HEADER + struct.pack('<H', len(body)) + body + COMMAND_FOOTER


class ReplyParser:
    def __init__(self):
        self.buffer = bytearray()

    def feed(self, chunk):
        self.buffer.extend(chunk)
        replies = []
        while True:
            index = self.buffer.find(COMMAND_HEADER)
            if index < 0:
                self.buffer[:] = self.buffer[-3:]
                break
            del self.buffer[:index]
            if len(self.buffer) < 6:
                break
            length = struct.unpack_from('<H', self.buffer, 4)[0]
            if not 4 <= length <= 128:
                del self.buffer[0]
                continue
            end = 6 + length
            if len(self.buffer) < end + 4:
                break
            if self.buffer[end:end+4] != COMMAND_FOOTER:
                del self.buffer[0]
                continue
            command, status = struct.unpack_from('<HH', self.buffer, 6)
            replies.append((command, status, bytes(self.buffer[10:end])))
            del self.buffer[:end+4]
        return replies


def diagnostic_query(uart, command, value=b'', timeout=.6):
    uart.write(command_frame(command, value))
    parser = ReplyParser()
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        for reply, status, data in parser.feed(uart.read(max(1, min(uart.in_waiting, 4096)))):
            if reply == command | 0x100:
                if status:
                    raise ValueError(f'Radar command {command:#x} rejected ({status})')
                return data
    raise TimeoutError(f'No reply to radar command {command:#x}; check radar RX wiring')


def read_diagnostics(uart):
    """Enter configuration, read firmware/mode/zones, always exit config.

    These queries neither change tracking mode nor erase configured zones.
    Diagnostic failure must not disable a working one-way data connection.
    """
    result = {}
    try:
        diagnostic_query(uart, 0xff, struct.pack('<H', 1))
        version = diagnostic_query(uart, 0xa0)
        if len(version) != 8:
            raise ValueError('Unexpected firmware reply length')
        kind, major, build = struct.unpack('<HHI', version)
        result['firmware'] = f'V{major >> 8:x}.{major & 0xff:02x}.{build:08x}'
        mode = diagnostic_query(uart, 0x91)
        if len(mode) != 2:
            raise ValueError('Unexpected tracking mode reply length')
        result['tracking_mode'] = {1: 'single', 2: 'multiple'}.get(struct.unpack('<H', mode)[0], 'unknown')
        zones = diagnostic_query(uart, 0xc1)
        if len(zones) != 26:
            raise ValueError('Unexpected zone reply length')
        values = struct.unpack('<13h', zones)
        result['zone_filter'] = {0: 'disabled', 1: 'include', 2: 'exclude'}.get(values[0], 'unknown')
        result['zones_mm'] = [list(values[i:i+4]) for i in (1, 5, 9)]
    except (ValueError, TimeoutError, OSError) as exc:
        result['query_error'] = str(exc)
    finally:
        try:
            diagnostic_query(uart, 0xfe)
        except (ValueError, TimeoutError, OSError) as exc:
            result['exit_error'] = str(exc)
    return result
