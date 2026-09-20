# check_doc_anchors.ps1 -- validate that every section NAME cited from source and docs still exists.
#
# Source comments and documents cite sections of the architecture documents by name, not by markdown
# anchor:
#     // See "Native functions" in docs/VirtualMachine.md.
#     // GUIDE: docs/Compiler.md "No truthiness"
# Renaming or moving a heading is invisible to every other check, so such a citation would rot silently.
# This script collects every citable target of the document family -- headings AND bold paragraph leads
# (`**Name**`), since documents are often cited by those -- and checks every citation it finds.
#
# What a CITATION is. A quoted name on a line that mentions one of the documents, where a TRIGGER sits just
# before the quote with citation-shaped text between: the document name itself (`Compiler.md "X"`) or a
# continuation word (`see "X"`, `under "X"`, `section "X"`). Anything else in quotes on such a line -- an
# error message, a grep pattern -- is not a citation.
#
# Matching is deliberately generous: names are normalized (lowercase, every run of non-alphanumerics -> one
# space), and match strength scales with the citation's length. ONE word must match a target exactly, TWO
# words may match a prefix, THREE or more may match anywhere inside the target. That lets an ASCII comment
# cite an em-dashed heading, and a short citation name a longer heading, without a bare "Dispatch" resolving
# by accident against an unrelated target.
#
# Usage (from a developer shell, any cwd):
#   powershell -File tests/check_doc_anchors.ps1
#   powershell -File tests/check_doc_anchors.ps1 -Docs docs\Compiler.md -Trigger 'Compiler\.md'
#
# Parameters (all optional; the defaults check docs/VirtualMachine.md, docs/Compiler.md and
# docs/LanguageServer.md):
#   -Docs               repo-relative paths of the document family (missing files are skipped)
#   -Trigger            regex a line must match to be scanned; also the primary citation trigger
#   -ExtraDir           an additional directory of *.md files to scan for citations
#   -RejectPartNumbers  reject `Part I/II/III` references next to the trigger
#   -Label              the name printed in the summary
#
# Exit code: 0 = every citation resolves; 1 = at least one does not (or no document was found).

param(
    [string[]]$Docs = @('docs\VirtualMachine.md', 'docs\Compiler.md', 'docs\LanguageServer.md'),
    [string]  $Trigger = '(VirtualMachine|Compiler|LanguageServer)\.md',
    [string]  $ExtraDir = '',
    [switch]  $RejectPartNumbers,
    [string]  $Label = 'doc anchors'
)

$ErrorActionPreference = 'Stop'

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot  = (Resolve-Path (Join-Path $scriptDir '..')).Path

# ---- the document family ---------------------------------------------------------------------------
$family = @()
foreach ($rel in $Docs) {
    $p = Join-Path $repoRoot $rel
    if (Test-Path $p) { $family += $p }
}
if ($family.Count -eq 0) {
    Write-Host "ERROR: none of the documents ($($Docs -join ', ')) exists under '$repoRoot'." -ForegroundColor Red
    exit 1
}

function Get-Norm([string]$s) {
    $t = $s.Trim().ToLowerInvariant()
    $t = [regex]::Replace($t, '[^0-9a-z]+', ' ')
    return $t.Trim()
}

# Pass 1: every citable TARGET -- headings and bold paragraph leads.
$headings = New-Object System.Collections.Generic.List[string]
$boldLeads = New-Object System.Collections.Generic.List[string]
foreach ($f in $family) {
    $inFence = $false
    foreach ($ln in (Get-Content -LiteralPath $f -Encoding UTF8)) {
        if ($ln -match '^\s*```') { $inFence = -not $inFence; continue }
        if ($inFence) { continue }
        $m = [regex]::Match($ln, '^#{1,6}\s+(.*)$')
        if ($m.Success) { $headings.Add((Get-Norm $m.Groups[1].Value)); continue }
        foreach ($bm in [regex]::Matches($ln, '\*\*(.+?)\*\*')) {
            $b = Get-Norm $bm.Groups[1].Value
            if ($b.Length -ge 3) { $boldLeads.Add($b) }
        }
    }
}

function Test-Against($list, [string]$n, [int]$words) {
    foreach ($h in $list) {
        if     ($words -eq 1) { if ($h -eq $n) { return $true } }
        elseif ($words -eq 2) { if ($h -eq $n -or $h.StartsWith($n)) { return $true } }
        elseif ($h.Contains($n)) { return $true }
    }
    return $false
}

