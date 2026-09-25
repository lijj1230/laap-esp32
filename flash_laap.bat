@echo off
title LAAP-lite 烧录 (实战派 ESP32-S3)
setlocal
cd /d "%~dp0"

echo ============================================
echo   LAAP-lite 固件烧录 - 立创实战派 ESP32-S3
echo ============================================
echo.

where python >nul 2>nul
if errorlevel 1 (
  echo [错误] 未找到 python，请先安装 Python 3 并勾选 Add to PATH
  pause
  exit /b 1
)

python -m esptool version >nul 2>nul
if errorlevel 1 (
  echo [提示] 首次使用，正在安装 esptool（清华镜像，很快）...
  python -m pip install esptool -i https://pypi.tuna.tsinghua.edu.cn/simple -q
  if errorlevel 1 (
    echo [错误] esptool 安装失败，请检查网络后重试
    pause
    exit /b 1
  )
)

set "PORT=%~1"
if "%PORT%"=="" (
  echo 可用串口：
  python -m esptool ports 2>nul
  set /p PORT=请输入开发板 COM 口（如 COM5）后回车（板子先插好）:
)
if "%PORT%"=="" (
  echo [错误] 没有输入 COM 口，退出。插好板子后重新双击本文件
  pause
  exit /b 1
)

echo.
echo 即将烧录 4 个分区到 %PORT% ...
echo 若一直等待 Connecting...：按住板上 BOOT 键，点按一下 RST，松开 RST，2 秒后再松开 BOOT
echo.
python -m esptool --chip esp32s3 --port "%PORT%" --baud 921600 --before default-reset --after hard-reset write-flash -z 0x0 firmware\laap-esp32.ino.bootloader.bin 0x8000 firmware\laap-esp32.ino.partitions.bin 0xe000 firmware\boot_app0.bin 0x10000 firmware\laap-esp32.ino.bin
if errorlevel 1 (
  echo.
  echo [失败] 分区烧录出错，改用整片镜像方式再试...
  echo [注意] 整片烧录会覆盖整颗 16MB，包含记忆区（LittleFS）——人格与记忆会清空。
  echo         刷完可在网页"记忆"页用"导入记忆"恢复此前的备份。
  python -m esptool --chip esp32s3 --port "%PORT%" --baud 921600 write-flash 0x0 firmware\laap-lite-merged-16MB.bin
  if errorlevel 1 (
    echo [失败] 烧录未完成，请把上面窗口里的报错文字截图/复制发回来
    pause
    exit /b 1
  )
) else (
  echo.
  echo [成功] 烧录完成，设备已重启！
  echo   首次启动会创建热点 Aris-XXXX（密码 12345678），
  echo   手机连接后打开 http://192.168.4.1 完成 WiFi 和大模型 API 配置。
)
pause
