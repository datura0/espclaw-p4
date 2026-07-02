# ESP-IDF v5.5.4 环境激活脚本 (Waveshare ESP32-P4-WIFI6)
# 用法: . .\activate_idf.ps1   (注意前面的点和空格)

$env:IDF_PATH = "D:\.espressif\.espressif\v5.5.4\esp-idf"
$env:IDF_PYTHON_ENV_PATH = "C:\Users\Datura\.espressif\python_env\idf5.5_py3.13_env"
$env:IDF_TOOLS_PATH = "C:\Espressif\tools"
$env:ESP_ROM_ELF_DIR = "$env:IDF_TOOLS_PATH\esp-rom-elfs\20241011"

# 工具链路径
$env:PATH = @(
    "$env:IDF_PYTHON_ENV_PATH\Scripts",
    "$env:IDF_PATH\tools",
    "$env:IDF_TOOLS_PATH\cmake\3.30.2\bin",
    "$env:IDF_TOOLS_PATH\ninja\1.12.1",
    "$env:IDF_TOOLS_PATH\ccache",
    "$env:IDF_TOOLS_PATH\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin",
    "$env:IDF_TOOLS_PATH\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin",
    $env:PATH
) -join [IO.Path]::PathSeparator

# 使用短构建路径，避免 Windows MAX_PATH (260字符) 限制
$env:EDGE_BUILD_DIR = "D:\edge_build"

Write-Host "ESP-IDF v5.5.4 activated" -ForegroundColor Green
Write-Host "Target: esp32p4 | Build dir: $env:EDGE_BUILD_DIR" -ForegroundColor Cyan
Write-Host ""
Write-Host "Usage:" -ForegroundColor Yellow
Write-Host "  idf.py -B $env:EDGE_BUILD_DIR build" -ForegroundColor White
Write-Host "  idf.py -B $env:EDGE_BUILD_DIR -p COMxx flash monitor" -ForegroundColor White
