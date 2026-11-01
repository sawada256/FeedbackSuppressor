# Builds, tests, (optionally) validates and packages the Windows x64 release:
# VST3 + Standalone ZIP and an Inno Setup installer.
# Requires Visual Studio 2022 (C++), CMake and Inno Setup 6.
$ErrorActionPreference = "Stop"
Set-Location (Join-Path $PSScriptRoot "..")
$version = (Select-String -Path CMakeLists.txt -Pattern 'project\(FeedbackSuppressor VERSION ([0-9.]+)').Matches[0].Groups[1].Value
$name = "Feedback Suppressor V2.1"
$build = "build-windows"
cmake -S . -B $build -G "Visual Studio 17 2022" -A x64
if ($LASTEXITCODE) { exit $LASTEXITCODE }
cmake --build $build --config Release --parallel
if ($LASTEXITCODE) { exit $LASTEXITCODE }
ctest --test-dir $build -C Release --output-on-failure
if ($LASTEXITCODE) { exit $LASTEXITCODE }
$art = "$build/FeedbackSuppressor_artefacts/Release"

if ($env:FS_PLUGINVAL) {
  $pv = Join-Path $env:RUNNER_TEMP "pluginval"
  Invoke-WebRequest "https://github.com/Tracktion/pluginval/releases/download/v1.0.4/pluginval_Windows.zip" -OutFile "$pv.zip"
  Expand-Archive "$pv.zip" -DestinationPath $pv -Force
  & "$pv\pluginval.exe" --strictness-level 5 --validate-in-process --validate "$art/VST3/$name.vst3"
  if ($LASTEXITCODE) { Write-Error "pluginval failed"; exit $LASTEXITCODE }
}

New-Item -ItemType Directory -Force dist | Out-Null
$stage = Join-Path "dist" "FeedbackSuppressorV2.1-$version-Windows-x64"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item -Recurse "$art/VST3/$name.vst3" $stage
Copy-Item "$art/Standalone/$name.exe" $stage
Copy-Item README.md, LICENSE, THIRD_PARTY.md $stage
Copy-Item -Recurse licenses, docs $stage
Compress-Archive -Path $stage -DestinationPath "dist/FeedbackSuppressorV2.1-$version-Windows-x64.zip" -Force

$iscc = "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe"
if (-not (Test-Path $iscc)) { choco install innosetup -y --no-progress }
& $iscc "/DAppVersion=$version" "/DBuildDir=..\$build" installer\FeedbackSuppressorV21.iss
if ($LASTEXITCODE) { exit $LASTEXITCODE }
Get-ChildItem dist
