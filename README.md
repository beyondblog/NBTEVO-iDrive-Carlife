# NBTEVO iDrive Carlife+

面向 BMW NBT Evo iDrive 5／6 的 CarLife+ 投屏项目，包含 QNX 车机程序、iDrive 菜单插件和桌面模拟器。

| 目录 | 内容 |
| --- | --- |
| [`nbt/`](nbt/) | QNX 投屏会话、无线发现、RHMI 菜单、构建和部署脚本 |
| [`desktop/`](desktop/) | 桌面 USB／Wi-Fi 模拟器及离线投屏 Demo |
| [`vendor/hu_lab/`](vendor/hu_lab/) | 第三方 CarLife 协议及 USB 模块 |
| `dist/qnx-armv7le/` | 本地构建生成的 QNX ARMv7LE 安装产物及 SHA-256 校验值 |

第三方组件及其许可证见 [NOTICE](NOTICE)。

[BMW NBT Evo 部署手册](nbt/INSTALL.md) 记录多媒体菜单、部署、启动与重连、持久 PF 规则和回滚。构建方法和上次实车结论见 [QNX 台架版](nbt/README.md)。回到电脑后可先做下面的离线测试。

## 电脑离线自测

在仓库根目录运行 `sh nbt/test-host.sh` 可执行 C++ 测试，覆盖协议协商、显示恢复、触控、PCM、Wi-Fi 发现及防火墙规则处理。GitHub Actions 同时运行 C++ 测试、离线投屏、脚本语法检查和 RHMI 菜单资源生成检查。

准备 Python 3 和带 `libx264` 编码器的 FFmpeg。在仓库根目录运行：

```sh
python3 demo.py
```

终端会显示模拟手机和车机间的版本协商、视频启动、H.264 首帧解码和中心点触控回传，并给出解码后的 PNG 路径。可以用 `--output PATH` 指定一个尚未存在的目录。

Demo 通过内存字节流交换 CarLife 格式的数据包。画面来自 FFmpeg 测试图，触控使用固定坐标。已有本地 H.264 录制文件时，可用 `ffplay -f h264 录制文件.h264` 回放，或用 `ffmpeg -v error -xerror -f h264 -i 录制文件.h264 -f null -` 检查解码。

2026-11-21 在 Mac 上复测通过。离线 Demo 完成握手、首帧与触控回传，vivo 实录 1280 × 720 H.264 可完整解码，C++ 会话通过 852／848 × 480 和 60／30 fps 协商用例。

消息参考 [CarLife-Vehicle-Lib](https://github.com/674809/carlife/tree/f9522db/CarLife-Vehicle-Lib) 和 [Carlife_on_box 协议实现](https://github.com/SadykovIV/Carlife_on_box/blob/d9397d9/hu_lab/protocol.py)。

## 真机 USB 模拟器

安装系统的 libusb 1.0（macOS 可运行 `brew install libusb`），然后在项目目录运行：

```sh
python3.12 -m venv .venv
.venv/bin/python -m pip install -r desktop/requirements.txt
.venv/bin/python simulator.py
```

默认选择已联调的 vivo X Fold5 和 Mac USB 端口 `2:1`。窗口打开后重新插入手机，保持蓝牙连接并使用手机内置「智能车载」。收到画面后，点击或拖动画面发送单指触摸。更换 USB 端口或机型时可通过 `--device` 和 `--vendor` 指定。端口和 VID 可用 `python -c 'from vendor.hu_lab.usb import LibUSB; u=LibUSB(); print([(d["physical_key"], hex(d["identity"]["vid"])) for d in u.discover()]); u.close()'` 查看。

模拟器通过 AOA 连接，持续解码 H.264，断线后等待重新插入。它默认在内存中处理画面。本版覆盖桌面画面和单指触摸，没有音频输出。

## BMW NBT Evo 移植

[QNX 台架版](nbt/README.md) 包含 CarLife C++ 会话、Supra 车机接口适配、Docker 交叉编译和 BMW 上屏的实测记录。

### 在 Mac 测试无线 CarLife

Mac 和手机处于同一可达的 Wi-Fi 网络时，在 vivo 的「智能车载」中选择 CarLife+ 无线连接。从手机当前网络的详情里读取 IPv4，然后启动：

```sh
.venv/bin/python simulator.py --wifi 手机IPv4
```

如果内置智能车载要求手机开热点，让 Mac 加入手机热点，再用热点网关地址作为手机 IPv4。无线模拟器直连手机的六路 CarLife TCP 服务。窗口会显示第一个连接失败的端口。已用 vivo X Fold5 内置 CarLife+ 在同一 Wi-Fi 网络完成真机 1280 × 720 实时投屏。触摸回传已通过本机模拟手机测试，真机操作响应仍需确认。
