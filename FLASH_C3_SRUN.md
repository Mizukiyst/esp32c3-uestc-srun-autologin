# ESP32-C3 深澜版烧录

这是基于本工程 Wi-Fi/AP 配置部分改写的深澜（Srun）客户端。默认服务器为 `10.253.0.235`，默认 `AC ID=3`，与认证页 `http://10.253.0.235/srun_portal_pc?ac_id=3&theme=pro` 对应。

## 编译

安装 ESP-IDF 后，在 PowerShell 执行：

```powershell
. C:\Espressif\frameworks\esp-idf-v5.5\export.ps1
.\build_c3_srun.ps1
```

本机现有的 `build.ps1` 中路径是另一台电脑的旧路径，不能直接使用；只要 `idf.py` 已在 PATH 中，`build_c3_srun.ps1` 不依赖固定安装目录。

如果使用 PlatformIO，可执行 `..\\.tools\\venv\\Scripts\\pio.exe run`；编译后执行 `./export_srun_firmware.ps1` 整理出可给 Flash Download Tool 使用的文件。

## Flash Download Tool

选择 `ESP32-C3`、`UART`、`DIO`、`4MB`，勾选四个文件：

| 地址 | 文件 |
|---|---|
| `0x0000` | `build/bootloader/bootloader.bin` |
| `0x8000` | `build/partition_table/partition-table.bin` |
| `0x10000` | `release/ESP32-C3-SRUN/esp32c3_srun.bin` |
| `0x2D0000` | `release/ESP32-C3-SRUN/spiffs.bin` |

烧录后连接 `ESurfing-Config`（密码 `esurfing2024`），打开 `http://192.168.4.1`。填写 2.4 GHz Wi-Fi、深澜账号密码；服务器填 `10.253.0.235`，AC ID 填 `3`。运营商后缀按学校要求填写，没有则留空。认证 IP 通常留空，程序会自动使用 ESP32 从路由器获得的 IP。

程序每 60 秒查询 `cgi-bin/rad_user_info`；掉线后自动获取 challenge 并重新登录。密码只写入 SPIFFS 配置文件，不会写入源码。
