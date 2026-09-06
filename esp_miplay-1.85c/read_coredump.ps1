# read_coredump.ps1 - 从 ESP32-S3 读取并解析 coredump
# 用法: .\read_coredump.ps1 [-Port COM35] [-Baud 460800] [-Force]
# -Force: 忽略 SHA256 不匹配（固件已更新但 coredump 来自旧版本时使用）

param(
    [string]$Port = "COM35",
    [int]$Baud = 460800,
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$CoreBin = Join-Path $ScriptDir "coredump_flash.bin"
$Elf = Join-Path $ScriptDir "build\dlna.elf"
$Gdb = "E:\ESP\.espressif\tools\xtensa-esp-elf-gdb\16.3_20250913\xtensa-esp-elf-gdb\bin\xtensa-esp32s3-elf-gdb.exe"
$IdfPython = "C:\Users\MR\.espressif\python_env\idf5.5_py3.14_env\Scripts\python.exe"
$LoaderPy = "C:\Users\MR\.espressif\python_env\idf5.5_py3.14_env\Lib\site-packages\esp_coredump\corefile\loader.py"

# 1. 从 flash 读取 coredump 分区 (0x810000, 64KB)
Write-Host "[1/4] 从 flash 读取 coredump ($Port)..." -ForegroundColor Cyan
python -m esptool --port $Port --baud $Baud read_flash 0x810000 0x10000 $CoreBin 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: esptool 读取失败" -ForegroundColor Red
    exit 1
}

# 检查是否全空
$bytes = [System.IO.File]::ReadAllBytes($CoreBin)
$nonZero = ($bytes | Where-Object { $_ -ne 0 }).Count
if ($nonZero -lt 100) {
    Write-Host "WARNING: coredump 分区几乎全空 ($nonZero 非零字节)，可能没有崩溃数据" -ForegroundColor Yellow
    exit 0
}
Write-Host "  读取成功: $nonZero 非零字节 / $($bytes.Length) 总字节" -ForegroundColor Green

# 2. 检查 ELF 是否匹配
Write-Host "[2/4] 检查 ELF 匹配..." -ForegroundColor Cyan
$output = & $IdfPython -m esp_coredump info_corefile -c $CoreBin $Elf --gdb $Gdb 2>&1
$shaMismatch = $output | Select-String "SHA256"
if ($shaMismatch) {
    Write-Host "  ELF SHA 不匹配 (固件已更新)" -ForegroundColor Yellow
    if (-not $Force) {
        Write-Host "  使用 -Force 参数强制解析（符号可能不完全匹配）" -ForegroundColor Yellow
        Write-Host "  或手动: git checkout <crash-commit> -- main/dlna.c; idf.py build" -ForegroundColor Yellow
        exit 1
    }
    Write-Host "  -Force: 临时禁用 SHA 检查..." -ForegroundColor Yellow
    # 临时 patch loader.py
    $loaderContent = Get-Content $LoaderPy -Raw
    $loaderBackup = $LoaderPy + ".bak"
    Copy-Item $LoaderPy $loaderBackup -Force
    $patched = $loaderContent -replace `
        "raise ESPCoreDumpLoaderError\(\s*'Invalid application image for coredump: coredump SHA256",
        "logging.warning('SHA256 mismatch (forced): coredump SHA256"
    Set-Content $LoaderPy $patched -NoNewline
    Write-Host "  SHA 检查已临时禁用" -ForegroundColor Green
}

# 3. 解析 coredump
Write-Host "[3/4] 解析 coredump..." -ForegroundColor Cyan
$output = & $IdfPython -m esp_coredump info_corefile -c $CoreBin --gdb $Gdb $Elf 2>&1

# 恢复 loader.py
if ($shaMismatch -and $Force) {
    Copy-Item $loaderBackup $LoaderPy -Force
    Remove-Item $loaderBackup -Force
    Write-Host "  SHA 检查已恢复" -ForegroundColor Green
}

# 4. 输出关键信息
Write-Host "`n[4/4] 崩溃摘要:" -ForegroundColor Cyan
$output | ForEach-Object {
    $line = $_.ToString()
    # 高亮关键行
    if ($line -match "exccause|excvaddr|esp_gmf_alc|LoadProhibited|StoreProhibited|Guru|panic|abort") {
        Write-Host $line -ForegroundColor Red
    } elseif ($line -match "^#\d|pc\s+0x") {
        Write-Host $line -ForegroundColor Yellow
    } else {
        Write-Host $line
    }
}
