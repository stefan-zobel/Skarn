# run_guide_claims.ps1 -- the executable guide-claim checker.
#
# Each *.skn under this directory is a self-contained Skarn program that encodes ONE
# categorical claim one of the two guides makes -- SkarnGuide.md (tier1/2/3) or
# SkarnIn30Minutes.md (intro/). A header comment declares the expected outcome; this runner
# compiles+runs each program with static_vmrun and asserts the outcome matches. The point:
# a guide claim that silently drifts from the compiler's actual behaviour fails here instead
# of misleading a reader.
#
# Directive (a line inside the .skn, so the file still compiles):
#   // EXPECT: ok   <exact-stdout>     -- exit 0, trimmed stdout equals <exact-stdout>
#   // EXPECT: warn <stderr-substring> -- exit 0, stderr CONTAINS <stderr-substring>
#   // EXPECT: fail <stderr-substring> -- exit != 0, stderr CONTAINS <stderr-substring>
# (// CLAIM:/// GUIDE: lines are documentation only.)
#
# Usage (from a developer shell, repo root or anywhere):
#   powershell -File tests/guide_claims/run_guide_claims.ps1
#   powershell -File tests/guide_claims/run_guide_claims.ps1 -Config Debug
#   powershell -File tests/guide_claims/run_guide_claims.ps1 -Exe path\to\static_vmrun.exe
#
# After the claims it runs run_guide_examples.ps1 (every ```rust block of both guides), then
# tests/run_examples.ps1 (every program under examples/, exact output), then check_anchors.ps1 over both
# guides (every internal link resolves), so one command is the whole doc gate.
# -NoExamples skips all three, and so does a -Dir subset.
#
# Exit code: 0 = all claims (and examples) held; 1 = at least one drifted (or a fixture was malformed).

param(
    [string]$Exe    = '',
    [string]$Config = 'Release',
    [string]$Dir    = '',
    [switch]$NoExamples
)

$ErrorActionPreference = 'Stop'

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$runExamples = (-not $NoExamples) -and [string]::IsNullOrEmpty($Dir)
if ([string]::IsNullOrEmpty($Dir)) { $Dir = $scriptDir }
$repoRoot = (Resolve-Path (Join-Path $scriptDir '..\..')).Path

if ([string]::IsNullOrEmpty($Exe)) {
    $Exe = Join-Path $repoRoot ("x64\{0}\static_vmrun.exe" -f $Config)
}
if (-not (Test-Path $Exe)) {
    Write-Host "ERROR: static_vmrun not found at '$Exe'. Build the solution (x64 $Config) first." -ForegroundColor Red
    exit 1
}

$files = Get-ChildItem -Path $Dir -Filter *.skn -Recurse | Sort-Object FullName
if ($files.Count -eq 0) {
    Write-Host "ERROR: no *.skn claim files found under '$Dir'." -ForegroundColor Red
    exit 1
}

$outFile = [System.IO.Path]::GetTempFileName()
$errFile = [System.IO.Path]::GetTempFileName()
$passed = 0
$failed = 0
$skipped = 0
$failNames = @()

foreach ($f in $files) {
    $rel   = $f.FullName.Substring($repoRoot.Length).TrimStart('\')
    $lines = Get-Content $f.FullName
    $directive = $null
    foreach ($ln in $lines) {
        $m = [regex]::Match($ln, '^\s*//\s*EXPECT:\s*(ok|warn|fail)\b\s?(.*)$')
        if ($m.Success) { $directive = $m; break }
    }
    if ($null -eq $directive) {
        # A file with no EXPECT directive is a SUPPORT MODULE (imported by a multi-file
        # claim's entry, e.g. Tier 3), not a claim. It is never run on its own.
        $skipped++; continue
    }
    $verb     = $directive.Groups[1].Value
    $expected = $directive.Groups[2].Value.Trim()

    $p = Start-Process -FilePath $Exe -ArgumentList "`"$($f.FullName)`"" -NoNewWindow -Wait -PassThru `
                       -RedirectStandardOutput $outFile -RedirectStandardError $errFile
    $code   = $p.ExitCode
    $stdout = (Get-Content $outFile -Raw); if ($null -eq $stdout) { $stdout = '' }
    $stderr = (Get-Content $errFile -Raw); if ($null -eq $stderr) { $stderr = '' }

    $ok = $false
    $why = ''
    switch ($verb) {
        'ok' {
            if ($code -ne 0)                     { $why = "expected exit 0, got $code; stderr: $($stderr.Trim())" }
            elseif ($stdout.Trim() -ne $expected){ $why = "stdout '$($stdout.Trim())' != expected '$expected'" }
            else                                 { $ok = $true }
        }
        'warn' {
            if ($code -ne 0)                     { $why = "expected exit 0 (warn), got $code" }
            elseif (-not $stderr.Contains($expected)) { $why = "stderr missing warning substring '$expected'" }
            else                                 { $ok = $true }
        }
        'fail' {
            if ($code -eq 0)                     { $why = "expected a rejection, but it compiled+ran (exit 0)" }
            elseif (-not $stderr.Contains($expected)) { $why = "stderr missing substring '$expected'; got: $($stderr.Trim())" }
            else                                 { $ok = $true }
        }
    }

    if ($ok) {
        $passed++
        Write-Host ("PASS  {0}  [{1}]" -f $rel, $verb) -ForegroundColor Green
    } else {
        $failed++; $failNames += $rel
        Write-Host ("FAIL  {0}  [{1}]  {2}" -f $rel, $verb, $why) -ForegroundColor Red
    }
}

Remove-Item $outFile, $errFile -ErrorAction SilentlyContinue

Write-Host ''
$claimCount = $passed + $failed
$skipNote = if ($skipped -gt 0) { "  ($skipped support module(s) skipped)" } else { '' }
Write-Host ("==== guide claims: {0} passed, {1} failed  ({2} claims){3} ====" -f $passed, $failed, $claimCount, $skipNote)
if ($failed -gt 0) {
    Write-Host 'Drifted claims:' -ForegroundColor Red
    $failNames | ForEach-Object { Write-Host "  - $_" -ForegroundColor Red }
}

$examplesFailed = $false
if ($runExamples) {
    Write-Host ''
    & (Join-Path $scriptDir 'run_guide_examples.ps1') -Exe $Exe
    $examplesFailed = ($LASTEXITCODE -ne 0)
    Write-Host ''
    & (Join-Path $scriptDir '..\run_examples.ps1') -Exe $Exe
    $examplesFailed = $examplesFailed -or ($LASTEXITCODE -ne 0)
    # The guides' own internal links. This check used to stand alone and was therefore easy to forget --
    # a renumbering of the sections once left six table-of-contents links pointing at headings that no
    # longer existed, and every routine gate stayed green, because the anchor checks that run routinely
    # read the documents under docs/, not the guides. It runs here so that "the one command is the whole
    # doc gate" is true of the links as well.
    foreach ($doc in @('SkarnGuide.md', 'SkarnIn30Minutes.md')) {
        Write-Host ''
        & (Join-Path $scriptDir 'check_anchors.ps1') -File (Join-Path $repoRoot $doc)
        $examplesFailed = $examplesFailed -or ($LASTEXITCODE -ne 0)
    }
}
if ($failed -gt 0 -or $examplesFailed) { exit 1 }
exit 0
