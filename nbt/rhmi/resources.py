"""Build original text and image resources for the RHMI CarLife menu."""

import binascii
import struct
import sys
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile
import zlib


ENGLISH = {
    1: "CarLife", 2: "Start connection", 3: "Resume projection", 4: "Force stop",
    5: "Check firewall rules", 6: "Add firewall rules",
    7: "Wi-Fi password", 8: "Show Wi-Fi password", 9: "Reset Wi-Fi password",
    10: "Confirm reset", 11: "Wi-Fi Direct devices will disconnect. Reconnect with the new password.",
    100: "Ready", 101: "Looking for CarLife phones", 102: "Connecting to phone",
    103: "Projecting", 104: "Connected, screen hidden", 105: "No phone found in 60 s",
    106: "Timed out waiting for first picture", 107: "Disconnected",
    108: "Cannot start display", 109: "Stopped", 110: "Choose one phone",
    111: "Applying saved Wi-Fi password", 112: "Could not apply saved Wi-Fi password", 113: "No device",
    120: "Firewall status unknown", 121: "Checking or updating firewall",
    122: "CarLife firewall rules active", 123: "Rules saved, not active",
    124: "CarLife rules missing", 125: "Rules saved, PF disabled",
    126: "Firewall check or update failed", 127: "Stopping CarLife",
    128: "Waiting for vehicle services to finish", 129: "PF disabled, CarLife rules missing",
    130: "Stop CarLife before updating firewall",
    131: "CarLife timed out. Returning to iDrive",
    132: "CarLife recovered. Start again when ready",
    140: "Password hidden", 141: "Reading Wi-Fi password", 142: "Resetting Wi-Fi password",
    143: "BMW saved password", 144: "CarLife password last applied this boot",
    145: "Saved. Applied when you start CarLife.", 146: "Wi-Fi Direct or BMW password unavailable",
    147: "Stop projection before changing Wi-Fi", 148: "Wi-Fi password reset did not complete",
    149: "Previous Wi-Fi could not be restored. Check after restarting the head unit.",
}
CHINESE = {
    1: "CarLife", 2: "开始连接", 3: "返回投屏", 4: "强制停止",
    5: "检测防火墙规则", 6: "添加防火墙规则",
    7: "Wi-Fi 密码", 8: "查看 Wi-Fi 密码", 9: "重置 Wi-Fi 密码",
    10: "确认重置", 11: "Wi-Fi Direct 设备会断开，请用新密码重新连接。",
    100: "待连接", 101: "正在搜索 CarLife 手机", 102: "正在连接手机",
    103: "正在投屏", 104: "已连接，画面已隐藏", 105: "60 秒内未找到手机",
    106: "等待投屏首帧超时", 107: "连接已断开",
    108: "显示服务无法启动", 109: "已停止", 110: "请选择一部手机",
    111: "正在应用保存的 Wi-Fi 密码", 112: "无法应用保存的 Wi-Fi 密码", 113: "空",
    120: "防火墙状态待检测", 121: "正在处理防火墙规则",
    122: "CarLife 防火墙规则已生效", 123: "规则已保存，尚未生效",
    124: "缺少 CarLife 防火墙规则", 125: "规则已保存，PF 当前关闭",
    126: "防火墙操作失败", 127: "正在停止 CarLife", 128: "正在等待车机服务完成",
    129: "PF 当前关闭，且缺少 CarLife 规则", 130: "请先停止 CarLife 再更新防火墙",
    131: "CarLife 超时，正在返回 iDrive", 132: "CarLife 已退出，可以重新开始连接",
    140: "密码已隐藏", 141: "正在读取 Wi-Fi 密码", 142: "正在重置 Wi-Fi 密码",
    143: "BMW 保存的密码", 144: "CarLife 本次开机已应用的密码",
    145: "已保存，下次点开始连接时应用", 146: "Wi-Fi Direct 或 BMW 密码不可用",
    147: "请先结束投屏再更改 Wi-Fi", 148: "Wi-Fi 密码重置未完成",
    149: "旧 Wi-Fi 未恢复，重启车机后再检查",
}


def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", binascii.crc32(kind + data))


def icon():
    width, height = 63, 54
    rows = bytearray()
    for y in range(height):
        rows.append(0)
        for x in range(width):
            ring = 15 ** 2 < (x - 31) ** 2 + (y - 27) ** 2 <= 19 ** 2
            play = 25 <= x <= 39 and abs(y - 27) <= (39 - x) * 0.63
            rows.extend((110, 208, 255, 255) if ring or play else (0, 0, 0, 0))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def build(directory):
    directory.mkdir(parents=True, exist_ok=True)
    with ZipFile(directory / "NbtCarLife_ALL_Texts.zip", "w", ZIP_DEFLATED) as archive:
        for locale, label, translations in (
            ("en-US", "English", ENGLISH), ("en-GB", "English", ENGLISH),
            ("de-DE", "Deutsch", ENGLISH), ("zh-CN", "简体中文", CHINESE),
        ):
            devices = {1000 + i: f"172.16.222.{i}" for i in range(2, 255)}
            rows = "\n".join(f"{number}={text}" for number, text in sorted({**translations, **devices}.items()))
            archive.writestr(f"{locale}.txt", f"100000={label}\n{rows}\n".encode())
    with ZipFile(directory / "NbtCarLife_ALL_Images.zip", "w", ZIP_DEFLATED) as archive:
        archive.writestr("55001.png", icon())


if __name__ == "__main__":
    build(Path(sys.argv[1]))
