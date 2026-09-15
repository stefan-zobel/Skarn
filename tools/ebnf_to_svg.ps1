# =============================================================================
# ebnf_to_svg.ps1 -- render an EBNF grammar file to a syntax-highlighted SVG.
#
# Usage:  powershell -ExecutionPolicy Bypass -File tools\ebnf_to_svg.ps1
#         (reads docs\skarn_grammar.ebnf, writes docs\skarn_grammar.svg)
#
# No external dependencies -- a small hand-rolled tokenizer colours comments,
# terminal strings, nonterminals, the defined rule name, and EBNF operators.
# Re-run after editing the grammar to regenerate the image.
# =============================================================================

param(
    [string]$In  = "docs/skarn_grammar.ebnf",
    [string]$Out = "docs/skarn_grammar.svg"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$inPath  = Join-Path $root $In
$outPath = Join-Path $root $Out

$lines = Get-Content -LiteralPath $inPath

function Esc([string]$s) {
    $s = $s -replace '&','&amp;'
    $s = $s -replace '<','&lt;'
    $s = $s -replace '>','&gt;'
    return $s
}

$tok = [regex]'(?<c>\(\*.*?\*\))|(?<t>"[^"]*"|''[^'']*'')|(?<n>[A-Za-z_][A-Za-z0-9_]*)|(?<o>[=|;,{}\[\]().])|(?<s>[ \t]+)|(?<x>.)'

# layout metrics
$charW  = 8.0
$lineH  = 19.0
$padX   = 40.0
$top    = 78.0
$maxLen = ($lines | Measure-Object -Property Length -Maximum).Maximum
$width  = [int]($padX * 2 + $maxLen * $charW)
$height = [int]($top + $lines.Count * $lineH + 26)

$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine("<svg xmlns='http://www.w3.org/2000/svg' width='$width' height='$height' viewBox='0 0 $width $height' font-family='Consolas, ""DejaVu Sans Mono"", monospace'>")
[void]$sb.AppendLine("  <defs><style>")
[void]$sb.AppendLine("    .bg{fill:#0f172a} .card{fill:#111827} .title{fill:#e2e8f0;font-size:20px;font-weight:bold} .sub{fill:#64748b;font-size:12px}")
[void]$sb.AppendLine("    text.code{fill:#cbd5e1;font-size:14px} .c{fill:#6b7280;font-style:italic} .t{fill:#34d399} .n{fill:#60a5fa} .o{fill:#94a3b8} .d{fill:#fbbf24;font-weight:bold}")
[void]$sb.AppendLine("  </style></defs>")
[void]$sb.AppendLine("  <rect class='bg' x='0' y='0' width='$width' height='$height'/>")
[void]$sb.AppendLine("  <rect class='card' x='14' y='14' width='$($width-28)' height='$($height-28)' rx='10'/>")
[void]$sb.AppendLine("  <text class='title' x='$padX' y='42'>Skarn &#8212; EBNF Grammar</text>")
[void]$sb.AppendLine("  <text class='sub' x='$padX' y='60'>Rust-family syntax, sound static types &#183; generated from docs/skarn_grammar.ebnf</text>")

for ($i = 0; $i -lt $lines.Count; $i++) {
    $line = $lines[$i]
    $y = [int]($top + $i * $lineH + 12)
    if ($line.Trim().Length -eq 0) { continue }

    $isRule = $line -match '^\s*[A-Za-z_][A-Za-z0-9_]*\s*='
    $seenEq = $false
    $row = New-Object System.Text.StringBuilder
    [void]$row.Append("  <text class='code' x='$padX' y='$y' xml:space='preserve'>")

    foreach ($m in $tok.Matches($line)) {
        if ($m.Groups['c'].Success) {
            [void]$row.Append("<tspan class='c'>$(Esc $m.Value)</tspan>")
        } elseif ($m.Groups['t'].Success) {
            [void]$row.Append("<tspan class='t'>$(Esc $m.Value)</tspan>")
        } elseif ($m.Groups['n'].Success) {
            if ($isRule -and -not $seenEq) {
                [void]$row.Append("<tspan class='d'>$(Esc $m.Value)</tspan>")
            } else {
                [void]$row.Append("<tspan class='n'>$(Esc $m.Value)</tspan>")
            }
        } elseif ($m.Groups['o'].Success) {
            if ($m.Value -eq '=') { $seenEq = $true }
            [void]$row.Append("<tspan class='o'>$(Esc $m.Value)</tspan>")
        } else {
            [void]$row.Append((Esc $m.Value))
        }
    }
    [void]$row.Append("</text>")
    [void]$sb.AppendLine($row.ToString())
}

[void]$sb.AppendLine("</svg>")

[System.IO.File]::WriteAllText($outPath, $sb.ToString(), (New-Object System.Text.UTF8Encoding($false)))   # UTF-8 without BOM
Write-Host "wrote $outPath ($width x $height, $($lines.Count) lines)"
