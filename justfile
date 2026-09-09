# WinTop Preview 构建脚本
set shell := ["powershell", "-NoProfile", "-NonInteractive", "-Command"]

build_dir := "build"
config    := "Release"
exe       := build_dir + "/bin/" + config + "/WinTopPreview.exe"

project_name := "WinTopPreview"
version_file := "version.txt"

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

# 编译 Release 并部署到本地工作台目录，并在项目名后追加版本号
# 版本存于 version.txt，部署一次 patch 号 +1；目录名形如 WinTopPreview-x.y.z
# （该 shell 下每条 recipe 行是独立进程，逻辑须写在同一行用分号衔接）
deploy-workshop: build
    $v = '0.0.0'; if (Test-Path '{{version_file}}') { $v = (Get-Content '{{version_file}}' -Raw).Trim() }; $seg = $v -split '\.'; $patch = [int]$seg[2] + 1; $ver = $seg[0] + '.' + $seg[1] + '.' + $patch; Set-Content -Path '{{version_file}}' -Value $ver; $dest = 'C:/workshop/{{project_name}}-' + $ver; New-Item -ItemType Directory -Force -Path $dest | Out-Null; Copy-Item -Force '{{exe}}' -Destination $dest; Write-Host "已部署到 $dest"