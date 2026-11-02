# SPDX-License-Identifier: Apache-2.0
<#
.SYNOPSIS
The Visual Studio 2022 x64 developer environment (cl.exe, link.exe, the Windows SDK, the bundled
ninja) for the Ninja presets, from any shell.

.DESCRIPTION
Run a command in it:
    powershell -NoProfile -ExecutionPolicy Bypass -File tools/dev/vsenv.ps1 cmake --build --preset ninja-maya2022
or bring it into the current PowerShell session (then plain cmake works):
    . tools/dev/vsenv.ps1

vcvars64.bat takes a few seconds, so what it sets is kept in %USERPROFILE%/groomlab-deps/vsenv
(per Visual Studio install and version) and applied from there afterwards. Nothing is done when the
session already is a developer environment.
#>

function Import-GroomLabVsEnv {
    $ErrorActionPreference = "Stop"
    if ($env:VCToolsInstallDir -and $env:VSCMD_ARG_TGT_ARCH -eq "x64" -and (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        return
    }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found: install Visual Studio 2022 with the C++ workload" }
    $vs = & $vswhere -latest -products * -version "[17.0,18.0)" -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw "No Visual Studio 2022 with the x64 C++ tools found" }
    $vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
    $stamp = (Get-Item $vcvars).LastWriteTimeUtc.Ticks
    $tools = Get-ChildItem (Join-Path $vs "VC\Tools\MSVC") -Directory | Sort-Object Name | Select-Object -Last 1
    $key = [BitConverter]::ToString([Security.Cryptography.MD5]::Create().ComputeHash(
        [Text.Encoding]::UTF8.GetBytes("$vcvars|$stamp|$($tools.Name)"))).Replace("-", "").Substring(0, 16)
    $cacheDir = Join-Path $env:USERPROFILE "groomlab-deps\vsenv"
    $cache = Join-Path $cacheDir "$key.json"

    if (Test-Path $cache) {
        $saved = Get-Content $cache -Raw | ConvertFrom-Json
    } else {
        # The variables vcvars64 sets: all of them but PATH as they come, PATH as the entries it adds.
        $before = @{}
        foreach ($line in (cmd.exe /d /c "set")) { $i = $line.IndexOf("="); if ($i -gt 0) { $before[$line.Substring(0, $i)] = $line.Substring($i + 1) } }
        $after = @{}
        foreach ($line in (cmd.exe /d /c "`"$vcvars`" >nul 2>&1 && set")) { $i = $line.IndexOf("="); if ($i -gt 0) { $after[$line.Substring(0, $i)] = $line.Substring($i + 1) } }
        if (-not $after.ContainsKey("VCToolsInstallDir")) { throw "$vcvars did not set up the environment" }
        $vars = [ordered]@{}
        foreach ($name in $after.Keys) {
            if ($name -ieq "PATH") { continue }
            if (-not $before.ContainsKey($name) -or $before[$name] -ne $after[$name]) { $vars[$name] = $after[$name] }
        }
        $old = @{}
        foreach ($p in ($before["Path"] -split ";")) { if ($p) { $old[$p.TrimEnd("\").ToLowerInvariant()] = $true } }
        $added = @($after["Path"] -split ";" | Where-Object { $_ -and -not $old.ContainsKey($_.TrimEnd("\").ToLowerInvariant()) })
        $saved = [pscustomobject]@{ vars = [pscustomobject]$vars; path = $added }
        New-Item -ItemType Directory -Force $cacheDir | Out-Null
        $tmp = "$cache.$PID.tmp"
        $saved | ConvertTo-Json -Depth 4 | Set-Content -Encoding utf8 $tmp
        Move-Item -Force $tmp $cache
    }
    foreach ($p in $saved.vars.PSObject.Properties) { Set-Item -Path "env:$($p.Name)" -Value $p.Value }
    # The Build Tools bundle a 32-bit CMake and Ninja: everything they start, ctest's tests included,
    # would see Program Files (x86) as ProgramFiles and a redirected System32. A 64-bit CMake and
    # ninja.exe in groomlab-deps/cmake/bin (the portable zip) go first, the 32-bit folders last.
    $added = @($saved.path)
    $own = Join-Path $env:USERPROFILE "groomlab-deps\cmake\bin"
    if ((Test-Path (Join-Path $own "cmake.exe")) -and (Test-Path (Join-Path $own "ninja.exe"))) {
        $x86 = @($added | Where-Object {
            foreach ($exe in "cmake.exe", "ninja.exe") {
                $f = Join-Path $_ $exe
                if (Test-Path $f) {
                    $b = [IO.File]::ReadAllBytes($f)
                    if ([BitConverter]::ToUInt16($b, [BitConverter]::ToInt32($b, 0x3C) + 4) -eq 0x14C) { return $true }
                }
            }
            $false
        })
        $added = @($own) + @($added | Where-Object { $x86 -notcontains $_ })
        $env:PATH = (@($added) + @($env:PATH) + $x86) -join ";"
        return
    }
    $env:PATH = (@($added) + @($env:PATH)) -join ";"
}

Import-GroomLabVsEnv
if ($args.Count -gt 0) {
    $exe = $args[0]
    $rest = @()
    if ($args.Count -gt 1) { $rest = $args[1..($args.Count - 1)] }
    & $exe @rest
    exit $LASTEXITCODE
}
