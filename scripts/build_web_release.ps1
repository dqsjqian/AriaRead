#!/usr/bin/env pwsh
# Compatibility entry point. MSVC x64 is the default; MinGW requires -Toolchain mingw.
param(
    [switch]$Clean,
    [switch]$SkipCMake,
    [string]$Config = "Release",
    [ValidateSet("auto", "msvc", "mingw")][string]$Toolchain = "auto",
    [string]$Generator,
    [string]$BuildDir,
    [string]$DepsPrefix,
    [int]$Jobs,
    [ValidateSet("auto", "openssl", "schannel")][string]$TlsBackend = "auto",
    [switch]$Offline,
    [switch]$Test,
    [switch]$RequireWebTests,
    [switch]$ConfigureOnly,
    [switch]$CleanOnly
)
$ErrorActionPreference = "Stop"
$buildArgs = @((Join-Path $PSScriptRoot "../tools/build.py"), "--config", $Config,
               "--toolchain", $Toolchain, "--tls-backend", $TlsBackend)
if ($Generator) { $buildArgs += @("--generator", $Generator) }
if ($BuildDir) { $buildArgs += @("--build-dir", $BuildDir) }
if ($DepsPrefix) { $buildArgs += @("--deps-prefix", $DepsPrefix) }
if ($PSBoundParameters.ContainsKey("Jobs")) { $buildArgs += @("--jobs", "$Jobs") }
if ($Clean) { $buildArgs += "--clean" }
if ($SkipCMake) { $buildArgs += "--skip-cmake" }
if ($Offline) { $buildArgs += "--offline" }
if ($Test) { $buildArgs += "--test" }
if ($RequireWebTests) { $buildArgs += "--require-web-tests" }
if ($ConfigureOnly) { $buildArgs += "--configure-only" }
if ($CleanOnly) { $buildArgs += "--clean-only" }
if (Get-Command py -ErrorAction SilentlyContinue) {
    & py -3 @buildArgs
} elseif (Get-Command python -ErrorAction SilentlyContinue) {
    & python @buildArgs
} else {
    throw "Python 3.10+ is required. Install Python and reopen this terminal."
}
if ($LASTEXITCODE -ne 0) { throw "AriaRead build failed (exit $LASTEXITCODE)" }
