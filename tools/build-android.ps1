# ==============================================================================================
#  AvatarForge :: tools/build-android.ps1   (Windows helper, PowerShell 5+)
# ==============================================================================================
#
#  Builds the arm64-v8a native libraries on Windows and then packages the debug APK through the
#  Gradle wrapper that ships with this repository. Nothing here needs Android Studio.
#
#  NOTE ON PROVENANCE: this script is the Windows port of tools/build_android.sh, which has been
#  executed and verified on Linux/arm64-target in this project. This .ps1 has *not* been run - there
#  is no Windows host in the build environment - so treat it as a documented convenience and compare
#  its cmake command line against tools/build_android.sh if anything looks off. If a Windows build
#  misbehaves, running the four commands printed by `-DryRun` by hand is identical work.
#
#  Usage (from the repository root):
#      powershell -ExecutionPolicy Bypass -File tools\build-android.ps1
#      powershell -ExecutionPolicy Bypass -File tools\build-android.ps1 -Abi x86_64 -SkipGradle
#      powershell -ExecutionPolicy Bypass -File tools\build-android.ps1 -DryRun
#
#  Requirements: JDK 17, ANDROID_HOME pointing at an SDK with platform 34, NDK r26 and CMake 3.22.
# ==============================================================================================
[CmdletBinding()]
param(
    [string]$Abi = "arm64-v8a",
    [string]$Platform = "android-26",
    [string]$Sdk = $env:ANDROID_HOME,
    [switch]$SkipGradle,
    [switch]$Release,
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot          # tools\ -> repository root
Write-Host "AvatarForge native build (Windows)" -ForegroundColor Cyan
Write-Host "  repository : $root"

# --- 1. locate the SDK --------------------------------------------------------------------------
# local.properties wins over the environment: that is the file Android Studio writes, so a Windows
# user who opened the project once already has the right answer in it.
$localProps = Join-Path $root "local.properties"
if (-not $Sdk -and (Test-Path $localProps)) {
    $line = Get-Content $localProps | Where-Object { $_ -match '^\s*sdk\.dir\s*=' } | Select-Object -First 1
    if ($line) {
        $Sdk = ($line -replace '^\s*sdk\.dir\s*=', '').Trim()
        # Gradle writes forward slashes and escapes the drive colon; PowerShell wants a real path.
        $Sdk = $Sdk -replace '\\', ''
    }
}
if (-not $Sdk) { throw "set ANDROID_HOME (or write sdk.dir into local.properties) before running this" }
if (-not (Test-Path $Sdk)) { throw "Android SDK not found at '$Sdk'" }
Write-Host "  sdk        : $Sdk"

# --- 2. locate NDK, CMake and ninja -------------------------------------------------------------
$ndkRoot = Join-Path $Sdk "ndk"
if (-not (Test-Path $ndkRoot)) { throw "no NDK under $ndkRoot - run: sdkmanager `"ndk;26.1.10909125`"" }
$ndk = Get-ChildItem $ndkRoot -Directory | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
$toolchain = Join-Path $ndk.FullName "build\cmake\android.toolchain.cmake"
if (-not (Test-Path $toolchain)) { throw "NDK at $($ndk.FullName) has no CMake toolchain file" }

$cmakeRoot = Join-Path $Sdk "cmake"
if (-not (Test-Path $cmakeRoot)) { throw "no CMake under $cmakeRoot - run: sdkmanager `"cmake;3.22.1`"" }
$cmakeExe = (Get-ChildItem $cmakeRoot -Directory | Sort-Object { [version]$_.Name } -Descending |
             ForEach-Object { Join-Path $_.FullName "bin\cmake.exe" } |
             Where-Object { Test-Path $_ } | Select-Object -First 1)
if (-not $cmakeExe) { throw "no cmake.exe found under $cmakeRoot" }
$ninja = Join-Path (Split-Path -Parent $cmakeExe) "ninja.exe"
if (-not (Test-Path $ninja)) { throw "ninja.exe is not next to cmake.exe ($ninja) - reinstall the SDK cmake package" }
Write-Host "  ndk        : $($ndk.Name)"
Write-Host "  cmake      : $cmakeExe"

# --- 3. configure + build ----------------------------------------------------------------------
# One directory per ABI so an x86_64 emulator build does not clobber an arm64 device build.
$buildDir = if ($Abi -eq "arm64-v8a") { Join-Path $root "build-android" } else { Join-Path $root "build-android-$Abi" }
$cmakeArgs = @(
    "-S", (Join-Path $root "native"),
    "-B", $buildDir,
    "-G", "Ninja",
    "-DCMAKE_TOOLCHAIN_FILE=$toolchain",
    "-DANDROID_ABI=$Abi",
    "-DANDROID_PLATFORM=$Platform",
    "-DANDROID_STL=c++_static",       # static STL: the APK ships three .so files and no libc++_shared.so
    "-DCMAKE_BUILD_TYPE=Release",
    "-DCMAKE_MAKE_PROGRAM=$ninja"
)

if ($DryRun) {
    Write-Host "`n[dry run] cmake:" -ForegroundColor Yellow
    Write-Host "  & '$cmakeExe' $($cmakeArgs -join ' ')"
    Write-Host "[dry run] ninja:" -ForegroundColor Yellow
    Write-Host "  & '$ninja' -C '$buildDir'"
    exit 0
}

Push-Location $root
try {
    & $cmakeExe @cmakeArgs
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
    & $ninja -C $buildDir
    if ($LASTEXITCODE -ne 0) { throw "ninja build failed" }

    # --- 4. install into prebuilt\<abi> (what :sdk packages with -Paf.prebuiltJni) --------------
    $out = Join-Path $root "prebuilt\$Abi"
    New-Item -ItemType Directory -Force -Path $out | Out-Null
    Get-ChildItem (Join-Path $buildDir "*.so") | Copy-Item -Destination $out -Force
    Write-Host "`ninstalled into prebuilt\$Abi :" -ForegroundColor Green
    Get-ChildItem $out | Format-Table Name, Length -AutoSize

    # --- 5. package the APK --------------------------------------------------------------------
    if (-not $SkipGradle) {
        $task = if ($Release) { ":app:assembleRelease" } else { ":app:assembleDebug" }
        $gradlew = Join-Path $root "gradlew.bat"
        if (-not (Test-Path $gradlew)) { throw "gradlew.bat is missing from the repository root" }
        Write-Host "`nrunning $task ..." -ForegroundColor Cyan
        & $gradlew --no-daemon $task "-Paf.abIs=$Abi" "-Paf.prebuiltJni=$root\prebuilt"
        if ($LASTEXITCODE -ne 0) { throw "gradle $task failed" }
        Write-Host "`nAPK written under app\build\outputs\apk\" -ForegroundColor Green
    }
}
finally {
    Pop-Location
}
