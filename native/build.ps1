param([switch]$Development, [string]$BuildDirectory)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$projectRoot = Split-Path -Parent $PSScriptRoot

function Invoke-A24ReleaseBuild([string]$ProjectRoot, [string]$BuildRoot, [string]$Cmake, [string]$Ctest) {
    # Only vendored compiler inputs are needed. The public SDK validator remains
    # a separately locked DEV check; no sibling checkout or engine is imported.
    $Lock = Get-Content -LiteralPath (Join-Path $ProjectRoot 'native/dependencies.lock.json') -Raw | ConvertFrom-Json
    foreach ($Dependency in $Lock.dependencies) {
        foreach ($Pair in @(@($Dependency.path, $Dependency.sha256), @($Dependency.noticePath, $Dependency.noticeSha256))) {
            if ((Get-FileHash -LiteralPath (Join-Path $ProjectRoot $Pair[0]) -Algorithm SHA256).Hash.ToLowerInvariant() -cne $Pair[1]) {
                throw 'Locked vendored dependency changed.'
            }
        }
    }
    $ContractRoot = Join-Path $ProjectRoot 'contracts/audio-system/v1'
    $Contract = Get-Content -LiteralPath (Join-Path $ContractRoot 'provenance.json') -Raw | ConvertFrom-Json
    foreach ($Entry in $Contract.files.PSObject.Properties) {
        if ((Get-FileHash -LiteralPath (Join-Path $ContractRoot $Entry.Name) -Algorithm SHA256).Hash.ToLowerInvariant() -cne $Entry.Value) {
            throw 'Locked public audio contract changed.'
        }
    }
    # Do not use mw-development: that optional preset deliberately belongs to
    # the existing sibling DEV tooling. Explicitly clear its module cache value.
    & $Cmake -S $ProjectRoot -B $BuildRoot -G 'Visual Studio 17 2022' -A x64 '-DMW_NATIVE_DEVELOPMENT_MODULE='
    if ($LASTEXITCODE -ne 0) { throw 'A24 release CMake configuration failed.' }
    & $Cmake --build $BuildRoot --config Release --parallel 2
    if ($LASTEXITCODE -ne 0) { throw 'A24 release compilation failed.' }
    & $Ctest --test-dir $BuildRoot -C Release --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'A24 release component checks failed.' }
    $OutputRoot = Join-Path $ProjectRoot 'native/out/windows-x86_64'
    New-Item -ItemType Directory -Path (Join-Path $OutputRoot 'notices') -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $BuildRoot 'Release/companion.exe') -Destination $OutputRoot
    foreach ($File in @('LICENSE', 'THIRD_PARTY_NOTICES.md')) {
        Copy-Item -LiteralPath (Join-Path $ProjectRoot $File) -Destination $OutputRoot
    }
    Copy-Item -LiteralPath (Join-Path $ProjectRoot 'third_party/nlohmann/LICENSE.MIT') -Destination (Join-Path $OutputRoot 'notices/nlohmann-json-LICENSE.MIT')
    Copy-Item -LiteralPath (Join-Path $ContractRoot 'LICENSE.MIT') -Destination (Join-Path $OutputRoot 'notices/a18-audio-system-LICENSE.MIT')
}

if ($Development) {
    & node (Join-Path $projectRoot 'tools/build-candidate.mjs')
    if ($LASTEXITCODE -ne 0) { throw 'A24 DEV candidate build failed' }
    return
}
if ($env:OS -ne 'Windows_NT') { throw 'A24 release requires Windows x64 MSVC.' }
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or -not $vs) { throw 'Visual C++ x64 build tools required.' }
$cmake = Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
$ctest = Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
if (-not $BuildDirectory) { $BuildDirectory = Join-Path ([IO.Path]::GetTempPath()) ('mywallpaper-a24-release-' + [guid]::NewGuid().ToString('N')) }
Invoke-A24ReleaseBuild $projectRoot $BuildDirectory $cmake $ctest
