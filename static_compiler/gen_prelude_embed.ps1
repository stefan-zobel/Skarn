# gen_prelude_embed.ps1 -- static_compiler pre-build codegen.
#
# Embeds the SINGLE-SOURCE stdlib (static_compiler/std/*.skn, ordered by std/modules.manifest)
# into prelude_embed.inc, which Compiler.cpp #includes as the initializer of the
# std::vector<PreludeModule> svc::builtin_prelude() returns. The std/ files are the ONLY source
# of truth: the baked-in prelude (used by the test suite and as skarnvm's fallback) can never
# drift from the editable files.
#
# The stdlib split: modules.manifest lists one `<prefix> <file>` per line, in EMBED ORDER (blank
# lines and `#` comments ignored; `<file>` is relative to the manifest's directory). This script
# reads each std/<file> and emits ONE `{ "<prefix>", <chunks> },` initializer entry per line. Each
# section's body is emitted as adjacent C++ raw-string chunks (MSVC caps a single string literal
# below the whole prelude), which the compiler concatenates into one source string.
#
# Writes the .inc only when its content changes (keeps incremental builds fast).
#
# Usage:  gen_prelude_embed.ps1 <modules.manifest> <out .inc>

param([Parameter(Mandatory=$true)][string]$Manifest,
      [Parameter(Mandatory=$true)][string]$Out)

$ErrorActionPreference = 'Stop'

$delim = 'SKARN_PRELUDE'
$stdDir = Split-Path -Parent $Manifest

$banner  = "// AUTO-GENERATED from static_compiler/std/*.skn (ordered by std/modules.manifest) by the"
$banner += " static_compiler pre-build (gen_prelude_embed.ps1). DO NOT EDIT -- edit the std/*.skn files instead."

# Append `<sectionText>` to $sb as adjacent ~4 KB raw-string chunks (split on line boundaries).
function Emit-Chunks {
    param([System.Text.StringBuilder]$sb, [string]$sectionText)
    $lines = $sectionText.Split("`n")
    $chunk = New-Object System.Text.StringBuilder
    $maxChunk = 4096
    for ($i = 0; $i -lt $lines.Length; $i++) {
        $line = $lines[$i]
        if ($i -lt $lines.Length - 1) { $line += "`n" }   # re-add the '\n' Split removed
        [void]$chunk.Append($line)
        if ($chunk.Length -ge $maxChunk -or $i -eq $lines.Length - 1) {
            [void]$sb.Append('R"').Append($delim).Append('(').Append($chunk.ToString()).Append(')').Append($delim).Append('"').Append("`n")
            $chunk = New-Object System.Text.StringBuilder
        }
    }
}

$sb = New-Object System.Text.StringBuilder
[void]$sb.Append($banner).Append("`n")

foreach ($raw in [System.IO.File]::ReadAllLines($Manifest)) {
    $line = $raw.Trim()
    if ($line.Length -eq 0 -or $line.StartsWith('#')) { continue }
    # `<prefix> <file>` -- prefix is the first whitespace token (may contain '$'/'::'), file the rest.
    $parts = $line -split '\s+', 2
    if ($parts.Length -ne 2) { throw "gen_prelude_embed: bad manifest line '$raw' (want '<prefix> <file>')" }
    $prefix = $parts[0]
    $file   = $parts[1].Trim()
    $path   = Join-Path $stdDir $file
    if (-not (Test-Path -LiteralPath $path)) { throw "gen_prelude_embed: manifest file not found: $path" }

    # ReadAllText auto-detects + strips a UTF-8 BOM. Normalize CRLF -> LF so the embedded string is
    # byte-for-byte what the lexer would otherwise get from the LF-newline literals.
    $text = [System.IO.File]::ReadAllText($path) -replace "`r`n", "`n"
    if ($text.Trim().Length -eq 0) { continue }   # a whitespace/comment-only module contributes nothing

    # A raw string ends at the first `)<delim>"`. Skarn source never contains that; fail loudly.
    if ($text.Contains(')' + $delim + '"')) {
        throw "gen_prelude_embed: $file contains the raw-string terminator )$delim`"; change `$delim in gen_prelude_embed.ps1"
    }

    [void]$sb.Append('{ "').Append($prefix).Append('",').Append("`n")
    Emit-Chunks $sb $text
    [void]$sb.Append("},`n")
}

$wrapped = $sb.ToString()

$existing = if (Test-Path -LiteralPath $Out) { [System.IO.File]::ReadAllText($Out) } else { $null }
# -cne, not -ne: PowerShell's -ne compares strings CASE-INSENSITIVELY, so an edit that only changed
# case (`Fn` -> `fn`) was reported "up to date" and never embedded.
if ($existing -cne $wrapped) {
    [System.IO.File]::WriteAllText($Out, $wrapped, (New-Object System.Text.UTF8Encoding($false)))
    Write-Host "gen_prelude_embed: wrote $Out"
} else {
    Write-Host "gen_prelude_embed: $Out up to date"
}
