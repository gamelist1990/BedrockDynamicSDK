param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("analyze", "search", "function", "decompile", "xrefs")]
    [string]$Operation,

    [Parameter(Mandatory = $true)]
    [string]$Target,

    [string]$Focus,
    [string]$EvidenceDirectory
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $Target -PathType Leaf)) {
    throw "Target executable not found: $Target"
}

if (-not $EvidenceDirectory) {
    $EvidenceDirectory = Join-Path $HOME "Documents/PEXData/REA"
}
New-Item -ItemType Directory -Force -Path $EvidenceDirectory | Out-Null

$targetName = [IO.Path]::GetFileNameWithoutExtension($Target)
$targetHash = (Get-FileHash -LiteralPath $Target -Algorithm SHA256).Hash.ToLowerInvariant().Substring(0, 16)
$targetIdentity = $targetName + "-" + $targetHash
$snapshot = Join-Path $EvidenceDirectory ($targetIdentity + ".rea-snapshot.json")
$safeFocus = if ($Focus) { $Focus -replace '[^A-Za-z0-9._-]', '_' } else { "overview" }
$evidencePath = Join-Path $EvidenceDirectory ($targetIdentity + "-" + $Operation + "-" + $safeFocus + ".json")

$command = Get-Command rea -ErrorAction SilentlyContinue
if ($command) {
    $launcher = $command.Source
    $prefix = @()
} else {
    $command = Get-Command npx -ErrorAction Stop
    $launcher = $command.Source
    $prefix = @("--yes", "rea-agents@latest")
}

$arguments = @($Operation, (Resolve-Path -LiteralPath $Target).Path)
switch ($Operation) {
    "search" {
        if (-not $Focus) { throw "-Focus is required for search (string or procedure name)." }
        $arguments += @($Focus, "--kind", "strings")
    }
    "function" {
        if (-not $Focus) { throw "-Focus is required for function (name or address)." }
        $arguments += $Focus
    }
    "decompile" {
        if (-not $Focus) { throw "-Focus is required for decompile (name or address)." }
        $arguments += $Focus
    }
    "xrefs" {
        if (-not $Focus) { throw "-Focus is required for xrefs (name or address)." }
        $arguments += $Focus
    }
}
$arguments += @("--provider", "ghidra", "--snapshot", $snapshot, "--format", "json")

$output = & $launcher @prefix @arguments
if ($LASTEXITCODE -ne 0) {
    throw "REA exited with code $LASTEXITCODE. See the diagnostic output above."
}

$jsonText = ($output -join [Environment]::NewLine)
[IO.File]::WriteAllText($evidencePath, $jsonText, [Text.UTF8Encoding]::new($false))
Write-Output "Evidence: $evidencePath"
