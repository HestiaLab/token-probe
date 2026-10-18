#!/usr/bin/env pwsh
<#
.SYNOPSIS
    把 Release 构建打包为可分发的 zip 压缩包。

.DESCRIPTION
    把选定的构建安装到暂存目录，用新构建的 tokenprobe 对已安装版本做冒烟检查，
    然后压缩完整安装树（bin/、include/、lib/、lib/cmake/）并生成 SHA-256 校验和。

    仅本地执行，不访问网络。发布到 GitHub 由 scripts/publish.ps1 负责。

    用法：
      cmake --preset ninja-release
      cmake --build --preset ninja-release
      ./scripts/package.ps1
#>

[CmdletBinding()]
param(
    [string]$Preset = "ninja-release"
)

$ErrorActionPreference = "Stop"
$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")

Push-Location $RepoRoot
try {

    function Invoke-Captured {
        param(
            [Parameter(Mandatory = $true)]
            [string]$Command,

            [Parameter(Mandatory = $true)]
            [string[]]$Arguments
        )

        # 在 PowerShell 5.1 的 $ErrorActionPreference="Stop" 下，写入 stderr 的外部命令
        # 可能触发 NativeCommandError。调用期间改为 Continue，用 $LASTEXITCODE 判断成败。
        $saved = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        try {
            $output = @(& $Command @Arguments 2>&1)
            $code = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $saved
        }
        if ($code -ne 0) {
            throw "$Command $($Arguments -join ' ') failed with exit code $code."
        }
        return $output
    }

    $cli = Join-Path "build" (Join-Path $Preset "bin\tokenprobe.exe")
    if (-not (Test-Path -LiteralPath $cli -PathType Leaf)) {
        throw "tokenprobe.exe not found: $cli. Build it first: cmake --preset $Preset; cmake --build --preset $Preset"
    }

    $version = ((Invoke-Captured -Command $cli -Arguments @("--version")) -join "`n").Trim()
    if ([string]::IsNullOrWhiteSpace($version)) {
        throw "Could not read a version from '$cli --version'."
    }
    $version = $version -replace '[^\w.\-]', '-'
    Write-Host "Packaging version: $version"

    $distDir = Join-Path $RepoRoot "dist"
    $stageDir = Join-Path $RepoRoot "build\$Preset\package-stage"
    if (Test-Path -LiteralPath $stageDir) {
        Remove-Item -LiteralPath $stageDir -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $distDir, $stageDir | Out-Null

    # 完整安装树：bin/、include/、lib/、lib/cmake/、share/。
    Invoke-Captured -Command "cmake" -Arguments @("--install", "build\$Preset", "--prefix", $stageDir) | Out-Null

    # 冒烟检查：已安装 CLI 的版本必须与构建版本一致。
    $installedCli = Join-Path $stageDir "bin\tokenprobe.exe"
    if (-not (Test-Path -LiteralPath $installedCli -PathType Leaf)) {
        throw "Installed CLI not found: $installedCli"
    }
    $installedVersion = ((Invoke-Captured -Command $installedCli -Arguments @("--version")) -join "`n").Trim()
    if ($installedVersion -ne $version) {
        throw "Smoke check failed: installed CLI reports '$installedVersion', expected '$version'."
    }
    Write-Host "Smoke check passed."

    $zipName = "tokenprobe-win64-$version.zip"
    $zipPath = Join-Path $distDir $zipName
    if (Test-Path -LiteralPath $zipPath) {
        Remove-Item -LiteralPath $zipPath -Force
    }
    Compress-Archive -Path (Join-Path $stageDir "*") -DestinationPath $zipPath -Force

    $sumsPath = Join-Path $distDir "SHA256SUMS.txt"
    $hash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
    "$hash  $zipName" | Set-Content -LiteralPath $sumsPath -Encoding ascii

    Write-Host "Packaged: $zipPath"
    Write-Host "SHA256:   $hash"
} finally {
    Pop-Location
}