# Returns 'heading', 'bold', or '' (unresolved).
function Resolve-Citation([string]$cite) {
    $n = Get-Norm $cite
    if ($n.Length -eq 0) { return 'heading' }
    $words = ([regex]::Matches($n, '[0-9a-z]+')).Count
    if (Test-Against $headings  $n $words) { return 'heading' }
    if (Test-Against $boldLeads $n $words) { return 'bold' }
    return ''
}

# The text between a trigger and the opening quote must be citation-shaped: punctuation, a section sign, a
# possessive, a path prefix -- and no ordinary word except the few that really do introduce a citation.
function Test-Connector([string]$between) {
    if ($between.Length -gt 30) { return $false }
    $words = [regex]::Matches($between.ToLowerInvariant(), '[a-z]+')
    foreach ($w in $words) {
        if (@('s','part','under','section','in','of','the','and','see','i','ii','iii') -notcontains $w.Value) {
            return $false
        }
    }
    return $true
}

# Pass 2: collect citing lines from source, project files, documents and the optional extra directory.
$scan = New-Object System.Collections.Generic.List[string]
$exts = @('.h', '.cpp', '.md', '.skn', '.ebnf', '.vcxproj')
Get-ChildItem -Path $repoRoot -Recurse -File | Where-Object {
    $exts -contains $_.Extension -and
    $_.FullName -notmatch '\\x64\\' -and $_.FullName -notmatch '\\\.git\\'
} | ForEach-Object { $scan.Add($_.FullName) }
if ($ExtraDir -and (Test-Path $ExtraDir)) {
    Get-ChildItem -Path $ExtraDir -File -Filter *.md | ForEach-Object { $scan.Add($_.FullName) }
}

$enc = [System.Text.UTF8Encoding]::new($false)
$bad = New-Object System.Collections.Generic.List[string]
$citations = 0
$sites = 0
$viaBold = 0

foreach ($f in $scan) {
    $lines = [System.IO.File]::ReadAllLines($f, $enc)
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $ln = $lines[$i]
        if ($ln -notmatch $Trigger) { continue }
        $sites++
        $rel = $f
        if ($f.StartsWith($repoRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
            $rel = $f.Substring($repoRoot.Length).TrimStart('\')
        } elseif ($ExtraDir -and $f.StartsWith($ExtraDir, [System.StringComparison]::OrdinalIgnoreCase)) {
            $rel = 'extra\' + (Split-Path -Leaf $f)
        }
        if ($RejectPartNumbers -and
            ($ln -match ('(' + $Trigger + ')[^.]{0,40}\bPart\s+(I|II|III)\b') -or
             $ln -match ('\bPart\s+(I|II|III)\b[^.]{0,40}(' + $Trigger + ')'))) {
            $bad.Add(("{0}:{1}  [Part number]  {2}" -f $rel, ($i + 1), $ln.Trim()))
        }
        foreach ($mm in [regex]::Matches($ln, '"([^"]{2,80})"')) {
            $cite = $mm.Groups[1].Value
            if ($cite -notmatch '[A-Za-z]') { continue }        # not a name
            if ($cite -match $Trigger) { continue }             # the quote contains the document name
            $head = $ln.Substring(0, $mm.Index)
            $isCite = $false
            foreach ($tm in [regex]::Matches($head, $Trigger + '|(?i)\bunder\b|\bsection\b|\bsee\b')) {
                $end = $tm.Index + $tm.Length
                if ($end -le $mm.Index -and (Test-Connector $head.Substring($end))) { $isCite = $true }
            }
            if (-not $isCite) { continue }
            $citations++
            $kind = Resolve-Citation $cite
            if ($kind -eq '')         { $bad.Add(("{0}:{1}  `"{2}`"" -f $rel, ($i + 1), $cite)) }
            elseif ($kind -eq 'bold') { $viaBold++ }
        }
    }
}

Write-Host ("{0}: {1} heading(s) + {2} bold lead(s) across {3} file(s); {4} citing line(s), {5} named citation(s), {6} resolved only via a bold lead" -f `
            $Label, $headings.Count, $boldLeads.Count, $family.Count, $sites, $citations, $viaBold)

if ($bad.Count -gt 0) {
    Write-Host ("==== BROKEN: {0} citation(s) resolve to no heading ====" -f $bad.Count) -ForegroundColor Red
    foreach ($b in ($bad | Sort-Object)) { Write-Host ("  " + $b) -ForegroundColor Red }
    exit 1
}

Write-Host "==== every cited section resolves ====" -ForegroundColor Green
exit 0
