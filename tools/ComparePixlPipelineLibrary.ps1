<#
.SYNOPSIS
  Proves two PIXL pipeline libraries contain byte-identical bytecode for every
  stage they both hold. Identical bytecode means identical rendering.

.EXAMPLE
  .\ComparePixlPipelineLibrary.ps1 -Reference "<copy of the official cache>" -Candidate "<MO2 Overwrite>\PIXL\PipelineLibrary"
#>
[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$Reference,
      [Parameter(Mandatory = $true)][string]$Candidate)
$ErrorActionPreference = 'Stop'
$ref = (Resolve-Path -LiteralPath $Reference).Path.TrimEnd('\')
$cand = (Resolve-Path -LiteralPath $Candidate).Path.TrimEnd('\')

function Get-Stages([string]$root) {
    $map = @{}
    foreach ($f in Get-ChildItem -LiteralPath $root -Recurse -File -Filter '*.pixlbin') {
        $map[$f.FullName.Substring($root.Length + 1)] = $f.FullName
    }
    return $map
}
$r = Get-Stages $ref
$c = Get-Stages $cand
$same = 0; $different = @(); $onlyRef = 0; $onlyCand = 0
foreach ($key in $c.Keys) {
    if (-not $r.ContainsKey($key)) { ++$onlyCand; continue }
    if ((Get-FileHash -LiteralPath $r[$key]).Hash -eq (Get-FileHash -LiteralPath $c[$key]).Hash) { ++$same }
    else { $different += $key }
}
foreach ($key in $r.Keys) { if (-not $c.ContainsKey($key)) { ++$onlyRef } }

Write-Output ("Compared stages : {0}" -f ($same + $different.Count))
Write-Output ("Byte-identical  : {0}" -f $same)
Write-Output ("DIFFERENT       : {0}" -f $different.Count)
Write-Output ("Only in reference / only in candidate : {0} / {1}" -f $onlyRef, $onlyCand)
if ($different.Count -gt 0) {
    $different | Select-Object -First 20 | ForEach-Object { Write-Output "  differs: $_" }
    Write-Output 'FAIL: some stages differ.'
    exit 1
}
if ($same -eq 0) { Write-Output 'FAIL: no common stages to compare.'; exit 2 }
Write-Output 'PASS: every common stage is byte-identical.'
