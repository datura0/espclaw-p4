@echo off
cd /d d:\esp_test\esp-claw\application\edge_agent

set IDF_PATH=D:\.espressif\.espressif\v5.5.4\esp-idf
set IDF_PYTHON_ENV_PATH=C:\Users\Datura\.espressif\python_env\idf5.5_py3.13_env
set IDF_TOOLS_PATH=C:\Espressif\tools
set ESP_ROM_ELF_DIR=C:\Espressif\tools\esp-rom-elfs\20241011
set IDF_COMPONENT_SUPPRESS_UNKNOWN_FILE_WARNINGS=1

set PATH=%IDF_PYTHON_ENV_PATH%\Scripts;%IDF_PATH%\tools;%IDF_TOOLS_PATH%\cmake\3.30.2\bin;%IDF_TOOLS_PATH%\ninja\1.12.1;%IDF_TOOLS_PATH%\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin;%PATH%

echo Cleaning...
if exist D:\bld rmdir /s /q D:\bld
if exist sdkconfig del /f sdkconfig

echo Building...
python "%IDF_PATH%\tools\idf.py" -B D:\bld build
