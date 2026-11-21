# BMW NBT Evo CarLife 部署手册

适用于 QNX 6.5 ARMv7 的 NBT Evo。停车挂 P 挡后操作，通过 SSH 在 `hu-omap` 前台启动。实车已验证上传、Wi-Fi 握手、硬解上屏和退出恢复。正式安装和卸载路径留待实车完整验收。构建方法和实测结果见 [QNX 台架版](README.md)。

## 从电脑部署

在项目根目录执行，`SSH_DESTINATION` 可用 `root@车机IP` 或 `~/.ssh/config` 里的主机别名。

| 操作 | 命令 | 结果 |
| --- | --- | --- |
| 上传试跑 | `sh nbt/deploy.sh upload SSH_DESTINATION` | 上传到 `/fs/sda1/opt/nbt-carlife-stage-*/nbt-carlife`，回读逐字节比对并打印实际路径 |
| 安装或更新 | `sh nbt/deploy.sh install SSH_DESTINATION` | 暂存二进制和 `install.sh`，在 `hu-omap` 执行安装，将程序放入 `/var/opt/nbt-carlife` |
| 安装多媒体菜单 | `sh nbt/deploy.sh install-menu SSH_DESTINATION` | 校验菜单包，在当前 HMI 的 `data/ro/Plugins/NbtCarLife` 注册插件 |
| 移除多媒体菜单 | `sh nbt/deploy.sh uninstall-menu SSH_DESTINATION` | 移除有安装标记的 CarLife 菜单 |
| 卸载 | `sh nbt/deploy.sh uninstall SSH_DESTINATION` | 在 `hu-omap` 运行 `uninstall.sh`，清理脚本管理的安装文件 |

从源码检出后，先按 [Docker 交叉编译说明](README.md) 构建，并将输出目录设为 `dist/qnx-armv7le`。部署脚本使用该目录中的车机程序、菜单插件和 SHA-256 校验值。登录使用 root 密码，主机指纹要与已知车机核对。脚本在 SSH 落到 `hu-jacinto` 时通过 `on -f hu-omap` 转发，用 `pidin` 验证有效 UID。部分车机使用旧主机密钥，SSH 客户端需要支持车机实际提供的算法。密码可以在终端按提示输入，也可通过可信的凭据管理器提供 `CARLIFE_SSH_PASSWORD`。部署和运行日志输出到控制台。

上传试跑是本车已验证的路径。暂存文件初始权限是 `0600`。确认脚本打印的实际文件路径后，在 `hu-omap` 上给该文件加 `0700` 权限。本车的 `/dev/shmem` 无法创建子目录。

安装器要求 `/var/opt` 可写，使用安装标记识别受管目录，目录和程序权限为 `0700`。安装菜单前先安装主程序。菜单操作暂时重挂只读的系统分区，完毕后恢复原挂载状态。菜单在 HMI 下次启动时加载。

## 多媒体菜单

在 **多媒体 → CarLife** 可以看到投屏和防火墙状态。开机加载菜单入口，点击「开始连接」才启动投屏程序。将手机连到 BMW Wi-Fi Direct，并在 vivo「智能车载」中点 CarLife+ 无线连接。

- 「开始连接」尝试 `wfd0` 邻居，优先尝试上次连接的手机。车机也尝试原厂 Wi-Fi Direct 的 DHCP 地址池 `.100–.110`，每个候选只连接 CarLife 的 TCP 7240。多台已知邻居同时接受连接时，可在菜单按 IP 选手机。60 秒内未收到有效的版本握手就提示没有找到手机。
- 车机收到有效版本应答后，六路连通、视频格式协商和首帧共用 15 秒预算。失败或断线时退出会话，回菜单手动重试。
- 实体 `MENU` 或长按 `BACK` 隐藏投屏，触摸和旋钮交回原车。「返回投屏」恢复仍连接的会话。
- 菜单在准备显示时等待最多 22 秒。进入投屏后，连续 7 秒收不到进程心跳会启动恢复，返回 iDrive，并请求正常退出。4 秒后仍未退出时尝试从独立进程恢复图层。CarPlay 解码器已归还或尚未借用时，可以强制结束卡住的 CarLife 进程。「强制停止」使用同一条收尾路径。
- 如果原厂显示服务持续无响应，菜单会提示车机服务尚未恢复。恢复机制不会自动重启车机。更新恢复版时需同时安装可执行文件和菜单插件。
- 「检测防火墙规则」显示缺失、已写入未生效、已生效或 PF 关闭。停止投屏后可用「添加防火墙规则」备份、写入并加载 CarLife 规则。

