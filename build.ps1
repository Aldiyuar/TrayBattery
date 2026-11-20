$ErrorActionPreference = "Stop"

$kitsRoot = "C:\Program Files (x86)\Windows Kits\10"
$sdkVer = Get-ChildItem "$kitsRoot\Include" -Directory -ErrorAction SilentlyContinue |
    Where-Object { Test-Path (Join-Path "$kitsRoot\Lib" "$($_.Name)\um\x64\kernel32.lib") } |
    Sort-Object { try { [version]$_.Name } catch { [version]"0.0.0.0" } } -Descending |
    Select-Object -First 1 -ExpandProperty Name

if (-not $sdkVer) {
    throw "Windows 10/11 SDK (um\x64) not found under $kitsRoot"
}

$inc = Join-Path $kitsRoot "Include\$sdkVer"
$lib = Join-Path $kitsRoot "Lib\$sdkVer"

function Resolve-LlvmTool($name) {
    $cmd = Get-Command $name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $candidates = @(
        "C:\Program Files\LLVM\bin\$name.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin\$name.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Tools\Llvm\x64\bin\$name.exe"
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { return $c }
    }
    throw "$name not found on PATH or standard LLVM locations"
}

$clang = Resolve-LlvmTool "clang++"
$lld = Resolve-LlvmTool "lld-link"

Write-Host "Using Windows SDK: $sdkVer"
Write-Host "Using Compiler:    $clang"

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$objDir = Join-Path $root "obj"
if (!(Test-Path $objDir)) { New-Item -ItemType Directory -Path $objDir | Out-Null }

$sources = @(
    "src\crt_shims.cpp",
    "src\autostart.cpp",
    "src\hid_monitor.cpp",
    "src\bt_monitor.cpp",
    "src\tray_icons.cpp",
    "src\taskbar_band.cpp",
    "src\flyout.cpp",
    "src\main.cpp"
)

$objs = @()
foreach ($s in $sources) {
    $srcPath = Join-Path $root $s
    $objPath = Join-Path $objDir ([System.IO.Path]::GetFileNameWithoutExtension($s) + ".obj")
    Write-Host "Compiling $s..."
    $argsList = @(
        "-target", "x86_64-pc-windows-msvc",
        "-std=c++20", "-O2", "-Wall", "-Wextra", "-fno-exceptions", "-fno-rtti",
        "-DUNICODE", "-D_UNICODE",
        "-I$inc\ucrt", "-I$inc\um", "-I$inc\shared",
        "-c", $srcPath, "-o", $objPath
    )
    if ($s -eq "src\crt_shims.cpp") { $argsList += "-fno-builtin" }
    & $clang @argsList
    if ($LASTEXITCODE -ne 0) { throw "Compilation failed for $s" }
    $objs += $objPath
}

$outExe = Join-Path $root "TrayBattery.exe"
Write-Host "Linking TrayBattery.exe..."
& $lld /SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup `
    /LIBPATH:"$lib\ucrt\x64" /LIBPATH:"$lib\um\x64" `
    ucrt.lib kernel32.lib user32.lib gdi32.lib gdiplus.lib d2d1.lib dwrite.lib msimg32.lib shell32.lib advapi32.lib `
    setupapi.lib hid.lib cfgmgr32.lib bthprops.lib bluetoothapis.lib `
    /OUT:$outExe @objs
if ($LASTEXITCODE -ne 0) { throw "Linking failed" }

Write-Host "Done: $outExe ($((Get-Item $outExe).Length) bytes)"
