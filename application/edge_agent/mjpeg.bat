@echo off
set IP=192.168.137.81
echo ==============================
echo  按 Enter 打开视频流，Ctrl+C 退出
echo  IP: %IP%
echo ==============================

:loop
set /p dummy=">>> "
ffplay -fflags nobuffer -flags low_delay -window_title "ESP32-P4 MJPEG" http://%IP%/mjpeg
goto loop