### Wi-Fi Direct 密码

在 **多媒体 → CarLife → Wi-Fi 密码** 点「查看 Wi-Fi 密码」，可查看 SSID 和已保存的密码。页面会注明是 BMW 保存的密码、CarLife 本次开机已应用的密码，还是下次启动 CarLife 时才应用的密码。离开该页面会隐藏明文。

停止 CarLife、Supra 和原厂 CarPlay 投屏后，点「重置 Wi-Fi 密码」并确认。程序生成一个新的 16 位密码，通过 BMW 连接管理器重建当前 Wi-Fi Direct 组。手机会断开，更新手机上这条网络的保存密码后再连接。失败时程序会尝试恢复之前的 Wi-Fi Direct 组。

CarLife 把新密码保存在私有安装目录里。车机重启后先由 BMW 启用原有 Wi-Fi Direct 配置，点「开始连接」时 CarLife 会重新应用保存的密码。密码变更涉及 Wi-Fi Direct，共享该组的设备也需要更新凭据。SSH 诊断可用 `--wifi-direct-info` 查看保存的密码，或用 `--wifi-reset --confirm` 执行同样的重置。

## SSH 启动与诊断

1. 进入车机 SSH 会话。若登录得到 `hu-jacinto`，运行 `on -f hu-omap /bin/ksh`。用 `uname -n` 确认当前节点。
2. 确认原厂 CarPlay 和 Supra Android Auto 已结束投屏。一次只运行一个投屏进程。
3. 设置 `CARLIFE_BIN` 指向已安装程序。若从暂存目录试跑，填入上传时打印的完整路径。

   ```sh
   CARLIFE_BIN=/var/opt/nbt-carlife/nbt-carlife
   "$CARLIFE_BIN" --wifi-direct-info
   ```

4. 用查询到的 SSID 与凭据让 vivo 加入车机 Wi-Fi Direct。该命令会在终端显示保存的明文密码。查看 vivo 当前 Wi-Fi IPv4，并在「智能车载」中点 CarLife+ 无线连接。
5. 在 `hu-omap` 启动自动搜索和渲染。

   ```sh
   export LD_LIBRARY_PATH=/opt/sys/lib:/opt/hmi/lib:/opt/conn/lib:/opt/conn/lib/carplay:/opt/mm/lib:/usr/lib:/lib
   "$CARLIFE_BIN" --auto
   ```

   `--auto` 自动搜索。排查特定手机时可以改用 `"$CARLIFE_BIN" --wifi-render PHONE_IPV4`。默认请求 `852×480 @ 60 fps`，本车上的 vivo 回应 `848×480 @ 60 fps`，车机按实际尺寸原像素居中显示。显式请求 30 fps 时在手动地址命令末尾加 `30`。以日志里的 `CarLife phone confirmed`、mdev 格式接受和 `CarLife video started` 判断是否协商成功并收到首帧。

6. 重连时先按 Ctrl+C 或向已核对的 CarLife 进程发送 SIGTERM，等原厂界面恢复。然后在手机重新点一次无线连接，再启动同一命令。程序在 CarPlay 图层隐藏时借用硬解码器，正常退出或断线时交还。强行杀进程会跳过交还逻辑。

`--wifi PHONE_IPV4` 是兼容入口，解码器已被占用时可用于单独验证网络和首帧。TCP 7240 连不上时先核对手机 Wi-Fi IP、防火墙规则和 vivo 的连接页面。其余五路在 7240 上确认版本后开放，通道不齐或首帧超时可按上面的步骤重连。

## 车机持久防火墙

