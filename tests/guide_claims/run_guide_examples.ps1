# run_guide_examples.ps1 -- every ```rust block of the two guides, extracted and run.
#
# The guides are the single source of truth: this runner reads SkarnGuide.md and SkarnIn30Minutes.md at
# run time, turns every ```rust fence into a program, runs it with static_vmrun, and checks it against the
# annotations the reader sees. Nothing is copied into the repository, so an example cannot drift from its
# test. (The hand-written claim fixtures next to this script are run by run_guide_claims.ps1.)
#
# Fence info string -- ```rust followed by optional space-separated markers:
#   (none)          run it; exit 0; checked as described below
#   group=<name>    blocks of one guide with the same group are concatenated, in document order, into ONE
#                   program (a later example that continues an earlier one)
#   file=<path>     (needs group=) this block is not part of the program text but the module file <path>
#                   beside it, e.g. file=geo.skn for `import geo`
#   fail            the program must be rejected; see `// error:` below
#   check           type-check only (static_vmrun --dump-ast), never run -- for examples that need the
#                   network or another external resource
#   ignore=<reason> not a program (a fragment); reported, never run. <reason> has no spaces.
# An unknown marker, a group without a program block, or `fail` inside a group is a malformed fence.
#
# Annotations inside a block:
#   // => <line>        one expected stdout line. A block with at least one `// =>` must print EXACTLY
#                       those lines, in order. The annotation may stand alone on its own line (for a second
#                       output line of one statement) or follow code. Text after two or more spaces and a
#                       `(` is a note, not output: `// => 3   (integer division)`.
#   // error: <text>    (after code, `fail` blocks only) the checker must report an error whose caret is on
#                       THIS line and whose message contains <text> up to the first ` -- `, em dash or ` (`.
#                       Every error it reports must be annotated this way.
#   // warning: <text>  the same for a warning. Every warning a block produces must be annotated.
# A comment line that starts with `//` is commented-out code: annotations in it are not read (except a
# standalone `// =>` line).
#
# Usage:
#   powershell -File tests/guide_claims/run_guide_examples.ps1
#   ... -Config Debug | -Exe <static_vmrun.exe> | -Guide <file.md>[,<file.md>]
#   ... -Only <line>        run only the program containing the fence at (or the block around) that line
#   ... -Emit <dir>         also write every extracted program to <dir> (for debugging; not committed)
#
# Exit code: 0 = every example held; 1 = at least one failed or a fence was malformed.

param(
    [string]  $Exe        = '',
    [string]  $Config     = 'Release',
    [string[]]$Guide      = @(),
    [int]     $Only       = 0,
    [string]  $Emit       = '',
    [int]     $TimeoutSec = 60
)

$ErrorActionPreference = 'Stop'

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot  = (Resolve-Path (Join-Path $scriptDir '..\..')).Path

if ([string]::IsNullOrEmpty($Exe)) { $Exe = Join-Path $repoRoot ("x64\{0}\static_vmrun.exe" -f $Config) }
if (-not (Test-Path $Exe)) {
    Write-Host "ERROR: static_vmrun not found at '$Exe'. Build the solution (x64 $Config) first." -ForegroundColor Red
    exit 1
}
if ($Guide.Count -eq 0) {
    $Guide = @((Join-Path $repoRoot 'SkarnGuide.md'), (Join-Path $repoRoot 'SkarnIn30Minutes.md'))
}

$utf8 = New-Object System.Text.UTF8Encoding($false)
$emDash = [string][char]0x2014

# ---------------------------------------------------------------------------------------------------------
# Extraction

# Returns the ```rust blocks of one markdown file: Line (fence line), BodyStart, Body (string[]),
# Markers (hashtable), Problem (malformed-fence message or $null).
function Get-GuideBlocks([string]$path) {
    $lines  = [System.IO.File]::ReadAllLines($path, $utf8)
    $blocks = New-Object System.Collections.ArrayList
    $i = 0
    while ($i -lt $lines.Length) {
        $m = [regex]::Match($lines[$i], '^```(\S*)(.*)$')
        if (-not $m.Success) { $i++; continue }
        $lang = $m.Groups[1].Value
        $fenceLine = $i + 1
        $j = $i + 1
        while ($j -lt $lines.Length -and $lines[$j] -notmatch '^```\s*$') { $j++ }
        if ($lang -eq 'rust') {
            $markers = @{}
            $problem = $null
            foreach ($tok in ($m.Groups[2].Value.Trim() -split '\s+')) {
                if ($tok -eq '') { continue }
                $kv = $tok -split '=', 2
                $key = $kv[0]
                $val = if ($kv.Length -gt 1) { $kv[1] } else { $null }
                switch ($key) {
                    { $_ -in 'fail', 'check' } {
                        if ($null -ne $val) { $problem = "marker '$key' takes no value" }
                    }
                    { $_ -in 'group', 'file', 'ignore' } {
                        if ([string]::IsNullOrEmpty($val)) { $problem = "marker '$key' needs a value ($key=...)" }
                    }
                    default { $problem = "unknown marker '$tok'" }
                }
                $markers[$key] = $val
            }
            $body = if ($j -gt $i + 1) { $lines[($i + 1)..($j - 1)] } else { @() }
            [void]$blocks.Add([pscustomobject]@{
                Line = $fenceLine; BodyStart = $fenceLine + 1; Body = @($body); Markers = $markers; Problem = $problem
            })
        }
        $i = $j + 1
    }
    return $blocks
}

