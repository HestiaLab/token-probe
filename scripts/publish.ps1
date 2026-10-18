#!/usr/bin/env pwsh
<#
.SYNOPSIS
    通过 gh CLI 把打包好的 zip 发布到 GitHub Release。

.DESCRIPTION
    把 scripts/package.ps1 生成的 zip 及其 SHA256SUMS.txt 上传到 GitHub Release。
    Release 不存在时创建，已存在时上传到现有 Release。需要已登录的 gh CLI。

    默认试运行；传 -Yes 才真正创建或上传。

    用法：
      ./scripts/package.ps1
      ./scripts/publish.ps1 -Tag v0.0.1            # 试运行
      ./scripts/publish.ps1 -Tag v0.0.1 -Yes       # 发布
      ./scripts/publish.ps1 -Tag v0.0.1 -Yes -Draft -Prerelease
#>

[CmdletBinding()]
param(
    [string]$Tag,
    [string]$ZipPath,
    [string]$NotesFile,
    [switch]$Draft,
    [switch]$Prerelease,
    [switch]$Yes
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

    if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
        throw "gh CLI not found. Install it from https://cli.github.com/"
    }
    & gh auth status *> $null
    if ($LASTEXITCODE -ne 0) {
        throw "gh CLI is not authenticated. Run 'gh auth login' first."
    }

    # Tag：显式指定 > 当前提交上的 tag > 报错。
    if ([string]::IsNullOrWhiteSpace($Tag)) {
        $described = ((& git describe --tags --exact-match HEAD 2>$null) | Out-String).Trim()
        if ($LASTEXITCODE -eq 0 -and $described) {
            $Tag = $described
        }
    }
    if ([string]::IsNullOrWhiteSpace($Tag)) {
        throw "No tag on the current commit. Pass -Tag <tag> or tag the commit first."
    }

    # Zip：显式指定 > dist/ 下最新的 *.zip。
    if ([string]::IsNullOrWhiteSpace($ZipPath)) {
        $ZipPath = @(Get-ChildItem -LiteralPath (Join-Path $RepoRoot "dist") -Filter "*.zip" -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
    }
    if (-not (Test-Path -LiteralPath $ZipPath -PathType Leaf)) {
        throw "Release zip not found: $ZipPath. Run ./scripts/package.ps1 first."
    }
    $sumsPath = Join-Path (Split-Path -Parent $ZipPath) "SHA256SUMS.txt"
    if (-not (Test-Path -LiteralPath $sumsPath -PathType Leaf)) {
        throw "SHA256SUMS.txt not found next to the zip. Run ./scripts/package.ps1 first."
    }

    # 发布说明：显式文件 > 从 git log 生成。
    if ([string]::IsNullOrWhiteSpace($NotesFile)) {
        $NotesFile = Join-Path $env:TEMP "tokenprobe-release-notes.md"
        $prevTag = ((& git describe --tags --abbrev=0 HEAD~1 2>$null) | Out-String).Trim()
        if ($LASTEXITCODE -eq 0 -and $prevTag) {
            Invoke-Captured -Command "git" -Arguments @("log", "--pretty=format:- %s", "$prevTag..HEAD") |
                Set-Content -LiteralPath $NotesFile -Encoding utf8
        } else {
            Invoke-Captured -Command "git" -Arguments @("log", "--pretty=format:- %s") |
                Set-Content -LiteralPath $NotesFile -Encoding utf8
        }
    }

    & gh release view $Tag *> $null
    $exists = ($LASTEXITCODE -eq 0)
    $action = if ($exists) { "upload to existing release '$Tag'" } else { "create release '$Tag'" }

    Write-Host "Package:  $ZipPath"
    Write-Host "Checksum: $sumsPath"
    Write-Host "Notes:    $NotesFile"
    Write-Host "Action:   $action"
    Write-Host "Flags:    draft=$Draft prerelease=$Prerelease"

    if (-not $Yes) {
        Write-Host ""
        Write-Host "Dry run - no changes made. Pass -Yes to publish."
        return
    }

    if ($exists) {
        Write-Host "Uploading assets to existing release '$Tag'."
        Invoke-Captured -Command "gh" -Arguments @("release", "upload", $Tag, $ZipPath, $sumsPath, "--clobber") | Out-Null
    } else {
        Write-Host "Creating release '$Tag'."
        $createArgs = @("release", "create", $Tag, $ZipPath, $sumsPath, "--title", $Tag, "--notes-file", $NotesFile)
        if ($Draft) {
            $createArgs += "--draft"
        }
        if ($Prerelease) {
            $createArgs += "--prerelease"
        }
        Invoke-Captured -Command "gh" -Arguments $createArgs | Out-Null
    }

    Write-Host "Published $Tag ($ZipPath)."
} finally {
    Pop-Location
}
