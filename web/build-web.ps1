# fd2re Emscripten/WASM build.
# The C sources are shared with the Windows build. Browser-specific behavior
# lives in the JS glue and in SDL2's Emscripten backend.
param(
    [string]$Emcc = "",
    [switch]$Clean,
    [switch]$Debug
)
$ErrorActionPreference = "Stop"
$web = $PSScriptRoot
$root = Split-Path $web -Parent
$fd2 = Join-Path $root "fd2re"
$adl = Join-Path $fd2 "vendor\libADLMIDI"

# Keep tool selection in a file rather than inheriting process environment.
$cfg = @{}
$envPath = Join-Path $web ".env"
if (Test-Path -LiteralPath $envPath) {
    Get-Content -LiteralPath $envPath | ForEach-Object {
        if ($_ -match '^\s*([A-Z0-9_]+)\s*=\s*(.*?)\s*$') {
            $cfg[$matches[1]] = $matches[2]
        }
    }
}
if ([string]::IsNullOrWhiteSpace($Emcc)) {
    if ($cfg.ContainsKey("EMCC")) { $Emcc = $cfg["EMCC"] }
    else { $Emcc = "D:\workspace\python\emsdk\upstream\emscripten\emcc.bat" }
}
if (-not (Test-Path -LiteralPath $Emcc)) { throw "emcc not found: $Emcc (set EMCC in web/.env or pass -Emcc)" }
foreach ($p in @(
        (Join-Path $fd2 "include\fd2.h"),
        (Join-Path $adl "include\adlmidi.h"),
        (Join-Path $fd2 "FD2SAMPLE.wopl"),
        (Join-Path $fd2 "FD2.TMP"),
        (Join-Path $root "FD2.SAV")
    )) { if (-not (Test-Path -LiteralPath $p)) { throw "missing dependency $p" } }

$buildDir = Join-Path $web "build"
$distDir = Join-Path $web "dist"
if ($Clean) {
    Remove-Item -LiteralPath $buildDir,$distDir -Recurse -Force -ErrorAction SilentlyContinue
}
New-Item -ItemType Directory -Force -Path $buildDir,$distDir | Out-Null

# This list mirrors the native libADLMIDI static build. The object is cached
# because compiling the emulator is much slower than linking the game.
$adlSrc = @(
    'src/adlmidi.cpp','src/adlmidi_load.cpp','src/adlmidi_midiplay.cpp',
    'src/adlmidi_opl3.cpp','src/adlmidi_private.cpp','src/adlmidi_sequencer.cpp',
    'src/chips/dosbox/dbopl.cpp','src/chips/dosbox_opl2.cpp','src/chips/dosbox_opl3.cpp',
    'src/chips/esfmu/esfm.c','src/chips/esfmu/esfm_registers.c','src/chips/esfmu_opl3.cpp',
    'src/chips/java_opl3.cpp','src/chips/mame/mame_fmopl.cpp','src/chips/mame_opl2.cpp',
    'src/chips/nuked/nukedopl2.c','src/chips/nuked/nukedopl3.c','src/chips/nuked_cqm.cpp',
    'src/chips/nuked_cqm/cqm.c','src/chips/nuked_fast/nukedopl3_fast.c',
    'src/chips/nuked_opl2.cpp','src/chips/nuked_opl3.cpp','src/chips/nuked_opl3_fast.cpp',
    'src/chips/opal/opal.c','src/chips/opal_opl3.cpp',
    'src/chips/ymfm/ymfm_adpcm.cpp','src/chips/ymfm/ymfm_misc.cpp',
    'src/chips/ymfm/ymfm_opl.cpp','src/chips/ymfm/ymfm_pcm.cpp',
    'src/chips/ymfm/ymfm_ssg.cpp','src/chips/ymfm_opl2.cpp','src/chips/ymfm_opl3.cpp',
    'src/models/model_ail.c','src/models/model_apogee.c','src/models/model_dmx.c',
    'src/models/model_generic.c','src/models/model_hmi_sos.c','src/models/model_msadlib.c',
    'src/models/model_oconnell.c','src/models/model_win9x.c','src/wopl/wopl_file.c'
)
$adlObj = Join-Path $buildDir "adlmidi.bundle.o"
if ($Clean -or -not (Test-Path -LiteralPath $adlObj)) {
    Write-Host "[1/2] compiling libADLMIDI"
    Push-Location $adl
    try {
        & $Emcc -O1 -Iinclude -DDISABLE_EMBEDDED_BANKS -DENABLE_END_SILENCE_SKIPPING -r -o $adlObj @adlSrc
        if ($LASTEXITCODE -ne 0) { throw "libADLMIDI compile failed" }
    } finally { Pop-Location }
} else { Write-Host "[1/2] libADLMIDI cache hit" }

$srcFiles = Get-ChildItem (Join-Path $fd2 "src") -Filter *.c | Sort-Object Name
$assets = @('ANI.DAT','BG.DAT','DATO.DAT','FDFIELD.DAT','FDMUS.DAT',
    'FDOTHER.DAT','FDSHAP.DAT','FDTXT.DAT','FIGANI.DAT','TAI.DAT',
    'FDICON.B24','FD2.TMP','FD2SAMPLE.wopl')
if ($Debug) { $opt = @('-O1','-g2','--profiling-funcs','-sASSERTIONS','--minify=0') }
else { $opt = @('-O2') }
$args = @($opt + '-sUSE_SDL=2','-sALLOW_MEMORY_GROWTH=1',
    '-sINITIAL_MEMORY=67108864','-sSTACK_SIZE=1048576','-sINVOKE_RUN=0',
    '-sFORCE_FILESYSTEM=1','-lidbfs.js',
    '--shell-file','shell.html','-I../fd2re/include',
    '-I../fd2re/vendor/libADLMIDI/include')
$args += @('-sASYNCIFY','-sASYNCIFY_IMPORTS=__wrap_usleep,__wrap_SDL_Delay',
    '-sEXPORTED_FUNCTIONS=_main,_vram_base,_g_music_ok,_g_music_track_cur',
    '-sEXPORTED_RUNTIME_METHODS=HEAPU8,FS',
    '--js-library','jslib.js','-Wl,--wrap=SDL_Delay','-Wl,--wrap=usleep')
$args += @('-o','dist/index.html')
# emcc response files use POSIX escaping even on Windows. Quote every native
# path and use forward slashes so drive paths are not collapsed to D:xxx.
foreach ($f in $srcFiles) { $args += ('"' + ($f.FullName -replace '\\','/') + '"') }
$args += ('"' + ($adlObj -replace '\\','/') + '"')
foreach ($a in $assets) {
    $args += '--preload-file'
    $args += ('"../fd2re/{0}@/{0}"' -f $a)
}
$args += '--preload-file'
$args += '"../FD2.SAV@/FD2.SAV"'
$rsp = Join-Path $buildDir "link.rsp"
[IO.File]::WriteAllLines($rsp, $args)

Write-Host "[2/2] linking $($srcFiles.Count) C sources"
Push-Location $web
try { & $Emcc "@$rsp"; if ($LASTEXITCODE -ne 0) { throw "WASM link failed" } }
finally { Pop-Location }
Write-Host "OK -> $distDir\index.html"
