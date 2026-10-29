$ErrorActionPreference = "Stop"
$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "../../..")).Path
$installer = Join-Path $repositoryRoot "exsample/CheckSig/Get-CheckSigProfile.ps1"
$temporary = Join-Path ([IO.Path]::GetTempPath()) ("CheckSigInstallerTest-" + [guid]::NewGuid().ToString("N"))
function Assert-True($Value, [string]$Message) {
    if (-not $Value) { throw $Message }
}
function Assert-Rejected([scriptblock]$Action, [string]$Message) {
    $rejected = $false
    try { & $Action | Out-Null } catch { $rejected = $true }
    Assert-True $rejected $Message
}
try {
    $destination = Join-Path $temporary "command"
    $local = Join-Path $repositoryRoot "data/1.26.60.5/windows-x64/manifest.json"
    $expected = Join-Path $destination "1.26.60.5-windows-x64.json"
    $messages = & $installer -Source Local -LocalPath $local -CommandDirectory $destination
    Assert-True (Test-Path -LiteralPath $expected) "Local JSON was not installed into command/"
    $localDocument = Get-Content -LiteralPath $expected -Raw -Encoding UTF8 | ConvertFrom-Json
    Assert-True ($localDocument.target.version -eq "1.26.60.5" -and $null -ne $localDocument.layouts) "Installed profile parts were not assembled"
    $expectedCount = @($localDocument.symbols.PSObject.Properties).Count
    Assert-True (($messages -join "`n") -match "with $expectedCount symbols") "Symbol count was not a scalar"
    Assert-Rejected { & $installer -Source Local -LocalPath $local -CommandsDirectory $destination } "Existing JSON was replaced without -Force"
    & $installer -Source Local -LocalPath $local -CommandsDirectory $destination -Force | Out-Null
    $folderDestination = Join-Path $temporary "from-folder"
    & $installer -Source Local -LocalPath (Join-Path $repositoryRoot "data") -CommandsDirectory $folderDestination -Version "1.26.52.3" | Out-Null
    Assert-True (Test-Path (Join-Path $folderDestination "1.26.52.3-windows-x64.json")) "Local directory version selection failed"
    $invalid = Join-Path $temporary "bad.json"
    Set-Content -LiteralPath $invalid -Value '{broken' -Encoding UTF8
    Assert-Rejected { & $installer -Source Local -LocalPath $invalid -CommandsDirectory $destination } "Malformed JSON was accepted"
    Assert-Rejected { & $installer -Source Local -LocalPath $local -Version "0.0.0.0" -CommandsDirectory $destination } "Wrong requested version was accepted"
    $remote = Join-Path $temporary "from-github"
    & $installer -Source GitHub -Version "1.26.60.5" -CommandsDirectory $remote | Out-Null
    # The transport authenticates the manifest and each part; installed output
    # is the assembled profile, not the manifest's original byte stream.
    $remoteDocument = Get-Content -LiteralPath (Join-Path $remote "1.26.60.5-windows-x64.json") -Raw -Encoding UTF8 | ConvertFrom-Json
    $remoteCanonical = $remoteDocument | ConvertTo-Json -Depth 100 -Compress
    $expectedCanonical = $localDocument | ConvertTo-Json -Depth 100 -Compress
    Assert-True ($remoteCanonical -ceq $expectedCanonical) "Assembled GitHub profile differs from class definitions"
    Write-Output "CheckSig profile installer tests passed (Local file/folder, GitHub, Force, invalid inputs)"
}
finally {
    if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Recurse -Force }
}
