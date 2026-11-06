# Validate the executable's PE imports before starting a Windows GUI process.
# A missing import fails before main() and can leave a system error dialog open.
param(
    [Parameter(Mandatory = $true)][string]$AppDir,
    [Parameter(Mandatory = $true)][string]$DumpbinPath
)

$ErrorActionPreference = 'Stop'
$exe = Join-Path $AppDir 'maichat.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw "Missing executable: $exe" }
if (-not (Test-Path -LiteralPath $DumpbinPath)) { throw "Missing dumpbin: $DumpbinPath" }

# ORT is loaded dynamically and is absent from maichat.exe's PE import graph.
# Validate its own direct/delay imports before shipping a package that can load Portrait.
$binaries = @($exe)
foreach ($runtimeName in 'onnxruntime.dll', 'onnxruntime_providers_shared.dll') {
    $runtime = Join-Path $AppDir $runtimeName
    if (Test-Path -LiteralPath $runtime -PathType Leaf) { $binaries += $runtime }
}
$imports = New-Object System.Collections.Generic.List[string]
foreach ($binary in $binaries) {
    $output = & $DumpbinPath /dependents $binary
    if ($LASTEXITCODE -ne 0) { throw "dumpbin /dependents failed for ${binary}: $LASTEXITCODE" }
    $inImports = $false
    $binaryImportCount = 0
    foreach ($line in $output) {
        if ($line -match 'Image has the following (delay load )?dependencies:') {
            $inImports = $true
            continue
        }
        if ($inImports -and $line -match '^\s*([A-Za-z0-9._-]+\.dll)\s*$') {
            $imports.Add($Matches[1])
            $binaryImportCount++
        }
    }
    if ($binaryImportCount -eq 0) { throw "No PE imports found in $binary" }
}

$missing = New-Object System.Collections.Generic.List[string]
foreach ($name in ($imports | Select-Object -Unique)) {
    # API-set DLL names are virtual contracts resolved by the Windows loader.
    if ($name -match '^(api|ext)-ms-win-') { continue }
    if ((Test-Path -LiteralPath (Join-Path $AppDir $name)) -or
        (Test-Path -LiteralPath (Join-Path $env:WINDIR "System32\$name"))) {
        continue
    }
    $missing.Add($name)
}
foreach ($plugin in 'platforms\qwindows.dll', 'sqldrivers\qsqlite.dll') {
    if (-not (Test-Path -LiteralPath (Join-Path $AppDir $plugin))) {
        $missing.Add($plugin)
    }
}
if ($missing.Count -gt 0) {
    throw "Missing MaiChat runtime dependencies in ${AppDir}: $($missing -join ', ')"
}
Write-Host "MaiChat runtime verified: $($binaries.Count) binaries, $($imports.Count) PE imports and Qt plugins"