# Groups blocks into programs. A program has Blocks (the concatenated main text, in order), Files
# (file= blocks), a Mode (run/fail/check/ignore), a Label and a Problem.
function Get-Programs($blocks) {
    $programs = New-Object System.Collections.ArrayList
    $groups = [ordered]@{}
    foreach ($b in $blocks) {
        $g = $b.Markers['group']
        if ($null -eq $g) {
            if ($b.Markers.ContainsKey('file') -and $null -eq $b.Problem) { $b.Problem = "marker 'file=' needs 'group='" }
            [void]$programs.Add([pscustomobject]@{ Blocks = @($b); Files = @(); Group = $null; Line = $b.Line })
        } else {
            if (-not $groups.Contains($g)) {
                $groups[$g] = [pscustomobject]@{ Blocks = @(); Files = @(); Group = $g; Line = $b.Line }
                [void]$programs.Add($groups[$g])
            }
            if ($b.Markers.ContainsKey('file')) { $groups[$g].Files += $b } else { $groups[$g].Blocks += $b }
        }
    }
    foreach ($p in $programs) {
        $all = @($p.Blocks) + @($p.Files)
        $problem = ($all | Where-Object { $null -ne $_.Problem } | Select-Object -First 1)
        $p | Add-Member Problem $(if ($problem) { "fence at line $($problem.Line): $($problem.Problem)" } else { $null })
        $mode = 'run'
        foreach ($b in $all) {
            if ($b.Markers.ContainsKey('ignore')) { $mode = 'ignore'; $p | Add-Member Reason $b.Markers['ignore'] -Force }
            elseif ($b.Markers.ContainsKey('check') -and $mode -ne 'ignore') { $mode = 'check' }
            elseif ($b.Markers.ContainsKey('fail') -and $mode -eq 'run') { $mode = 'fail' }
        }
        $p | Add-Member Mode $mode
        if ($null -eq $p.Problem) {
            if ($p.Blocks.Count -eq 0) { $p.Problem = "group '$($p.Group)' has only file= blocks, no program" }
            elseif ($null -ne $p.Group -and $mode -eq 'fail') { $p.Problem = "group '$($p.Group)': 'fail' is not supported inside a group" }
        }
    }
    return $programs
}

# ---------------------------------------------------------------------------------------------------------
# Annotations

$reOutput = '^(?<code>.*?)//\s*=>\s?(?<text>.*)$'
$reDiag   = '^(?<code>.*?)//\s*(?<kind>error|warning):\s*(?<text>.*)$'

function Test-IsCommentedOut([string]$code) { return $code.TrimStart().StartsWith('//') }

function Get-NoteStripped([string]$text) { return ([regex]::Replace($text, '\s{2,}\(.*$', '')).Trim() }

function Get-DiagKey([string]$text) {
    $t = $text
    foreach ($sep in @(' -- ', " $emDash", ' (')) {
        $k = $t.IndexOf($sep)
        if ($k -ge 0) { $t = $t.Substring(0, $k) }
    }
    return $t.Trim().TrimEnd('-').Trim()
}

# Parses `error:` / `warning:` diagnostics from stderr: Kind, Message, Line (first caret excerpt, 0 if none).
function Get-Diagnostics([string]$stderr) {
    $diags = New-Object System.Collections.ArrayList
    $cur = $null
    foreach ($ln in ($stderr -split "`r?`n")) {
        $h = [regex]::Match($ln, '^(error|warning):\s*(.*)$')
        if ($h.Success) {
            $cur = [pscustomobject]@{ Kind = $h.Groups[1].Value; Message = $h.Groups[2].Value; Line = 0 }
            [void]$diags.Add($cur)
            continue
        }
        if ($null -ne $cur -and $cur.Line -eq 0) {
            $x = [regex]::Match($ln, '^\s*(\d+) \| ')
            if ($x.Success) { $cur.Line = [int]$x.Groups[1].Value }
        }
    }
    return $diags
}

# ---------------------------------------------------------------------------------------------------------
# Running

