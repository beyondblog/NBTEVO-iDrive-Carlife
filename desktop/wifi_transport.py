"""Six TCP lanes used by the CarLife hotspot profile."""

import select
import socket
import struct
from contextlib import ExitStack, contextmanager

from vendor.hu_lab.protocol import CMD, MAX_INNER, TOUCH, FrameParser, ProtocolError

PORTS = (7240, 8240, 9240, 9241, 9242, 9340)


class WifiTransport:
    @classmethod
    @contextmanager
    def connect(cls, host, stop):
        with ExitStack() as resources:
            transport = cls(host, stop, resources)
            transport.open_lane(CMD)
            yield transport

    def __init__(self, host, stop, resources):
        self.host = host
        self.stop = stop
        self.resources = resources
        self.sockets = {}
        self.lanes = {}
        self.buffers = {}
        self.parser = FrameParser()

    def open_lane(self, channel):
        if self.stop.is_set():
            raise InterruptedError()
        port = PORTS[channel - 1]
        try:
            lane = self.resources.enter_context(
                socket.create_connection((self.host, port), timeout=1.5)
            )
        except OSError as error:
            raise OSError(f"手机 TCP {port} 连接失败 · {error}") from error
        lane.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.sockets[channel] = lane
        self.lanes[lane] = channel
        self.buffers[channel] = bytearray()

    def open_media_lanes(self):
        for channel in range(2, len(PORTS) + 1):
            self.open_lane(channel)

    def send(self, packet):
        self.sockets[packet.channel].sendall(packet.encode()[8:])

    def receive(self, timeout):
        ready, _, _ = select.select(self.lanes, [], [], timeout)
        for lane in ready:
            channel = self.lanes[lane]
            chunk = lane.recv(32768)
            if not chunk:
                raise OSError(f"手机关闭 TCP {PORTS[channel - 1]}")
            pending = self.buffers[channel]
            pending.extend(chunk)
            command = channel in (CMD, TOUCH)
            header = 8 if command else 12
            offset = 0
            while len(pending) - offset >= header:
                length = struct.unpack_from(">H" if command else ">I", pending, offset)[0]
                size = header + length
                if size > MAX_INNER:
                    raise ProtocolError("Wi-Fi payload exceeds frame limit")
                if len(pending) - offset < size:
                    break
                outer = struct.pack(">II", channel, size)
                yield from self.parser.feed(outer + pending[offset:offset + size])
                offset += size
            del pending[:offset]
