"""Desktop CarLife USB and Wi-Fi head unit with live H.264 and single-touch input."""
import argparse
from ipaddress import IPv4Address
from collections import deque
from dataclasses import dataclass
from threading import Event, Lock
import time

import av
from PySide6.QtCore import QRect, Qt, QThread, Signal
from PySide6.QtGui import QImage, QPainter
from PySide6.QtWidgets import QApplication, QLabel, QMainWindow, QWidget

from vendor.hu_lab.protocol import (
    CMD, TOUCH, TOUCH_ACTION, VIDEO_DATA, VIDEO_PAUSE, FrameParser, HUSession,
    Packet, protobuf,
)
from vendor.hu_lab.usb import LibUSB, USBIdentity

from .wifi_transport import WifiTransport

CARLIFE_IDENTITY = (
    "Baidu", "CarLife", "Baidu CarLife", "1.0.0",
    "http://carlife.baidu.com/", "0720SerialNo.",
)
DOWN, UP, MOVE = 0, 1, 2


def usb_identity(descriptor):
    info = descriptor["identity"]
    return USBIdentity(**{**info, "ports": tuple(info["ports"])})


@dataclass(frozen=True)
class SelectedDevice:
    identity: USBIdentity

    def require(self, identity):
        if identity != self.identity:
            raise ValueError("USB device changed before the requested operation")


class Projection(QThread):
    status = Signal(str)
    frame_available = Signal()

    def __init__(self, port, vendor, parent=None, wifi=None):
        super().__init__(parent)
        self.port, self.vendor, self.wifi = port, vendor, wifi
        self.stop = Event()
        self.lock = Lock()
        self.touches = deque()
        self.frame = None

    def input(self, action, x, y):
        with self.lock:
            if action == MOVE and self.touches and self.touches[-1][0] == MOVE:
                self.touches.pop()
            self.touches.append((action, x, y))

    def take_frame(self):
        with self.lock:
            frame, self.frame = self.frame, None
            return frame

    def publish(self, image):
        with self.lock:
            notify = self.frame is None
            self.frame = image
        if notify:
            self.frame_available.emit()

    def run(self):
        if self.wifi:
            self.run_wifi()
            return
        try:
            with LibUSB() as usb:
                while not self.stop.is_set():
                    descriptor = self.wait_for_phone(usb)
                    if descriptor is None:
                        break
                    with self.lock:
                        self.touches.clear()
                    try:
                        self.connect_phone(usb, descriptor)
                    except InterruptedError:
                        return
                    except (OSError, RuntimeError, ValueError) as error:
                        self.status.emit(f"连接中断 · {error} · 请重新插拔 USB")
                        while not self.stop.is_set() and any(
                            device["physical_key"] == self.port for device in usb.discover()
                        ):
                            self.stop.wait(0.25)
        except (OSError, RuntimeError, ValueError) as error:
            self.status.emit(str(error))

    def run_wifi(self):
        self.status.emit(f"正在连接手机 · Wi-Fi {self.wifi}")
        try:
            with WifiTransport.connect(self.wifi, self.stop) as phone:
                self.stream(phone)
        except InterruptedError:
            pass
        except (OSError, RuntimeError, ValueError) as error:
            self.status.emit(f"Wi-Fi 连接中断 · {error}")

    def wait_for_phone(self, usb):
        self.status.emit(f"等待手机 · USB {self.port} · 已插入时请重新插拔")
        while not self.stop.is_set():
            devices = [
                device for device in usb.discover()
                if device["physical_key"] == self.port
                and device["identity"]["vid"] == self.vendor
            ]
            if len(devices) == 1:
                return devices[0]
            self.stop.wait(0.25)
        return None

    def connect_phone(self, usb, descriptor):
        identity = usb_identity(descriptor)
        self.status.emit("正在启动 CarLife USB 配件模式")
        with usb.open_exact(identity) as phone:
            phone.start_accessory(CARLIFE_IDENTITY, SelectedDevice(identity))
        accessory = usb.wait_accessory(identity, self.stop)
        accessory_id = usb_identity(accessory)
        if self.stop.wait(2):
            return
        with usb.open_exact(accessory_id) as phone:
            phone.claim_accessory(SelectedDevice(accessory_id))
            self.stream(phone)

    @staticmethod
    def send(phone, packet):
        if isinstance(phone, WifiTransport):
            phone.send(packet)
            return
        wire = packet.encode()
        # Historical CarLife HU writes the outer USB header separately.
        phone.write(wire[:8])
        phone.write(wire[8:])

    def stream(self, phone):
        session, parser = HUSession(), FrameParser()
        wifi = isinstance(phone, WifiTransport)
        decoder = av.CodecContext.create("h264", "r")
        request, = session.start()
        self.send(phone, request)
        self.status.emit("正在协商 CarLife 视频")
        deadline = time.monotonic() + 18
        retry = None if wifi else time.monotonic() + 7
        last_video = None
        contact = None

        while not self.stop.is_set():
            now = time.monotonic()
            if retry and now >= retry and session.state == "WAIT_VERSION":
                self.send(phone, request)
                retry = None
            if last_video is None and now >= deadline:
                raise TimeoutError("等待首帧超时")
            if last_video is not None and now - last_video > 15:
                raise TimeoutError("视频流停止")

            with self.lock:
                events = tuple(self.touches)
                self.touches.clear()
            if last_video is not None:
                for action, x, y in events:
                    self.send(phone, Packet(TOUCH, TOUCH_ACTION, protobuf(f1=action, f2=x, f3=y)))
                    contact = None if action == UP else (x, y)

            if wifi:
                packets = phone.receive(timeout=0.05)
            else:
                try:
                    packets = parser.feed(phone.read(timeout=0.05))
                except TimeoutError:
                    continue
            for packet in packets:
                waiting_for_version = session.state == "WAIT_VERSION"
                replies = session.receive(packet)
                if wifi and waiting_for_version and session.state == "WAIT_INIT":
                    phone.open_media_lanes()
                for reply in replies:
                    self.send(phone, reply)
                if packet.message != VIDEO_DATA:
                    continue
                for decoded in decoder.decode(av.Packet(packet.payload)):
                    frame = decoded.reformat(format="rgb24")
                    plane = frame.planes[0]
                    image = QImage(bytes(plane), frame.width, frame.height,
                                   plane.line_size, QImage.Format.Format_RGB888).copy()
                    self.publish(image)
                    if last_video is None:
                        self.status.emit(f"实时投屏 · {frame.width} × {frame.height} · 鼠标点击或拖动")
                    last_video = time.monotonic()

        if session.state == "STREAMING":
            if contact is not None:
                x, y = contact
                self.send(phone, Packet(TOUCH, TOUCH_ACTION, protobuf(f1=UP, f2=x, f3=y)))
            self.send(phone, Packet(CMD, VIDEO_PAUSE))


