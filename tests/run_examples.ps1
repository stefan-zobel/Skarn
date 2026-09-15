# run_examples.ps1 -- runs every program under examples/ and checks it still prints exactly what it did.
#
# The examples are realistic, newcomer-style programs (see examples/README.md). Each lives in its own
# directory:
#   examples/<name>/main.skn       the entry program (sibling .skn files / sub-directories are its modules)
#   examples/<name>/expected.out   the exact standard output
#   examples/<name>/args.txt       optional: whitespace-separated command-line arguments
#   anything else                  data files the program reads (input.txt, data.csv, ...)
#
# Each example runs with `--strict` (a warning is fatal) in a FRESH COPY of its directory under %TEMP%, as
# the working directory -- so relative paths resolve to its data files and anything it writes never lands
# in the repository. It passes when the exit code is 0, standard error is empty, and standard output equals
# expected.out byte for byte (line endings normalized to LF).
#
# Usage (from a developer shell, repo root or anywhere):
#   powershell -File tests/run_examples.ps1
#   powershell -File tests/run_examples.ps1 -Config Debug
#   powershell -File tests/run_examples.ps1 -Only word_freq
#   powershell -File tests/run_examples.ps1 -Update     # (re)write expected.out from the current output --
#                                                        # review the diff before keeping it
#
# Exit code: 0 = every example matched; 1 = at least one did not (or the driver is missing).

param(
    [string]$Exe        = '',
    [string]$Config     = 'Release',
    [string]$Only       = '',
    [int]   $TimeoutSec = 60,
    [switch]$Update
)

$ErrorActionPreference = 'Stop'

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = (Resolve-Path (Join-Path $scriptDir '..')).Path
$examplesDir = Join-Path $repoRoot 'examples'
$utf8 = New-Object System.Text.UTF8Encoding($false)

if ([string]::IsNullOrEmpty($Exe)) {
    $Exe = Join-Path $repoRoot ("x64\{0}\static_vmrun.exe" -f $Config)
}
if (-not (Test-Path $Exe)) {
    Write-Host "ERROR: static_vmrun not found at '$Exe'. Build the solution (x64 $Config) first." -ForegroundColor Red
    exit 1
}

$dirs = Get-ChildItem -Path $examplesDir -Directory | Where-Object { Test-Path (Join-Path $_.FullName 'main.skn') } |
        Sort-Object Name
if (-not [string]::IsNullOrEmpty($Only)) { $dirs = $dirs | Where-Object { $_.Name -eq $Only } }
if (@($dirs).Count -eq 0) {
    Write-Host "ERROR: no examples found under '$examplesDir'$(if ($Only) { " matching '$Only'" })." -ForegroundColor Red
    exit 1
}

function Invoke-Example([string]$workDir, [string[]]$arguments) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.Arguments = ($arguments | ForEach-Object { '"' + $_ + '"' }) -join ' '
    $psi.WorkingDirectory = $workDir
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.StandardOutputEncoding = $utf8
    $psi.StandardErrorEncoding = $utf8
    $proc = [System.Diagnostics.Process]::Start($psi)
    $proc.StandardInput.Close()
    $outTask = $proc.StandardOutput.ReadToEndAsync()
    $errTask = $proc.StandardError.ReadToEndAsync()
    if (-not $proc.WaitForExit($TimeoutSec * 1000)) {
        try { $proc.Kill() } catch { }
        return [pscustomobject]@{ Code = -1; Out = ''; Err = "timed out after $TimeoutSec s" }
    }
    $proc.WaitForExit()
    return [pscustomobject]@{ Code = $proc.ExitCode; Out = $outTask.Result; Err = $errTask.Result }
}

$workRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("skarn_examples_" + [System.Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $workRoot | Out-Null

$passed = 0
$failed = 0
$failNames = @()
try {
    foreach ($d in $dirs) {
        $work = Join-Path $workRoot $d.Name
        Copy-Item -Path $d.FullName -Destination $work -Recurse
        $expectedPath = Join-Path $d.FullName 'expected.out'
        Remove-Item (Join-Path $work 'expected.out') -ErrorAction SilentlyContinue   # not visible to the program

        $arguments = @('--strict', 'main.skn')
        $argsPath = Join-Path $d.FullName 'args.txt'
        if (Test-Path $argsPath) {
            $arguments += ([System.IO.File]::ReadAllText($argsPath, $utf8) -split '\s+' | Where-Object { $_ -ne '' })
        }

        $r = Invoke-Example $work $arguments
        $out = $r.Out -replace "`r`n", "`n"
        $why = ''
        if ($r.Code -ne 0)                      { $why = "exit code $($r.Code); stderr: $($r.Err.Trim())" }
        elseif (-not [string]::IsNullOrEmpty($r.Err)) { $why = "unexpected stderr: $($r.Err.Trim())" }
        elseif ($Update) {
            [System.IO.File]::WriteAllText($expectedPath, $out, $utf8)
        }
        elseif (-not (Test-Path $expectedPath)) { $why = "no expected.out (run with -Update to create it)" }
        else {
            $expected = [System.IO.File]::ReadAllText($expectedPath, $utf8) -replace "`r`n", "`n"
            if ($out -ne $expected) {
                $o = $out -split "`n"; $e = $expected -split "`n"
                $i = 0
                while ($i -lt [Math]::Min($o.Count, $e.Count) -and $o[$i] -eq $e[$i]) { $i++ }
                $got  = if ($i -lt $o.Count) { $o[$i] } else { '<end of output>' }
                $want = if ($i -lt $e.Count) { $e[$i] } else { '<end of expected.out>' }
                $why = "output differs at line $($i + 1): got '$got', expected '$want'"
            }
        }

        if ($why -eq '') {
            $passed++
            $tag = if ($Update) { 'UPDATED' } else { 'PASS' }
            Write-Host ("{0,-7} examples/{1}" -f $tag, $d.Name) -ForegroundColor Green
        } else {
            $failed++; $failNames += $d.Name
            Write-Host ("FAIL    examples/{0}  {1}" -f $d.Name, $why) -ForegroundColor Red
        }
    }
} finally {
    Remove-Item -Recurse -Force $workRoot -ErrorAction SilentlyContinue
}

Write-Host ''
Write-Host ("==== examples: {0} passed, {1} failed ====" -f $passed, $failed)
if ($failed -gt 0) {
    Write-Host 'Failing examples:' -ForegroundColor Red
    $failNames | ForEach-Object { Write-Host "  - $_" -ForegroundColor Red }
    exit 1
}
exit 0