本车在 `hu-omap` 上使用 PF。`/etc` 链接到 `/fs/sda0/etc`，两个路径下的 `pf.conf` 是同一文件。新版的「添加防火墙规则」会把上次实车配置的单一 vivo 地址替换为 `172.16.222.0/24`，按同样的六个端口应用整段规则。也可在 SSH 中执行 `"$CARLIFE_BIN" --pf-check` 或 `"$CARLIFE_BIN" --pf-install`。

```pf
# nbt-carlife: outbound CarLife over Wi-Fi Direct
pass out quick on wfd0 inet proto tcp from 172.16.222.1 to 172.16.222.0/24 port { 7240, 8240, 9240, 9241, 9242, 9340 } keep state label "nbt-carlife"
```

这条规则允许车机在 `wfd0` 上主动连接 Wi-Fi Direct 网段内已知的六个 CarLife TCP 端口，`keep state` 允许对应的回包。手机在该网段内换地址时，规则继续适用。以下命令可核对现状。

```sh
ifconfig wfd0
arp -an
ls -lid /etc /etc/pf.conf /fs/sda0/etc/pf.conf
pfctl -s info
pfctl -s rules | grep 'label "nbt-carlife"'
```

### 写入与验证

菜单的添加动作在 `/var/opt/nbt-carlife/pf.before.*` 保留最近变更前的配置。以下步骤适用于手动诊断。

1. 在 `/fs/sda1/opt` 创建仅 root 可读写的备份目录。备份当前 `/fs/sda0/etc/pf.conf` 为 `pf.conf.before`，并记录 `pfctl -s rules`、`pfctl -s nat` 和 `pfctl -v -s Anchors` 的输出。
2. 从备份构造候选文件。首次添加时把上面的注释和规则放在现有过滤规则末尾。换手机 IP 时修改原有 CarLife 规则。执行 `pfctl -n -f 候选文件` 做完整语法检查，再用 `pfctl -n -v -R -f 候选文件` 核对除了 CarLife 规则外的 filter 内容与当前一致。若当前生效规则和磁盘配置有其他差异，先查明差异。
3. 确认原配置仍与备份一致。若 `/fs/sda0` 是只读挂载，临时执行 `mount -uw /fs/sda0`。本车当时为只读。首次添加用 `>>` 追加经过检查的两行。换 IP 时将已验证的候选内容写回原文件。原文件有硬链接，原位写入可保留 inode 和权限。
4. 先执行 `pfctl -n -f /etc/pf.conf` 再执行 `pfctl -R -f /etc/pf.conf`，即时加载过滤规则。确认 `pfctl -s info` 里的 `Status: Enabled`、六条带 `nbt-carlife` 标签的规则，以及 NAT、anchors 和已有规则与预期一致。原来是只读时，执行 `mount -ur /fs/sda0` 恢复原挂载状态。

本车的 Supra 已在 `/opt/aa/nbt-aad.env` 设置 `NBT_KEEP_PF=1`，使 Supra 启动时保留 PF。本次沿用此设置。 [Supra 防火墙实现](https://github.com/Sq00ky/Supra-iDrive6-Android-Auto/blob/main/milestone-7/tools/pf_aa.cpp) · [QNX pfctl 说明](https://www.qnx.com/developers/docs/6.5.0SP1/neutrino/utilities/p/pfctl.html)

### 回滚

回滚前确认备份对应本次操作。写入或加载失败时，把最近操作前的备份原位写回配置并用 `pfctl -n -f` 检查，再以 `pfctl -R -f` 恢复过滤规则和原挂载状态。若写入后已有其他配置变更，从当前配置中移除 CarLife 注释和规则，重新检查后加载，以保留后续的变更。

## 卸载范围

先退出投屏，再从电脑运行 `uninstall-menu` 和 `uninstall`。脚本移除受管菜单和程序，保留私有配置目录与 PF 备份。手动试跑的文件和持久 PF 规则分别按实际路径和上述回滚步骤处理。

部署方式参考 [Supra 安装指南](https://github.com/Sq00ky/Supra-iDrive6-Android-Auto/blob/main/install.md) 和 [CarPlay Advanced 安装器](https://github.com/st1ng/nbtevo-carplay-advanced/blob/main/carplay-advanced.sh)。