class Screen(QWidget):
    def __init__(self, projection):
        super().__init__()
        self.projection = projection
        self.frame = QImage()
        self.contact = None
        projection.frame_available.connect(self.refresh)

    def refresh(self):
        frame = self.projection.take_frame()
        if frame is not None:
            self.frame = frame
            self.update()

    def reset(self):
        self.contact = None
        self.frame = QImage()
        self.update()

    def picture(self):
        size = self.frame.size().scaled(self.size(), Qt.AspectRatioMode.KeepAspectRatio)
        return QRect((self.width() - size.width()) // 2,
                     (self.height() - size.height()) // 2, size.width(), size.height())

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.fillRect(self.rect(), Qt.GlobalColor.black)
        if not self.frame.isNull():
            painter.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)
            painter.drawImage(self.picture(), self.frame)

    def point(self, event, clamp=False):
        picture = self.picture()
        if picture.isEmpty():
            return None
        pos = event.position().toPoint()
        if not clamp and not picture.contains(pos):
            return None
        x = max(0, min(picture.width() - 1, pos.x() - picture.x()))
        y = max(0, min(picture.height() - 1, pos.y() - picture.y()))
        return x * self.frame.width() // picture.width(), y * self.frame.height() // picture.height()

    def mousePressEvent(self, event):
        if event.button() == Qt.MouseButton.LeftButton:
            self.contact = self.point(event)
            if self.contact:
                self.projection.input(DOWN, *self.contact)

    def mouseMoveEvent(self, event):
        if self.contact is not None:
            point = self.point(event, clamp=True)
            if point is not None and point != self.contact:
                self.contact = point
                self.projection.input(MOVE, *point)

    def mouseReleaseEvent(self, event):
        if event.button() == Qt.MouseButton.LeftButton and self.contact is not None:
            self.projection.input(UP, *(self.point(event, clamp=True) or self.contact))
            self.contact = None


class Window(QMainWindow):
    def __init__(self, args):
        super().__init__()
        self.setWindowTitle(f"CarLife {'Wi-Fi' if args.wifi else 'USB'} 模拟器")
        self.resize(960, 576)
        self.projection = Projection(args.device, args.vendor, self, args.wifi)
        self.screen = Screen(self.projection)
        self.setCentralWidget(self.screen)
        self.label = QLabel()
        self.statusBar().addWidget(self.label)
        self.projection.status.connect(self.show_status)
        self.projection.start()

    def show_status(self, message):
        if not message.startswith("实时投屏"):
            self.screen.reset()
        self.label.setText(message)
        print(message, flush=True)

    def closeEvent(self, event):
        self.projection.stop.set()
        self.projection.wait()
        event.accept()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", default="2:1", help="USB physical path (bus:port)")
    parser.add_argument("--vendor", type=lambda value: int(value, 16), default=0x2D95,
                        help="hex USB vendor ID before AOA (default: vivo 2d95)")
    parser.add_argument("--wifi", type=IPv4Address, help="phone IPv4 for CarLife Wi-Fi")
    args = parser.parse_args()
    if args.wifi:
        args.wifi = str(args.wifi)
    app = QApplication([])
    window = Window(args)
    window.show()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
