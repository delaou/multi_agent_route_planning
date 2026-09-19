param([switch]$Stop)
$ErrorActionPreference = 'Stop'
$nodeCommand = Get-Command node.exe -ErrorAction SilentlyContinue
$nodePath = if ($nodeCommand) { $nodeCommand.Source } else { $null }
if (-not $nodePath) {
    $bundledNode = Join-Path $env:USERPROFILE '.cache\codex-runtimes\codex-primary-runtime\dependencies\node\bin\node.exe'
    if (Test-Path -LiteralPath $bundledNode) { $nodePath = $bundledNode }
}
if (-not $nodePath) { throw 'Node.js 18+ is required. Install Node.js or put node.exe on PATH.' }
$launcher = Join-Path $PSScriptRoot 'local-launcher.cjs'
if ($Stop) { & $nodePath $launcher --stop } else { & $nodePath $launcher }
exit $LASTEXITCODE
