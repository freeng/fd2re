# fd2re 构建脚本（不依赖 make；SDL2/ADLMIDI 静态库由 MSVC 构建）
# 用法: powershell -ExecutionPolicy Bypass -File build.ps1
$ErrorActionPreference = "Stop"

$config = @{}
Get-Content (Join-Path $PSScriptRoot ".env") | ForEach-Object {
    if ($_ -match '^\s*([A-Z0-9_]+)\s*=\s*(.*?)\s*$') {
        $config[$matches[1]] = $matches[2]
    }
}
foreach ($required in "SDL2_INCLUDE", "SDL2_LIBRARY", "MSVC_SETUP", "SDL2_SYSTEM_LIBS", "ADLMIDI_INCLUDE", "ADLMIDI_LIBRARY") {
    if (-not $config.ContainsKey($required)) { throw ".env is missing $required" }
}
$sdlInclude = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot $config["SDL2_INCLUDE"]))
$sdlLibrary = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot $config["SDL2_LIBRARY"]))
$adlInclude = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot $config["ADLMIDI_INCLUDE"]))
$adlLibrary = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot $config["ADLMIDI_LIBRARY"]))
$msvcSetup = $config["MSVC_SETUP"]
if (-not (Test-Path $sdlInclude)) { throw "SDL2 include directory not found: $sdlInclude" }
if (-not (Test-Path $sdlLibrary)) { throw "SDL2 library not found: $sdlLibrary" }
if (-not (Test-Path $adlInclude)) { throw "ADLMIDI include directory not found: $adlInclude" }
if (-not (Test-Path $adlLibrary)) { throw "ADLMIDI library not found: $adlLibrary" }
if (-not (Test-Path $msvcSetup)) { throw "MSVC setup script not found: $msvcSetup" }
$sdlSystemLibs = $config["SDL2_SYSTEM_LIBS"].Split(' ', [System.StringSplitOptions]::RemoveEmptyEntries)

$src = Get-ChildItem (Join-Path $PSScriptRoot "src") -Filter *.c | Sort-Object Name
$buildDir = Join-Path $PSScriptRoot "build"
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

function Test-OutputOccupied([string]$path) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return $false }
    $stream = $null
    try {
        $stream = [IO.File]::Open(
            $path,
            [IO.FileMode]::Open,
            [IO.FileAccess]::ReadWrite,
            [IO.FileShare]::None)
        return $false
    } catch [IO.IOException] {
        return $true
    } finally {
        if ($null -ne $stream) { $stream.Dispose() }
    }
}

$outputName = "fd2re.exe"
$outputPath = Join-Path $PSScriptRoot $outputName
if (Test-OutputOccupied $outputPath) {
    $suffix = 1
    do {
        $candidateName = "fd2re-{0}.exe" -f $suffix
        $candidatePath = Join-Path $PSScriptRoot $candidateName
        $suffix++
    } while (Test-Path -LiteralPath $candidatePath)
    $outputName = $candidateName
    $outputPath = $candidatePath
    Write-Host "fd2re.exe is occupied; using $outputName"
}

$objects = @()
$compile = foreach ($file in $src) {
    $object = Join-Path $buildDir ($file.BaseName + ".obj")
    $objects += $object
    # The project intentionally uses the portable C stdio/string APIs.  C4996
    # is MSVC's opt-in secure-CRT migration hint, not a correctness diagnostic
    # for this DOS-era compatibility layer, so keep it out of normal build
    # output while retaining /W4 for actionable warnings.
    'cl /nologo /std:c11 /utf-8 /W4 /wd4100 /MD /D_CRT_SECURE_NO_WARNINGS /Zi /I"{0}" /I"{1}" /I"{2}" /Fo"{3}" /c "{4}"' -f `
        (Join-Path $PSScriptRoot "include"), $sdlInclude, $adlInclude, $object, $file.FullName
}
$objectArgs = ($objects | ForEach-Object { '"' + $_ + '"' }) -join ' '
$systemLibArgs = $sdlSystemLibs -join ' '
$link = 'link /nologo /out:"{0}" {1} "{2}" "{3}" {4}' -f `
    $outputPath, $objectArgs, $sdlLibrary, $adlLibrary, $systemLibArgs
# 源文件渐多后单条 cmd 链超过命令行长度上限，编译/链接命令落盘为批处理再执行；
# 每行带 || exit /b 1 ——批处理默认不因单条失败中止，防止旧 .obj 被静默链接
$batPath = Join-Path $buildDir "build_cmd.bat"
$batLines = @(('call "{0}" -arch=x64 -host_arch=x64 >nul || exit /b 1' -f $msvcSetup)) `
    + ($compile | ForEach-Object { $_ + ' || exit /b 1' }) `
    + @(($link + ' || exit /b 1'))
[IO.File]::WriteAllLines($batPath, $batLines)
& cmd.exe /d /s /c "`"$batPath`""
if ($LASTEXITCODE -ne 0) { throw "build failed" }
Write-Host "OK -> $outputName"
