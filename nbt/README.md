# NBT Evo 原生 CarLife 台架版

目标是 BMW NBT Evo ID5／ID6，使用 QNX 6.5 ARMv7LE。这个入口是独立的 `nbt-carlife` 进程。它复用 Supra 项目的 QNX USB、显示和原车输入模块，使用本项目的 CarLife 会话处理手机报文。

- Wi-Fi 模式连接手机 IPv4 上的六路 TCP 端口 7240、8240、9240、9241、9242、9340。命令、视频和触控通过各自的通道传输。
- USB 模式使用 `UsbDirect`，按历史 CarLife 配件标识切换 AOA，并分两次写入外层头和消息体。
- 无线渲染请求手机直接编码 852 × 480。vivo 实测回报 848 × 480，车机按确认尺寸原像素居中显示。默认请求 60 fps，支持命令末尾指定帧率。SPS/PPS 从 Annex B 数据中提取后送给车机硬件解码器。
- 原车触屏发送单指按下、移动和抬起。iDrive 的旋转、按压、方向和返回转换为 CarLife 硬键。旋转方向与 vivo 焦点行为需要在台架确认。
- 首个已入解码队列的视频帧触发投屏显示。退出或断线后隐藏图层并交还触屏。当前进程一次处理一条投屏会话，断线后退出；下一次测试重新启动进程。
- 「多媒体 → CarLife」的 RHMI 插件提供手动启动、返回投屏、强制停止、检测和添加防火墙规则，以及查看和重置 Wi-Fi Direct 密码。自动发现和按键操作见 [部署手册](INSTALL.md)。
- CarLife 的媒体、导航提示和语音播放 PCM 通道接到 Supra 的原生 Browser Audio 后端，经 libasound 和 `audioconn` 播放。切回 iDrive 释放这条音频路由，返回时按当前手机流状态恢复。可用 `NBT_CARLIFE_AUDIO=0` 排查音频问题。

## 已编译的台架产物

仓库中的 [nbt-carlife](../dist/qnx-armv7le/nbt-carlife) 是 QNX ARMv7LE 可执行文件，使用 QNX 6.5 启动对象和目标库，加载器为 `/usr/lib/ldqnx.so.2`。C++ 运行库已静态链接。可执行文件与菜单包分别使用 [SHA256SUMS](../dist/qnx-armv7le/SHA256SUMS) 和 [SHA256SUMS.plugin](../dist/qnx-armv7le/SHA256SUMS.plugin) 校验。

## 在 Lima Ubuntu 中使用 Docker 构建

`nbt/docker-build.sh` 在容器中运行完整的 `make`。脚本挂载项目、Supra 源码和 QNX sysroot，复用本地编译镜像，并将可执行文件、RHMI 插件和校验值写入指定目录。Docker 宿主使用 Python 3 生成菜单资源。已在这台 Mac 的 Lima Ubuntu 环境内构建。

在 Lima 的 shell 中运行，三个参数都是虚拟机里可见的路径：

```sh
sudo sh /path/to/project/nbt/docker-build.sh \
  /path/to/supra/milestone-7 /path/to/sdp/target/qnx6 /tmp/carlife-output
```

## 使用 Clang 和 lld 重建

