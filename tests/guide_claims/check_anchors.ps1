# check_anchors.ps1 -- validate that every internal markdown anchor link resolves to a heading.
#
# The language guide (SkarnGuide.md) cross-references itself with `[text](#slug)` links. When a
# heading is renamed but a link is not, the link silently 404s a reader -- a blind spot no claim fixture can
# cover. This script mechanically checks every internal anchor: each `](#slug)`
# target must equal the GitHub-style slug of some heading in the same file. CI-gateable.
#
# Slug rule (GitHub): lowercase, drop every char that is not a letter/digit/space/hyphen, spaces -> hyphens;
# a duplicate slug is disambiguated with -1, -2, ... (the first occurrence stays bare). Fenced code blocks are
# skipped for BOTH headings and links, so a shell `# comment` inside ``` isn't mistaken for a heading (which
# would mask a genuinely broken link) and a `](#x)` shown as example code isn't checked as a real link.
#
# Usage (from a developer shell, any cwd):
#   powershell -File tests/guide_claims/check_anchors.ps1
#   powershell -File tests/guide_claims/check_anchors.ps1 -File path\to\doc.md
#
# Exit code: 0 = every internal anchor resolves; 1 = at least one is broken (or the file is missing).

param(
    [string]$File = ''
)

$ErrorActionPreference = 'Stop'

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot  = (Resolve-Path (Join-Path $scriptDir '..\..')).Path
if ([string]::IsNullOrEmpty($File)) { $File = Join-Path $repoRoot 'SkarnGuide.md' }
if (-not (Test-Path $File)) {
    Write-Host "ERROR: markdown file not found at '$File'." -ForegroundColor Red
    exit 1
}

function Get-Slug([string]$heading) {
    $s = $heading.Trim().ToLowerInvariant()
    $s = [regex]::Replace($s, '[^0-9a-z \-]', '')   # keep alnum, space, hyphen
    return ($s -replace ' ', '-')
}

$lines = Get-Content -LiteralPath $File

# Pass 1: heading slugs (fence-aware, with duplicate disambiguation).
$slugs = @{}
$headingCount = 0
$inFence = $false
foreach ($ln in $lines) {
    if ($ln -match '^\s*```') { $inFence = -not $inFence; continue }
    if ($inFence) { continue }
    $m = [regex]::Match($ln, '^#{1,6}\s+(.*)$')
    if (-not $m.Success) { continue }
    $headingCount++
    $slug = Get-Slug $m.Groups[1].Value
    if (-not $slugs.ContainsKey($slug)) {
        $slugs[$slug] = $true
    } else {
        $n = 1
        while ($slugs.ContainsKey("$slug-$n")) { $n++ }
        $slugs["$slug-$n"] = $true
    }
}

# Pass 2: internal link targets (fence-aware).
$targets = New-Object System.Collections.Generic.List[string]
$inFence = $false
foreach ($ln in $lines) {
    if ($ln -match '^\s*```') { $inFence = -not $inFence; continue }
    if ($inFence) { continue }
    foreach ($mm in [regex]::Matches($ln, '\]\(#([^)]+)\)')) {
        $targets.Add($mm.Groups[1].Value)
    }
}

$distinct = @($targets | Select-Object -Unique)
$broken   = @($distinct | Where-Object { -not $slugs.ContainsKey($_) })

$rel = if ($File.StartsWith($repoRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
           $File.Substring($repoRoot.Length).TrimStart('\')
       } else { $File }
Write-Host ("anchors: {0} headings, {1} internal links ({2} distinct targets) in {3}" -f `
            $headingCount, $targets.Count, $distinct.Count, $rel)

if ($broken.Count -gt 0) {
    Write-Host ("==== BROKEN: {0} anchor(s) resolve to no heading ====" -f $broken.Count) -ForegroundColor Red
    foreach ($b in ($broken | Sort-Object)) {
        $hit = $lines | Select-String -SimpleMatch "](#$b)" | Select-Object -First 1
        $where = if ($hit) { "first at line $($hit.LineNumber)" } else { "location unknown" }
        Write-Host ("  #{0}   ({1})" -f $b, $where) -ForegroundColor Red
    }
    exit 1
}

Write-Host "==== all internal anchors resolve ====" -ForegroundColor Green
exit 0
