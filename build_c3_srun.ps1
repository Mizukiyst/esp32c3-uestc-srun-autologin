$ErrorActionPreference = "Stop"

if (-not $env:IDF_PATH) {
    Write-Error "请先执行 ESP-IDF export.ps1，例如: . C:\Espressif\frameworks\esp-idf-v5.5\export.ps1"
}

idf.py -C $PSScriptRoot set-target esp32c3
idf.py -C $PSScriptRoot build

Write-Host "构建完成。Flash Download Tool 地址："
Write-Host "  0x0000   build\bootloader\bootloader.bin"
Write-Host "  0x8000   build\partition_table\partition-table.bin"
Write-Host "  0x10000  build\esp32_esurfing.bin"
Write-Host "  0x2D0000 build\spiffs.bin"