构建使用 [QNX 6.5 ARMv7 sysroot](https://github.com/luka-dev/qnx65-armv7-toolchain/tree/baa45224f18ae6b13c2e9c77c6663bb8181eb6a1/sdp/target/qnx6) 和下文固定版本的 Supra 源码。设置 `QNX_TARGET` 为 sysroot 的 `sdp/target/qnx6` 目录，`SUPRA_SRC` 为 Supra 的 `milestone-7` 目录。

```sh
make -f nbt/clang-qnx.mk QNX_TARGET="$QNX_TARGET" SUPRA_SRC="$SUPRA_SRC"
```

输出为 `build/qnx-armv7le/nbt-carlife`。如果 Clang 和 lld 在不同的主机，先在 Clang 主机执行 `objects` 目标，将输出目录的对象文件复制到 lld 主机，再用该主机的目录参数执行 `link` 目标。当前发布产物由 Docker 容器中的 Clang 14 和 lld 14 生成。

## 使用 Supra 的 qcc 环境构建

需要有许可证的 QNX SDP 6.5 ARMv7 工具链，以及
[Supra-iDrive6-Android-Auto 固定版本](https://github.com/Sq00ky/Supra-iDrive6-Android-Auto/tree/5ff16924c77394d2f6fbe12c71e92742913a730a)。
在本项目的根目录中执行，`SUPRA_REPO` 指向该版本的本地检出目录：

```sh
CARLIFE_NBT="$PWD/nbt"
make -C "$SUPRA_REPO/milestone-7" -f Makefile -f "$CARLIFE_NBT/nbt.mk" \
  CARLIFE_NBT="$CARLIFE_NBT" nbt-carlife
```

生成位置为 `$SUPRA_REPO/milestone-7/nbt-carlife`。链接的 Supra 模块适用其
[PolyForm Noncommercial 1.0.0 许可证](https://github.com/Sq00ky/Supra-iDrive6-Android-Auto/blob/5ff16924c77394d2f6fbe12c71e92742913a730a/LICENSE.md)。
Copyright (c) 2026 nanaisu - NBT-AAD。

## 实车验收记录

部署、启动、重连和 PF 操作见 [BMW NBT Evo 部署手册](INSTALL.md)。实车通过 SSH 前台运行，QNX USB 台架入口为 `./nbt-carlife --bench`。

- **网络**　vivo X Fold5 加入车机现有 Wi-Fi Direct 后，在内置「智能车载」手动发起 CarLife+。车机先连 TCP 7240，待手机确认协议版本后再连其余五路。已在车机通过这条路径多次重连并上屏。
- **画面**　面板为 1280 × 480。原先 1280 × 720 以原尺寸显示会裁掉底部。当前由 vivo 直接编码，852 × 480 的请求在 30 和 60 fps 下均回报 848 × 480。QNX Screen 实测源区和目标区均为 848 × 480，目标起点是 `(216, 0)`，使用独立图层 1284。
- **帧率**　vivo 已确认 60 fps，车机 mdev 也接受了 848 × 480 @ 60 fps。一次上屏后的约 9 秒样本中，解码器平均收到约 32 帧/秒。持续 60 帧和高负载表现留待后续测量。
- **退出**　已验证正常退出时，CarLife 释放自己的解码上下文，恢复原厂界面，并交还已有 CarPlay 解码器。
- **车机切屏**　原厂 iDrive 和 CarLife 分别使用图层 256 和 1284。切换时先显示目标图层，再隐藏旧图层。屏幕尺寸从 QNX Screen 读取，失败时默认 1280 × 480，可用 `NBT_SCREEN` 指定。显示和隐藏以最近一次请求为准。
- **实车往返**　同一会话完成两次切回 iDrive 和返回投屏。车机日志连续记录了显示、隐藏和重新显示。
- **实车音频**　无线车机在视频初始化前发送 `CONNECT_TYPE=5` 和 `MEDIA_SAMPLE_RATE=0`，声明 Wi-Fi 并优先使用 44.1 kHz。更新后实测收到 vivo 的 44.1 kHz、16 位立体声 PCM，并确认车内喇叭能播放音乐。[CarLife 特性协商参考](https://github.com/jjuugk/CarPToCarL/blob/main/app/src/main/java/io/github/shaonialife/carptocar/carlife/CarLifeProtocol.kt)
- **实体音量**　复用 Supra 的 AudioMaster 音量监听，将读到的百分比应用到 CarLife 的音乐增益。实车重连后读到了 25% 至 46% 的音量变化。投屏内音量条仍待接入。
- **启动故障与恢复**　音量版曾在启动时遇到图层查询超时。新版只在借用解码器前确认 CarPlay 画面是否隐藏，查询失败会短暂重试一次。实车模拟投屏进程暂停时，iDrive 图层已在第 12 秒恢复可见。解除暂停后，CarLife 正常退出并清理自己的解码器上下文。实车再次从菜单启动已成功上屏。
- **待验收**　音频路由释放、受管卸载、蓝牙唤醒和手机自动入网。

Supra 的 Android Auto WPP 通过蓝牙 RFCOMM 协商无线接入。本项目从已加入 BMW Wi-Fi Direct 的手机中自动寻找 CarLife 服务，手机在「智能车载」中手动开启监听。

在电脑运行 `sh nbt/test-host.sh` 可验证版本应答、原生分辨率协商、显示命令超时、触控释放、提前到达的 PCM 格式及其启停、Wi-Fi 邻居选择、单 IP PF 规则迁移和 Wi-Fi 密码生成。密码重置、失败回滚和重启后重新应用需要实车验证。
