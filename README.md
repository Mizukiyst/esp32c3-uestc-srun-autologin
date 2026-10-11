# ESP32-C3 深澜校园网自动认证

这是一个基于 ESP-IDF 的 ESP32-C3 深澜（Srun）自动认证工程，适用于深澜认证页面类似以下地址的校园网：

```text
http://10.253.0.235/srun_portal_pc?ac_id=3&theme=pro
```

## 功能

- 上电或复位后运行配置 AP 和上游 WiFi STA，定时唤醒时仅运行 STA
- 手机连接 `ESurfing-Config` 配置 WiFi 与校园网账号
- 内置 HTTP 配置页面：`http://192.168.4.1`
- 支持 challenge、XEncode、深澜 Base64、HMAC-MD5、SHA-1
- 自动检测在线状态，掉线后自动重新认证
- 每次唤醒检查深澜在线状态，发现掉线立即重新认证，随后深度睡眠 120 秒
- 睡眠期间关闭 WiFi 和配置热点，唤醒后重新加载配置并连接 WiFi
- 板载指示灯 GPIO8 默认关闭
- 配置保存在 SPIFFS，重启后自动恢复

## 快速使用

1. 编译工程：

   ```powershell
   ..\.tools\venv\Scripts\pio.exe run
   .\export_srun_firmware.ps1
   ```

2. 使用 Flash Download Tool 选择 `ESP32-C3 / DIO / 4MB`。
3. 初次烧录写入：

   ```text
   0x0000   release/ESP32-C3-SRUN/bootloader.bin
   0x8000   release/ESP32-C3-SRUN/partition-table.bin
   0x10000  release/ESP32-C3-SRUN/esp32c3_srun.bin
   0x2D0000 release/ESP32-C3-SRUN/spiffs.bin
   ```

4. 手机连接 `ESurfing-Config`，密码为 `esurfing2024`。
5. 打开 `http://192.168.4.1/`，填写上游 2.4 GHz WiFi 和深澜账号。

已配置过的设备更新程序时只烧录 `0x10000` 的应用文件，以保留 SPIFFS 配置。

## 深度睡眠与重新配置

设备完成一次在线检查或登录尝试后，关闭 WiFi 并深度睡眠 120 秒，由 RTC 定时器唤醒。唤醒后从 SPIFFS 读取原有配置，等待 WiFi 连接（最多 30 秒），再检查认证状态。失败时也会进入睡眠，下次唤醒后重试。两次检查的实际间隔为 120 秒加上启动、连接和认证所需时间。

尚未保存配置时，配置热点持续开启，不进入睡眠。已有配置时，重新上电、按 RESET 或通过网页重启可获得 2 分钟配置窗口；窗口结束后进入睡眠。定时唤醒不提供配置窗口，睡眠期间无法打开 `http://192.168.4.1/`。保存配置后会重启并使用新配置。

深睡期间 ESP32 的 WiFi 连接会断开。校园网会话能否保留取决于路由器和认证服务器；如果网络对 WiFi 离线立即销户，或重新连接后获得不同 IP，可能需要每次唤醒重新认证。请结合实际网络验证。整板电流还受稳压器、USB 电路和硬件电源灯影响，软件无法保证整板达到芯片标称的微安级功耗。

## 深澜配置

默认值：

```text
服务器：10.253.0.235
AC ID：3
认证 IP：留空
```

如果账号要求运营商后缀，例如 `学号@cmcc`，用户名填写学号，运营商后缀填写 `cmcc`。如果用户名已经包含完整后缀，运营商后缀留空。

## 工程结构

```text
main/app_main.c       启动流程与任务初始化
main/wifi_manager.c   AP/STA、DHCP、WiFi 事件
main/web_config.c     HTTP 配置页面与 SPIFFS 配置读写
main/config_codec.c   表单 URL 解码和 HTML 转义
main/srun_client.c    深澜协议、加密计算和自动认证
partitions.csv        ESP32-C3 4MB Flash 分区
```

## 诊断

串口速率为 `115200`。正常认证日志应包含：

```text
HTTP GET cgi-bin/get_challenge
HTTP 返回 status=200
深澜认证成功
```

`Specified method is invalid for this resource` 通常表示浏览器访问了旧的 `GET /save` 地址，返回首页即可；最新固件已兼容该地址。

深度睡眠日志包含 `进入深度睡眠，120秒后唤醒`，定时唤醒日志包含 `定时唤醒，仅开启 STA`。WiFi 发射功率仍设置为 8.5 dBm，唤醒期间使用最大 Modem-Sleep 省电模式。GPIO8 板载指示灯保持关闭，深睡期间保持其输出电平；其他板型可修改 `STATUS_LED_GPIO` 和电平宏。

## 免责声明

本项目的代码和文档完全由 AI 生成，未经专业人工审计或全面验证。使用者应自行检查代码、确认其符合所在网络的管理规定，并自行承担编译、烧录、运行及使用本项目产生的全部风险。作者不对设备损坏、网络中断、账号异常或其他直接、间接损失承担责任。

## 安全与许可

真实 WiFi 和校园网凭据只应通过网页配置，不要写入源码、提交配置文件或发布包含真实配置的固件。工程中的协议实现基于原有 ESurfingClient-CVersion，发布和再分发时请遵守上游 GPL 许可及 `licenses/` 中第三方组件的许可要求。