function Invoke-Driver([string]$workDir, [string[]]$arguments) {
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
        return [pscustomobject]@{ Code = -1; Out = ''; Err = "timed out after $TimeoutSec s"; TimedOut = $true }
    }
    $proc.WaitForExit()
    return [pscustomobject]@{ Code = $proc.ExitCode; Out = $outTask.Result; Err = $errTask.Result; TimedOut = $false }
}

function Write-Program($prog, [string]$dir) {
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $text = New-Object System.Collections.ArrayList
    $map = New-Object System.Collections.ArrayList       # program line (index+1) -> block, guide line
    foreach ($b in $prog.Blocks) {
        for ($k = 0; $k -lt $b.Body.Count; $k++) {
            [void]$text.Add($b.Body[$k])
            [void]$map.Add([pscustomobject]@{ Block = $b; GuideLine = $b.BodyStart + $k })
        }
    }
    [System.IO.File]::WriteAllText((Join-Path $dir 'main.skn'), (($text -join "`n") + "`n"), $utf8)
    foreach ($f in $prog.Files) {
        $target = Join-Path $dir $f.Markers['file']
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
        [System.IO.File]::WriteAllText($target, (($f.Body -join "`n") + "`n"), $utf8)
    }
    return [pscustomobject]@{ Lines = $text; Map = $map }
}

# Checks one program; returns a list of failure reasons (empty = passed).
function Test-Program($prog, [string]$dir, [string]$guideName) {
    $why = New-Object System.Collections.ArrayList
    $src = Write-Program $prog $dir
    foreach ($f in $prog.Files) {
        foreach ($ln in $f.Body) {
            $o = [regex]::Match($ln, $reOutput)
            if ($o.Success -and -not (Test-IsCommentedOut $o.Groups['code'].Value)) {
                [void]$why.Add("module file '$($f.Markers['file'])' carries a '// =>' annotation; output belongs to the program block")
                break
            }
        }
    }

    $expected = New-Object System.Collections.ArrayList
    $wantDiags = New-Object System.Collections.ArrayList
    for ($k = 0; $k -lt $src.Lines.Count; $k++) {
        $ln = $src.Lines[$k]
        $o = [regex]::Match($ln, $reOutput)
        if ($o.Success -and ($o.Groups['code'].Value.Trim() -eq '' -or -not (Test-IsCommentedOut $o.Groups['code'].Value))) {
            [void]$expected.Add((Get-NoteStripped $o.Groups['text'].Value))
        }
        $d = [regex]::Match($ln, $reDiag)
        if ($d.Success -and $d.Groups['code'].Value.Trim() -ne '' -and -not (Test-IsCommentedOut $d.Groups['code'].Value)) {
            [void]$wantDiags.Add([pscustomobject]@{
                Kind = $d.Groups['kind'].Value; Key = (Get-DiagKey $d.Groups['text'].Value); Line = $k + 1
                GuideLine = $src.Map[$k].GuideLine; Matched = $false })
        }
    }

    $wantErrors = @($wantDiags | Where-Object { $_.Kind -eq 'error' })
    if ($prog.Mode -eq 'fail') {
        if ($wantErrors.Count -eq 0) { [void]$why.Add("marked 'fail' but no line carries a '// error:' annotation") }
        if ($expected.Count -gt 0)   { [void]$why.Add("a 'fail' block must not carry '// =>' annotations") }
    } elseif ($wantErrors.Count -gt 0) {
        [void]$why.Add("line $($wantErrors[0].GuideLine) carries '// error:' but the fence is not marked 'fail'")
    }
    if ($why.Count -gt 0) { return $why }

    $drvArgs = if ($prog.Mode -eq 'check') { @('--dump-ast', 'main.skn') } else { @('main.skn') }
    $r = Invoke-Driver $dir $drvArgs
    if ($r.TimedOut) { [void]$why.Add($r.Err); return $why }

    $diags = Get-Diagnostics $r.Err
    foreach ($dg in $diags) {
        $hit = $wantDiags | Where-Object { -not $_.Matched -and $_.Kind -eq $dg.Kind -and $_.Line -eq $dg.Line -and
                                           $dg.Message.Contains($_.Key) } | Select-Object -First 1
        if ($null -ne $hit) { $hit.Matched = $true; continue }
        $at = if ($dg.Line -gt 0 -and $dg.Line -le $src.Map.Count) { "line $($src.Map[$dg.Line - 1].GuideLine)" } else { 'no line' }
        [void]$why.Add("unannotated $($dg.Kind) ($at): $($dg.Message)")
    }
    foreach ($w in ($wantDiags | Where-Object { -not $_.Matched })) {
        [void]$why.Add("line $($w.GuideLine): expected $($w.Kind) '$($w.Key)' on this line, none reported")
    }

    if ($prog.Mode -eq 'fail') {
        if ($r.Code -eq 0) { [void]$why.Add('expected a compile rejection, but the program ran (exit 0)') }
        return $why
    }
    if ($r.Code -ne 0) {
        if (-not ($diags | Where-Object { $_.Kind -eq 'error' })) {
            [void]$why.Add("exit $($r.Code): $(($r.Err.Trim() -split "`r?`n") | Select-Object -First 3)")
        }
        return $why
    }
    if ($prog.Mode -eq 'check' -or $expected.Count -eq 0) { return $why }

    $actual = @(($r.Out -replace "`r`n", "`n") -split "`n" | ForEach-Object { $_.Trim() })
    $n = $actual.Count
    while ($n -gt 0 -and $actual[$n - 1] -eq '') { $n-- }
    $actual = @(if ($n -gt 0) { $actual[0..($n - 1)] })
    $max = [Math]::Max($expected.Count, $actual.Count)
    for ($k = 0; $k -lt $max; $k++) {
        $e = if ($k -lt $expected.Count) { $expected[$k] } else { $null }
        $a = if ($k -lt $actual.Count)   { $actual[$k] }   else { $null }
        if ($e -ne $a) {
            if ($null -eq $e)     { [void]$why.Add("output line $($k + 1) '$a' has no '// =>' annotation") }
            elseif ($null -eq $a) { [void]$why.Add("expected output line $($k + 1) '$e' was not printed") }
            else                  { [void]$why.Add("output line $($k + 1): expected '$e', got '$a'") }
            break
        }
    }
    return $why
}

