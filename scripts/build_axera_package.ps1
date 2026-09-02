# build_axera_package.ps1 — Windows/Docker Desktop 上的 AX650N 包构建。
#
# 对标 build_sophon_package.ps1：先把源码树同步进命名 ext4 卷
# （规避 9P/drvfs 构建劣化），恢复 Linux .so 符号链接，然后在本地构建
# AXERA builder 镜像，最后由 compose 运行 scripts/build_axera_package.sh --chip ax650n。
#
# builder 镜像完全自包含：ARM GNU gcc-arm-9.2 工具链与 AX650 SDK V3.10.2
# msp/out 运行时（仅 lib/include）在镜像构建时从官方源下载
# （见 Dockerfile.axera）——不需要本地 SDK bind mount。
#
# 前提：Docker Desktop（镜像构建期间需访问 developer.arm.com 与
#       modelscope.cn 的网络）。
# 输出：build_output/ax650n/cosmo-*.tar.gz

$ErrorActionPreference = "Stop"

$VolumeName     = "cosmo-axera-source"
$ComposeFile    = "docker-compose.axera.yml"
$OverrideFile   = "docker-compose.axera.override.yml"
$Chip           = if ($args.Count -gt 0) { $args[0] } else { "ax650n" }
if ($Chip -ne "ax650n") {
    throw "Unsupported AXERA chip '$Chip'; expected ax650n"
}

$projectRoot = (Get-Location).Path
$dockerSrc   = $projectRoot.Replace('\', '/').Replace('C:', '/c').Replace('E:', '/e').Replace('D:', '/d')

function Write-Step([string]$message) {
    Write-Host ""
    Write-Host "=== $message ===" -ForegroundColor Cyan
}

function Invoke-Docker {
    param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Args)
    $flat = ($Args | ForEach-Object { $_ }) -join " "
    cmd /c "docker $flat 2>&1"
    if ($LASTEXITCODE -ne 0) {
        throw "docker $flat failed (exit $LASTEXITCODE)"
    }
}

Write-Step "Step 1/5 - Checking Docker"
try { Invoke-Docker info | Out-Null } catch { Write-Error "Docker is not running. Start Docker Desktop, then re-run."; exit 1 }
Write-Host "Docker is ready"

Write-Step "Step 2/5 - Syncing source to Docker volume ($VolumeName)"
cmd /c "docker volume inspect $VolumeName >nul 2>nul"
if ($LASTEXITCODE -ne 0) {
    cmd /c "docker volume create $VolumeName >nul 2>nul"
    Write-Host "Created volume: $VolumeName"
}
Invoke-Docker run --rm `
    -v "${dockerSrc}:/src:ro" `
    -v "${VolumeName}:/workspace" `
    alpine sh /src/scripts/sync-source-volume.sh
Write-Host "Source sync complete"

Write-Step "Step 3/5 - Restoring Linux .so symlinks"
Invoke-Docker run --rm `
    -v "${VolumeName}:/workspace" `
    alpine sh /workspace/scripts/restore-symlinks.sh

Write-Step "Step 4/5 - Building AXERA builder image + cross-compiling $Chip"
# 生成一个 compose override：把 bind mount 换成命名卷，保留 ./build_output
# 作为 bind mount，并保留 ext4 构建卷（CPack 暂存发生在那里，
# 不落在 Windows 盘上）。
@"
# 由 build_axera_package.ps1 自动生成 - 请勿提交。
services:
  cosmo-axera-package:
    volumes:
      - ${VolumeName}:/workspace
      - ./build_output:/build_output
      - cosmo-axera-build:/opt/axera/build
volumes:
  cosmo-axera-build:
  ${VolumeName}:
    external: true
"@ | Out-File -FilePath (Join-Path $projectRoot $OverrideFile) -Encoding utf8

Push-Location $projectRoot
try {
    cmd /c "docker compose -f $ComposeFile build 2>&1"
    if ($LASTEXITCODE -ne 0) {
        Write-Error "AXERA builder image build failed with exit code $LASTEXITCODE."
        exit $LASTEXITCODE
    }
    cmd /c "docker compose -f $ComposeFile -f $OverrideFile run --rm cosmo-axera-package --chip $Chip 2>&1"
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Docker build failed with exit code $LASTEXITCODE."
        exit $LASTEXITCODE
    }
} finally {
    Pop-Location
    Remove-Item (Join-Path $projectRoot $OverrideFile) -Force -ErrorAction SilentlyContinue
}

Write-Step "Step 5/5 - Build output"
$outputDir = Join-Path $projectRoot "build_output\$Chip"
if (Test-Path $outputDir) {
    $packages = Get-ChildItem $outputDir -Filter "*.tar.gz"
    if ($packages) {
        foreach ($pkg in $packages) {
            Write-Host "  $($pkg.Name)  ($('{0:N0}' -f $pkg.Length) bytes)" -ForegroundColor Green
        }
    } else {
        Write-Warning "No .tar.gz found in build_output/$Chip/"
    }
} else {
    Write-Warning "build_output/$Chip/ directory not found"
}

Write-Host ""
Write-Host "=== AX650N build completed ===" -ForegroundColor Green
