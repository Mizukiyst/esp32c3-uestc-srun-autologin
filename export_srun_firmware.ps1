$ErrorActionPreference = "Stop"
$out = Join-Path $PSScriptRoot "release\ESP32-C3-SRUN"
New-Item -ItemType Directory -Force $out | Out-Null
Copy-Item "$PSScriptRoot\.pio\build\esp32c3\bootloader.bin" "$out\bootloader.bin" -Force
Copy-Item "$PSScriptRoot\.pio\build\esp32c3\partitions.bin" "$out\partition-table.bin" -Force
Copy-Item "$PSScriptRoot\.pio\build\esp32c3\firmware.bin" "$out\esp32c3_srun.bin" -Force
Copy-Item "$PSScriptRoot\.pio\build\esp32c3\spiffs.bin" "$out\spiffs.bin" -Force
Write-Host "已导出 $out"
