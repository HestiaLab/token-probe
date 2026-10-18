[CmdletBinding()]
param(
    [string]$Preset = "ninja-debug",
    [switch]$SkipFormat,
    [switch]$FixFormat,
    [switch]$EnableTidy
)

$ErrorActionPreference = "Stop"

# PowerShell 会把误写的 bash 风格 `--flag` 绑定到第一个位置参数（这里是 $Preset），
# 随后以难以理解的 CMake 错误失败。这里提前失败并给出可操作的提示。
if ($Preset -like "-*") {
    [Console]::Error.WriteLine("Unknown option '$Preset'. PowerShell switches use a single dash: -EnableTidy, -SkipFormat, -FixFormat, -Preset <name>.")
    exit 2
}

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")

Push-Location $RepoRoot
try {

function Invoke-CheckedCommand {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Command,

        [Parameter(Mandatory = $true)]
        [string[]]$Arguments
    )

    # 在 PowerShell 5.1 的 $ErrorActionPreference="Stop" 下，写入 stderr 的外部命令
    # 会触发 NativeCommandError 并终止脚本，即使它只是返回了非零退出码。
    # 调用期间改为 Continue，改用 $LASTEXITCODE 判断成败。
    $savedErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        & $Command @Arguments
    } finally {
        $ErrorActionPreference = $savedErrorActionPreference
    }
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}

if (-not $SkipFormat -and (Get-Command clang-format -ErrorAction SilentlyContinue)) {
    $files = Get-ChildItem -Path include,src,tests -Recurse -File -Include *.c,*.h,*.cpp |
        Where-Object { $_.FullName -notlike "*\tests\vendor\*" } |
        Sort-Object FullName
    if ($files.Count -gt 0) {
        # 用 `+=` 逐个追加文件，不要用内联数组字面量。把
        # `@("-i", $files.FullName)` 绑定到 [string[]] 参数会把路径数组压平成
        # 一个空格拼接的字符串，clang-format 会以 "Invalid argument" 拒绝。
        # `+=` 保证每个路径是独立参数。
        if ($FixFormat) {
            $formatArgs = @("-i")
            $formatArgs += $files.FullName
            Invoke-CheckedCommand -Command "clang-format" -Arguments $formatArgs
        } else {
            $formatArgs = @("--dry-run", "--Werror")
            $formatArgs += $files.FullName
            Invoke-CheckedCommand -Command "clang-format" -Arguments $formatArgs
        }
    }
} elseif (-not $SkipFormat) {
    Write-Host "Skipping clang-format: command not found."
}

Invoke-CheckedCommand -Command "cmake" -Arguments @("--preset", $Preset)
Invoke-CheckedCommand -Command "cmake" -Arguments @("--build", "--preset", $Preset)

if ($EnableTidy) {
    if (Get-Command clang-tidy -ErrorAction SilentlyContinue) {
        $buildDirectory = Join-Path "build" $Preset
        $compileCommands = Join-Path $buildDirectory "compile_commands.json"
        $generatedIncludeDirectory = Join-Path $buildDirectory "generated\include"
        $files = Get-ChildItem -Path src,tests -Recurse -File -Include *.c |
            Where-Object { $_.FullName -notlike "*\tests\package_smoke\*" } |
            Where-Object { $_.FullName -notlike "*\tests\subproject_smoke\*" } |
            Where-Object { $_.FullName -notlike "*\tests\vendor\*" } |
            Sort-Object FullName
        foreach ($file in $files) {
            if (Test-Path $compileCommands) {
                Invoke-CheckedCommand -Command "clang-tidy" -Arguments @(
                    $file.FullName,
                    "--warnings-as-errors=*",
                    "-quiet",
                    "-p",
                    $buildDirectory
                )
            } else {
                Invoke-CheckedCommand -Command "clang-tidy" -Arguments @(
                    $file.FullName,
                    "--warnings-as-errors=*",
                    "-quiet",
                    "--extra-arg=-std=c11",
                    "--",
                    "-Iinclude",
                    "-I$generatedIncludeDirectory"
                )
            }
        }
    } else {
        Write-Host "Skipping clang-tidy: command not found."
    }
}

Invoke-CheckedCommand -Command "ctest" -Arguments @("--preset", $Preset, "--output-on-failure")
} finally {
    Pop-Location
}
