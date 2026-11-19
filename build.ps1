[CmdletBinding()]
param(
    [string] $ToolchainDir
)

$ErrorActionPreference = 'Stop'

if ($ToolchainDir) {
    $toolchainRoot = (Resolve-Path -LiteralPath $ToolchainDir).Path
    $toolchainBin = Join-Path $toolchainRoot 'bin'
    $compiler = Join-Path $toolchainBin 'clang++.exe'
    $resourceCompiler = Join-Path $toolchainBin 'llvm-rc.exe'

    foreach ($tool in @($compiler, $resourceCompiler)) {
        if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) {
            throw "LLVM-MinGW tool not found: $tool. Pass the directory containing bin to -ToolchainDir."
        }
    }
} else {
    $compiler = (Get-Command 'clang++.exe' -CommandType Application).Source
    $resourceCompiler = (Get-Command 'llvm-rc.exe' -CommandType Application).Source
}

$sourceDir = Join-Path $PSScriptRoot 'src'
$outputDir = Join-Path $PSScriptRoot 'dist'
$executable = Join-Path $outputDir 'VoltRay.exe'
$resource = Join-Path $outputDir 'VoltRay.res'
New-Item -ItemType Directory -Path $outputDir -Force | Out-Null

Push-Location -LiteralPath $sourceDir
try {
    & $resourceCompiler /FO $resource 'VoltRay.rc'
    if ($LASTEXITCODE -ne 0) {
        throw "Resource compilation failed with exit code $LASTEXITCODE."
    }

    $compilerArgs = @(
        '--target=x86_64-w64-mingw32', '-std=c++20', '-O2', '-s',
        '-static', '-municode', '-mwindows',
        'VoltRay.cpp', $resource, '-o', $executable,
        '-lsetupapi', '-lcfgmgr32', '-lhid', '-lbthprops', '-lbluetoothapis',
        '-lcomctl32', '-lwinmm', '-lole32', '-lgdi32', '-lshell32', '-lcomdlg32',
        '-lruntimeobject', '-ldwmapi', '-luxtheme'
    )
    & $compiler @compilerArgs
    if ($LASTEXITCODE -ne 0) {
        throw "VoltRay build failed with exit code $LASTEXITCODE."
    }
} finally {
    Pop-Location
}

Write-Output $executable
