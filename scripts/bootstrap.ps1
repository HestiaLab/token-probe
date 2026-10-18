[CmdletBinding()]
param(
    [string]$Preset = "ninja-debug",
    [switch]$CheckOnly,
    [switch]$CheckEditor
)

$ErrorActionPreference = "Stop"
$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$failed = $false

Push-Location $RepoRoot
try {

function Test-Tool {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name,

        [Parameter(Mandatory = $true)]
        [string]$InstallHint,

        [switch]$Required
    )

    if (Get-Command $Name -ErrorAction SilentlyContinue) {
        Write-Host "OK: found $Name"
        return $true
    }

    if ($Required) {
        Write-Host "ERROR: missing $Name. $InstallHint"
        $script:failed = $true
    } else {
        Write-Host "Optional: missing $Name. $InstallHint"
    }
    return $false
}

function Test-Compiler {
    if ($env:CC) {
        Write-Host "OK: using compiler from CC=$env:CC"
        $cmd = Get-Command $env:CC -ErrorAction SilentlyContinue
        if ($cmd) { $script:CCPath = $cmd.Source }
        return
    }

    foreach ($compiler in @("cl", "cc", "gcc", "clang")) {
        $cmd = Get-Command $compiler -ErrorAction SilentlyContinue
        if ($cmd) {
            Write-Host "OK: found C compiler command $compiler"
            $script:CCPath = $cmd.Source
            return
        }
    }

    if ($env:OS -eq "Windows_NT") {
        Write-Host "ERROR: no C compiler command found. Open Developer PowerShell, Developer Command Prompt, or select an MSVC CMake kit in VS Code."
    } else {
        Write-Host "ERROR: no C compiler command found. Install GCC or Clang (including AppleClang via Xcode Command Line Tools), or set CC to your C compiler."
    }
    $script:failed = $true
}

function Test-CMakeVersion {
    $versionLine = (& cmake --version | Select-Object -First 1)
    if ($versionLine -match "version\s+([0-9]+)\.([0-9]+)\.([0-9]+)") {
        $version = [version]"$($Matches[1]).$($Matches[2]).$($Matches[3])"
        if ($version -ge [version]"3.21.0") {
            Write-Host "OK: CMake version $version"
            return
        }

        Write-Host "ERROR: CMake $version is too old. Install CMake 3.21 or newer."
        $script:failed = $true
        return
    }

    Write-Host "ERROR: could not determine CMake version."
    $script:failed = $true
}

if (Test-Tool -Name "cmake" -InstallHint "Install CMake 3.21 or newer and add it to PATH." -Required) {
    Test-CMakeVersion
}

if ($Preset -like "ninja-*") {
    Test-Tool -Name "ninja" -InstallHint "Install Ninja or choose a generator-neutral preset through CMakeUserPresets.json." -Required | Out-Null
}

Test-Compiler

# 确保编译器目录在 PATH 中，使 CMake 子进程能找到运行时 DLL
# （适用于 MinGW、MSYS2 和独立安装的 LLVM）。
if ($script:CCPath) {
    $compilerDir = Split-Path $script:CCPath -Parent
    $env:PATH = "$compilerDir;$env:PATH"
    Write-Host "OK: added $compilerDir to PATH for compiler runtime"
}

Test-Tool -Name "clangd" -InstallHint "Install clangd for editor navigation." | Out-Null
Test-Tool -Name "clang-format" -InstallHint "Install clang-format for local format checks." | Out-Null
Test-Tool -Name "clang-tidy" -InstallHint "Install clang-tidy for optional static analysis." | Out-Null

if ($CheckEditor) {
    Test-Tool -Name "gdb" -InstallHint "Install gdb for debugging (MSYS2: pacman -S mingw-w64-ucrt-x86_64-gdb)." -Required | Out-Null
}

if ($failed) {
    exit 1
}

if (-not $CheckOnly) {
    & cmake --preset $Preset
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    $compileCommands = Join-Path (Join-Path "build" $Preset) "compile_commands.json"
    if (Test-Path $compileCommands) {
        Write-Host "OK: generated $compileCommands"
    } else {
        Write-Host "Note: $compileCommands was not generated. Use a generator that supports compilation databases for clangd."
    }
}
} finally {
    Pop-Location
}
