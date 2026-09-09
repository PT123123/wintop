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

# 编译 Release 并部署到本地工作台目录
# 注：该 shell 下每条 recipe 行是独立进程，须把逻辑写在同一行；用分号衔接
deploy-workshop: build
    New-Item -ItemType Directory -Force -Path 'C:/workshop/WinTopPreview' | Out-Null; Copy-Item -Force '{{exe}}' -Destination 'C:/workshop/WinTopPreview'; Write-Host "已部署到 C:\workshop\WinTopPreview"