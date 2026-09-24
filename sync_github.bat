@echo off
title LAAP-esp32 同步到 GitHub
setlocal
cd /d "%~dp0"

REM 首次使用前置条件（本机已完成，无需再配）：
REM   1. GitHub 令牌已存入 Windows 凭据管理器（git credential approve, wincred）
REM   2. hosts 已钉 github.com -> 20.27.177.113（若失效改回 hosts 里这一行即可）

git remote get-url origin >nul 2>nul
if errorlevel 1 (
  echo [错误] 未配置远端 origin，请先执行：
echo   git remote add origin https://github.com/lijj1230/laap-esp32.git
  pause & exit /b 1
)

git add -A
REM 无变化则跳过提交
git diff --cached --quiet
if errorlevel 1 (
  git commit -m "sync: %date% %time%"
) else (
  echo 没有需要提交的改动。
)

echo 正在推送...
git push origin main
if errorlevel 1 (
  echo.
  echo [失败] 推送出错。常见原因：
  echo   1. 网络抖动：稍等重试，或检查 hosts 里 github.com 指向的 IP 是否还通
  echo      (测试: curl -m 8 https://github.com 应返回 200)
  echo   2. 令牌失效：到 https://github.com/settings/tokens 重新生成，
  echo      然后 运行 git credential reject 后重新 git push 按提示输入令牌
  pause & exit /b 1
)

echo.
echo [成功] 已同步到 https://github.com/lijj1230/laap-esp32
pause