# ---------------------------------------------------------------------------------------------------------
# Main

$workRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('skarn_guide_examples_' + [guid]::NewGuid().ToString('N'))
$passed = 0; $failed = 0; $checked = 0; $ignored = 0
$failNames = @()
$ignoredNames = @()

foreach ($g in $Guide) {
    $guidePath = (Resolve-Path $g).Path
    $guideName = Split-Path -Leaf $guidePath
    $stem = [System.IO.Path]::GetFileNameWithoutExtension($guideName)
    $programs = Get-Programs (Get-GuideBlocks $guidePath)
    foreach ($p in $programs) {
        $lines = @($p.Blocks + $p.Files | ForEach-Object { $_.Line }) | Sort-Object
        if ($Only -gt 0) {
            $inside = $p.Blocks + $p.Files | Where-Object { $Only -ge $_.Line -and $Only -le $_.BodyStart + $_.Body.Count }
            if (-not $inside) { continue }
        }
        $label = "{0}:{1}" -f $guideName, ($lines -join '+')
        if ($null -ne $p.Group) { $label += " [group=$($p.Group)]" }

        if ($null -ne $p.Problem) {
            $failed++; $failNames += $label
            Write-Host ("FAIL  {0}  malformed: {1}" -f $label, $p.Problem) -ForegroundColor Red
            continue
        }
        if ($p.Mode -eq 'ignore') {
            $ignored++; $ignoredNames += "$label ($($p.Reason))"
            Write-Host ("SKIP  {0}  ignore={1}" -f $label, $p.Reason) -ForegroundColor Yellow
            continue
        }

        $dirName = "{0}_L{1:D4}" -f $stem, $p.Line
        $dir = Join-Path $workRoot $dirName
        $why = Test-Program $p $dir $guideName
        if (-not [string]::IsNullOrEmpty($Emit)) {
            $dest = Join-Path $Emit $dirName
            New-Item -ItemType Directory -Force -Path $dest | Out-Null
            Copy-Item -Path (Join-Path $dir '*') -Destination $dest -Recurse -Force
        }
        if ($why.Count -eq 0) {
            if ($p.Mode -eq 'check') { $checked++ } else { $passed++ }
            Write-Host ("PASS  {0}  [{1}]" -f $label, $p.Mode) -ForegroundColor Green
        } else {
            $failed++; $failNames += $label
            Write-Host ("FAIL  {0}  [{1}]" -f $label, $p.Mode) -ForegroundColor Red
            foreach ($w in $why) { Write-Host "        $w" -ForegroundColor Red }
        }
    }
}

Remove-Item -Recurse -Force $workRoot -ErrorAction SilentlyContinue

Write-Host ''
Write-Host ("==== guide examples: {0} passed, {1} check-only, {2} ignored, {3} failed ====" -f $passed, $checked, $ignored, $failed)
if ($ignored -gt 0) { $ignoredNames | ForEach-Object { Write-Host "  ignored: $_" -ForegroundColor Yellow } }
if ($passed + $checked + $ignored + $failed -eq 0) {
    $what = if ($Only -gt 0) { 'no ```rust block contains line ' + $Only } else { 'no ```rust blocks found' }
    Write-Host "ERROR: $what." -ForegroundColor Red
    exit 1
}
if ($failed -gt 0) {
    Write-Host 'Failed examples:' -ForegroundColor Red
    $failNames | ForEach-Object { Write-Host "  - $_" -ForegroundColor Red }
    exit 1
}
exit 0
