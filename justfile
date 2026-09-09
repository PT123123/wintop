# WinTop Preview 构建脚本
set shell := ["powershell", "-NoProfile", "-NonInteractive", "-Command"]

build_dir := "build"
config    := "Release"
exe       := build_dir + "/bin/" + config + "/WinTopPreview.exe"

# 配置并编译 Release
build:
    cmake -S . -B {{build_dir}}
    cmake --build {{build_dir}} --config {{config}}

# 清空所有构建产物
clean:
    if (Test-Path '{{build_dir}}') { Remove-Item '{{build_dir}}' -Recurse -Force }

# 编译并启动程序
run: build
    Start-Process '{{exe}}'