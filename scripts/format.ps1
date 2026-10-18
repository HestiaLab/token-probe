#!/usr/bin/env pwsh

<#
.SYNOPSIS
    检查或格式化项目中的 C 源文件和头文件。

.DESCRIPTION
    递归处理指定目录下所有 *.c 和 *.h 文件。

    -Check
        检查文件是否符合 clang-format 格式。
        不修改任何文件。
        如果发现需要格式化的文件，返回退出码 2。

    -Format
        使用 clang-format 格式化文件。
        会直接修改文件。

    -Path
        支持：
        1. 相对于当前 format.ps1 所在目录的路径
        2. 绝对路径

.PARAMETER Path
    要检查或格式化的目录。

.PARAMETER Check
    检查模式，只检查，不修改文件。

.PARAMETER Format
    格式化模式，实际修改文件。

.EXAMPLE
    .\format.ps1 -Path src -Check

    检查 format.ps1 所在目录下的 src 目录。

.EXAMPLE
    .\format.ps1 -Path src -Format

    格式化 format.ps1 所在目录下的 src 目录。

.EXAMPLE
    .\format.ps1 -Path ..\src -Check

    检查 format.ps1 上一级目录中的 src 目录。

.EXAMPLE
    .\format.ps1 -Path D:\Project\src -Check

    检查指定的绝对路径。

.EXAMPLE
    .\format.ps1 -Path D:\Project\src -Format

    格式化指定的绝对路径。

.NOTES
    需要预先安装 clang-format，并确保其位于 PATH 环境变量中。

    文件颜色：
      *.c -> 绿色
      *.h -> 黄色
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$Path,

    [Parameter(Mandatory = $false)]
    [switch]$Check,

    [Parameter(Mandatory = $false)]
    [switch]$Format
)

# ============================================================
# 参数检查
# ============================================================

if ($Check -and $Format) {
    Write-Error "-Check 和 -Format 不能同时使用。"
    exit 1
}

if (-not $Check -and -not $Format) {
    Write-Error "必须指定操作模式：-Check 或 -Format。"
    Write-Host ""
    Write-Host "示例：" -ForegroundColor Cyan
    Write-Host "  .\format.ps1 -Path src -Check"
    Write-Host "  .\format.ps1 -Path src -Format"
    exit 1
}

# ============================================================
# 路径处理
# ============================================================

# $PSScriptRoot 表示当前 format.ps1 所在目录
$scriptRoot = $PSScriptRoot

# 绝对路径直接使用
if ([System.IO.Path]::IsPathRooted($Path)) {
    $targetPath = [System.IO.Path]::GetFullPath($Path)
}
else {
    # 相对路径始终相对于 format.ps1 所在目录
    $targetPath = [System.IO.Path]::GetFullPath(
        [System.IO.Path]::Combine($scriptRoot, $Path)
    )
}

# ============================================================
# 检查目标目录
# ============================================================

if (-not (Test-Path -LiteralPath $targetPath -PathType Container)) {
    Write-Error "目录不存在：$targetPath"
    exit 1
}

# ============================================================
# 检查 clang-format
# ============================================================

$clangFormat = "clang-format"

try {
    $clangVersion = & $clangFormat --version 2>$null

    if ($LASTEXITCODE -ne 0) {
        throw
    }
}
catch {
    Write-Error "clang-format 未安装或不在 PATH 中。"
    Write-Host "请安装 LLVM 或将 clang-format 所在目录加入 PATH。" `
        -ForegroundColor Yellow
    exit 1
}

# ============================================================
# 查找所有 .c 和 .h 文件
# ============================================================

$files = @(
    Get-ChildItem `
        -LiteralPath $targetPath `
        -Recurse `
        -Include "*.c", "*.h" `
        -File `
        -ErrorAction SilentlyContinue
)

# 排除 vendored 第三方代码（如 tests/vendor）与构建产物（如 build/），
# 二者都不应参与格式检查或格式化。
$files = @($files | Where-Object {
    $_.FullName -notmatch '[\\/](vendor|build)[\\/]'
})

if ($files.Count -eq 0) {
    Write-Warning "未找到任何 *.c 或 *.h 文件。"
    Write-Host "扫描目录：$targetPath" -ForegroundColor Gray
    exit 0
}

# ============================================================
# 工具函数：获取相对路径
# ============================================================

function Get-RelativePath {
    param(
        [string]$BasePath,
        [string]$FullPath
    )

    $base = $BasePath.TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar
    )

    return $FullPath.Substring($base.Length).TrimStart(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar
    )
}

# ============================================================
# 工具函数：格式化文件大小
# ============================================================

function Format-FileSize {
    param(
        [long]$Bytes
    )

    if ($Bytes -lt 1KB) {
        return "{0} B" -f $Bytes
    }

    if ($Bytes -lt 1MB) {
        return "{0:N2} KB" -f ($Bytes / 1KB)
    }

    return "{0:N2} MB" -f ($Bytes / 1MB)
}

# ============================================================
# 工具函数：获取文件颜色
#
# .c -> 绿色
# .h -> 黄色
# ============================================================

