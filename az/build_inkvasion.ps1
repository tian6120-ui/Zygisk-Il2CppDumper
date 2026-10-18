$ErrorActionPreference = "Stop"

$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$ndk = $env:ANDROID_NDK_HOME
if (-not $ndk) { throw "ANDROID_NDK_HOME missing" }

$build = Join-Path $repo "build-inkvasion"
$src = Join-Path $repo "inkvasion\v1"
$toolchain = Join-Path $ndk "build\cmake\android.toolchain.cmake"

if (Test-Path $build) { Remove-Item -Recurse -Force $build }

cmake -S $src -B $build -G Ninja "-DCMAKE_TOOLCHAIN_FILE=$toolchain" "-DANDROID_ABI=arm64-v8a" "-DANDROID_PLATFORM=android-28" "-DCMAKE_BUILD_TYPE=Release" "-DREPO_ROOT=$repo"
if ($LASTEXITCODE -ne 0) { throw "Inkvasion cmake configure failed" }

cmake --build $build --parallel 4
if ($LASTEXITCODE -ne 0) { throw "Inkvasion native build failed" }

$so = Join-Path $build "libinkvasion_auto.so"
if (-not (Test-Path $so)) { throw "Missing Inkvasion so: $so" }

$outRoot = Join-Path $repo "out"
$out = Join-Path $outRoot "Inkvasion_Standalone_AutoUnlock_V1"
$zygisk = Join-Path $out "zygisk"
$zip = Join-Path $outRoot "Inkvasion_Standalone_AutoUnlock_V1.zip"

if (Test-Path $out) { Remove-Item -Recurse -Force $out }
if (Test-Path $zip) { Remove-Item -Force $zip }

New-Item -ItemType Directory -Force -Path $zygisk | Out-Null
Copy-Item $so (Join-Path $zygisk "arm64-v8a.so")
Copy-Item (Join-Path $src "module.prop") (Join-Path $out "module.prop")
New-Item -ItemType File -Force -Path (Join-Path $out "skip_mount") | Out-Null

Compress-Archive -Path (Join-Path $out "*") -DestinationPath $zip -CompressionLevel Optimal -Force

$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLowerInvariant()
$bytes = [System.IO.File]::ReadAllBytes($zip)
$b64 = [Convert]::ToBase64String($bytes)

Write-Host "INKVASION_ZIP_PATH=$zip"
Write-Host "INKVASION_ZIP_SHA256=$hash"
Write-Host "INKVASION_ZIP_SIZE=$($bytes.Length)"
Write-Host "INKVASION_ZIP_BASE64_BEGIN"
Write-Host $b64
Write-Host "INKVASION_ZIP_BASE64_END"