function Get-FileColor {
    param(
        [string]$FilePath
    )

    switch ([System.IO.Path]::GetExtension($FilePath).ToLowerInvariant()) {
        ".c" {
            return "Green"
        }

        ".h" {
            return "Yellow"
        }

        default {
            return "Gray"
        }
    }
}

# ============================================================
# CHECK 模式
# ============================================================

if ($Check) {

    try { Clear-Host } catch { }

    Write-Host ""
    Write-Host "============================================================" `
        -ForegroundColor Cyan

    Write-Host "             clang-format 格式检查" `
        -ForegroundColor Cyan

    Write-Host "============================================================" `
        -ForegroundColor Cyan

    Write-Host ""

    Write-Host "目录        : " -NoNewline
    Write-Host $targetPath -ForegroundColor Green

    Write-Host "文件数量    : " -NoNewline
    Write-Host $files.Count -ForegroundColor Yellow

    Write-Host "clang-format : " -NoNewline
    Write-Host $clangVersion -ForegroundColor Gray

    Write-Host ""

    Write-Host "文件颜色：" -ForegroundColor Cyan
    Write-Host "  *.c       " -NoNewline -ForegroundColor Green
    Write-Host "C 源文件"
    Write-Host "  *.h       " -NoNewline -ForegroundColor Yellow
    Write-Host "头文件"

    Write-Host ""
    Write-Host "正在检查文件..." -ForegroundColor Cyan
    Write-Host ""

    # --------------------------------------------------------
    # 检查统计
    # --------------------------------------------------------

    $needFormat = @()

    $checkedCount = 0
    $errorCount = 0

    # --------------------------------------------------------
    # 逐个检查文件
    # --------------------------------------------------------

    foreach ($file in $files) {

        $checkedCount++

        $relativePath = Get-RelativePath `
            -BasePath $targetPath `
            -FullPath $file.FullName

        $percent = [int](
            ($checkedCount / $files.Count) * 100
        )

        Write-Progress `
            -Activity "检查 clang-format 格式" `
            -Status "[$checkedCount/$($files.Count)] $relativePath" `
            -PercentComplete $percent

        try {

            # ------------------------------------------------
            # 使用 --dry-run --Werror 判定：
            #   已符合格式 -> 退出码 0
            #   需要格式化 -> 退出码非 0
            #
            # 不捕获 stdout 做文本比较：PowerShell 会把 UTF-8
            # 多字节字符按控制台代码页重解码而损坏，且 CRLF/LF
            # 与尾随换行也会造成误判。改用 clang-format 自身的
            # 退出码判定，既准确又简单。
            # ------------------------------------------------

            & $clangFormat --dry-run --Werror $file.FullName *> $null

            if ($LASTEXITCODE -eq 0) {

                $fileColor = Get-FileColor $file.FullName

                Write-Host "  ✓ 已符合格式  " `
                    -NoNewline `
                    -ForegroundColor Green

                Write-Host $relativePath `
                    -ForegroundColor $fileColor
            }
            else {

                $needFormat += [PSCustomObject]@{
                    File     = $relativePath
                    FullPath = $file.FullName
                    Size     = Format-FileSize $file.Length
                }

                $fileColor = Get-FileColor $file.FullName

                Write-Host "  ✗ 需要格式化  " `
                    -NoNewline `
                    -ForegroundColor Red

                Write-Host $relativePath `
                    -ForegroundColor $fileColor
            }
        }
        catch {

            $errorCount++

            Write-Host "  ! 检查失败     " `
                -NoNewline `
                -ForegroundColor Red

            Write-Host $relativePath `
                -ForegroundColor Red

            Write-Host "      $($_.Exception.Message)" `
                -ForegroundColor DarkRed
        }
    }

    Write-Progress `
        -Activity "检查 clang-format 格式" `
        -Completed

    # ========================================================
    # CHECK 结果
    # ========================================================

    Write-Host ""
    Write-Host "============================================================" `
        -ForegroundColor Cyan

    Write-Host "                    检查结果" `
        -ForegroundColor Cyan

    Write-Host "============================================================" `
        -ForegroundColor Cyan

    Write-Host ""

    $okCount = $files.Count - $needFormat.Count - $errorCount

    Write-Host "总文件数       : " -NoNewline
    Write-Host $files.Count

    Write-Host "符合格式       : " -NoNewline
    Write-Host $okCount -ForegroundColor Green

    Write-Host "需要格式化     : " -NoNewline

    if ($needFormat.Count -gt 0) {
        Write-Host $needFormat.Count -ForegroundColor Red
    }
    else {
        Write-Host "0" -ForegroundColor Green
    }

    Write-Host "检查失败       : " -NoNewline

    if ($errorCount -gt 0) {
        Write-Host $errorCount -ForegroundColor Red
    }
    else {
        Write-Host "0" -ForegroundColor Green
    }

    Write-Host ""

    # ========================================================
    # 显示需要格式化的文件
    # ========================================================

    if ($needFormat.Count -gt 0) {

        Write-Host "需要格式化的文件：" `
            -ForegroundColor Yellow

        Write-Host ""

        $index = 1

        foreach ($item in $needFormat) {

            $fileColor = Get-FileColor $item.FullPath

            Write-Host ("  {0,3}. " -f $index) `
                -NoNewline `
                -ForegroundColor DarkGray

            Write-Host $item.File `
                -ForegroundColor $fileColor

            $index++
        }

        Write-Host ""

        Write-Host "提示：" -ForegroundColor Cyan

        Write-Host "  执行以下命令进行格式化：" `
            -ForegroundColor Gray

        Write-Host ""

        Write-Host "    .\format.ps1 -Path `"$Path`" -Format" `
            -ForegroundColor Green

        Write-Host ""

        # 检查发现需要格式化的文件
        # 使用退出码 2，方便 CI/CD 判断
        exit 2
    }

    if ($errorCount -gt 0) {
        exit 1
    }

    Write-Host "✓ 所有文件均符合 clang-format 格式。" `
        -ForegroundColor Green

    Write-Host ""

    exit 0
}

# ============================================================
# FORMAT 模式
# ============================================================

if ($Format) {

    try { Clear-Host } catch { }

    Write-Host ""
    Write-Host "============================================================" `
        -ForegroundColor Cyan

    Write-Host "             clang-format 文件格式化" `
        -ForegroundColor Cyan

    Write-Host "============================================================" `
        -ForegroundColor Cyan

    Write-Host ""

    Write-Host "目录        : " -NoNewline
    Write-Host $targetPath -ForegroundColor Green

    Write-Host "文件数量    : " -NoNewline
    Write-Host $files.Count -ForegroundColor Yellow

    Write-Host "clang-format : " -NoNewline
    Write-Host $clangVersion -ForegroundColor Gray

    Write-Host ""

    Write-Host "文件颜色：" -ForegroundColor Cyan
    Write-Host "  *.c       " -NoNewline -ForegroundColor Green
    Write-Host "C 源文件"
    Write-Host "  *.h       " -NoNewline -ForegroundColor Yellow
    Write-Host "头文件"

    Write-Host ""
    Write-Host "开始格式化..." -ForegroundColor Cyan
    Write-Host ""

    # --------------------------------------------------------
    # 格式化统计
    # --------------------------------------------------------

    $successCount = 0
    $errorCount = 0
    $index = 0

    # --------------------------------------------------------
    # 逐个格式化文件
    # --------------------------------------------------------

    foreach ($file in $files) {

        $index++

        $relativePath = Get-RelativePath `
            -BasePath $targetPath `
            -FullPath $file.FullName

        $percent = [int](
            ($index / $files.Count) * 100
        )

        Write-Progress `
            -Activity "正在格式化文件" `
            -Status "[$index/$($files.Count)] $relativePath" `
            -PercentComplete $percent

        try {

            # ------------------------------------------------
            # 执行 clang-format
            # ------------------------------------------------

            & $clangFormat -i $file.FullName 2>&1

            if ($LASTEXITCODE -eq 0) {

                $successCount++

                $fileColor = Get-FileColor $file.FullName

                Write-Host "  ✓ " `
                    -NoNewline `
                    -ForegroundColor Green

                Write-Host $relativePath `
                    -ForegroundColor $fileColor
            }
            else {

                $errorCount++

                $fileColor = Get-FileColor $file.FullName

                Write-Host "  ✗ " `
                    -NoNewline `
                    -ForegroundColor Red

                Write-Host $relativePath `
                    -ForegroundColor $fileColor

                Write-Host "      clang-format 退出码：$LASTEXITCODE" `
                    -ForegroundColor DarkRed
            }
        }
        catch {

            $errorCount++

            $fileColor = Get-FileColor $file.FullName

            Write-Host "  ✗ " `
                -NoNewline `
                -ForegroundColor Red

            Write-Host $relativePath `
                -ForegroundColor $fileColor

            Write-Host "      $($_.Exception.Message)" `
                -ForegroundColor DarkRed
        }
    }

    Write-Progress `
        -Activity "正在格式化文件" `
        -Completed

    # ========================================================
    # FORMAT 结果
    # ========================================================

    Write-Host ""
    Write-Host "============================================================" `
        -ForegroundColor Cyan

    Write-Host "                   格式化完成" `
        -ForegroundColor Cyan

    Write-Host "============================================================" `
        -ForegroundColor Cyan

    Write-Host ""

    Write-Host "总文件数       : " -NoNewline
    Write-Host $files.Count

    Write-Host "格式化成功     : " -NoNewline
    Write-Host $successCount -ForegroundColor Green

    Write-Host "格式化失败     : " -NoNewline

    if ($errorCount -gt 0) {
        Write-Host $errorCount -ForegroundColor Red
    }
    else {
        Write-Host "0" -ForegroundColor Green
    }

    Write-Host ""

    if ($errorCount -eq 0) {

        Write-Host "✓ 所有文件格式化成功。" `
            -ForegroundColor Green

        Write-Host ""

        exit 0
    }

    Write-Warning "部分文件格式化失败，请检查上面的错误信息。"

    exit 1
}
