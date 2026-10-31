[CmdletBinding()]
param(
    [ValidateSet("GitHub", "Local")]
    [string]$Source = "GitHub",

    [string]$LocalPath,

    [string]$Version,

    [Alias("CommandDirectory")]
    [string]$CommandsDirectory,

    # Optional immutable revision for reproducible profile installation.
    [ValidatePattern('^(main|[0-9a-fA-F]{40})$')]
    [string]$GitHubRef = "main",

    [switch]$Force
)

$ErrorActionPreference = "Stop"

$owner = "gamelist1990"
$repository = "BedrockDynamicSDK"
$branch = $GitHubRef
$maxProfileBytes = 16777216

function Expand-ProfileParts {
    param([string]$Path, [string]$RemotePrefix = "")
    $descriptor = Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json
    if (-not ($descriptor.PSObject.Properties.Name -contains 'format') -or $descriptor.format -ne 'bedrock-profile-parts') { return $Path }
    if (@($descriptor.parts).Count -gt 256) { throw "Profile part count limit exceeded" }
    $root = [ordered]@{ schema = $descriptor.schema; target = $descriptor.target }
    if ($descriptor.PSObject.Properties.Name -contains 'protocol') { $root.protocol = $descriptor.protocol }
    $base = Split-Path -Parent $Path
    $total = 0
    foreach ($entry in $descriptor.parts) {
        if ([string]$entry.path -notmatch '^(classes|analysis|metadata)/[A-Za-z0-9_.-]+\.json$' -or
            [string]$entry.sha256 -notmatch '^[a-f0-9]{64}$') { throw "Invalid profile part path/hash" }
        if ([string]::IsNullOrEmpty($RemotePrefix)) {
            $partPath = Join-Path $base ([string]$entry.path)
        } else {
            $partPath = Join-Path $workDirectory (([guid]::NewGuid().ToString('N')) + '.json')
            [void](Invoke-WebRequest -Uri ($RemotePrefix + '/' + $entry.path) -OutFile $partPath -UseBasicParsing)
        }
        $size = (Get-Item -LiteralPath $partPath).Length
        $total += $size
        if ($size -gt 4MB -or $total -gt 16MB) { throw "Profile part byte limit exceeded" }
        if ((Get-FileHash -LiteralPath $partPath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $entry.sha256) {
            throw "Profile part checksum mismatch: $($entry.path)"
        }
        $part = Get-Content -LiteralPath $partPath -Raw -Encoding UTF8 | ConvertFrom-Json
        foreach ($property in $part.PSObject.Properties) {
            $key = $property.Name
            if ($key -in @('symbols','layouts','virtual_methods','analysis')) {
                if (-not $root.Contains($key)) { $root[$key] = [ordered]@{} }
                foreach ($item in $property.Value.PSObject.Properties) {
                    if ($root[$key].Contains($item.Name)) { throw "Duplicate profile definition: $key/$($item.Name)" }
                    $root[$key][$item.Name] = $item.Value
                }
            } else {
                if ($root.Contains($key)) { throw "Duplicate profile metadata: $key" }
                $root[$key] = $property.Value
            }
        }
    }
    $assembled = Join-Path $workDirectory ('assembled-' + [guid]::NewGuid().ToString('N') + '.json')
    [System.IO.File]::WriteAllText($assembled, ($root | ConvertTo-Json -Depth 100), (New-Object System.Text.UTF8Encoding($false)))
    return $assembled
}

function Get-ProfileMetadata {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $Path = Expand-ProfileParts -Path $Path
    $item = Get-Item -LiteralPath $Path
    if ($item.Length -le 0 -or $item.Length -gt $maxProfileBytes) {
        throw "Profile size must be between 1 byte and 16 MiB: $Path"
    }

    $document = Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($document -isnot [pscustomobject] -or
        $document.target -isnot [pscustomobject] -or
        $document.symbols -isnot [pscustomobject]) {
        throw "Profile must contain target and symbols objects: $Path"
    }

    $target = $document.target
    if ($target.product -ne "minecraft-bedrock" -or
        $target.platform -ne "windows" -or $target.arch -ne "x64") {
        throw "Profile target must be Minecraft Bedrock Windows x64: $Path"
    }
    if ([string]$target.version -notmatch '^\d+(\.\d+){1,3}$') {
        throw "Profile target.version is missing or invalid: $Path"
    }
    if ([string]$target.sha256 -notmatch '^[0-9a-fA-F]{64}$') {
        throw "Profile target.sha256 must be a 64-character SHA-256: $Path"
    }
    $symbolCount = @($document.symbols.PSObject.Properties).Count
    if ($document.schema -ne 1 -or $symbolCount -lt 1 -or $symbolCount -gt 20000) {
        throw "Profile must use schema 1 and contain 1..20000 symbols: $Path"
    }

    return [pscustomobject]@{
        Path = $item.FullName
        Version = [string]$target.version
        Sha256 = ([string]$target.sha256).ToLowerInvariant()
        ProfileFileSha256 = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        SymbolCount = $symbolCount
    }
}

function Get-RemoteProfile {
    param(
        [Parameter(Mandatory = $true)]
        [string]$WorkDirectory
    )

    $manifestUri = "https://raw.githubusercontent.com/$owner/$repository/$branch/data/manifest.json"
    $manifestPath = Join-Path $WorkDirectory "manifest.json"
    [void](Invoke-WebRequest -Uri $manifestUri -OutFile $manifestPath -UseBasicParsing)
    $manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($manifest.schema -ne 1 -or $null -eq $manifest.versions) {
        throw "Unsupported or invalid profile manifest from GitHub."
    }

    $available = @(
        foreach ($property in $manifest.versions.PSObject.Properties) {
            if ($property.Name -notmatch '^\d+(\.\d+){1,3}$') { continue }
            $entry = $property.Value.'windows-x64'
            if ($null -eq $entry -or
                [string]$entry.path -notmatch '^data/[0-9.]+/windows-x64(\.json|/manifest\.json)$' -or
                [string]$entry.sha256 -notmatch '^[0-9a-fA-F]{64}$') {
                continue
            }
            if ([string]::IsNullOrWhiteSpace($Version) -or $property.Name -eq $Version) {
                [pscustomobject]@{
                    Version = $property.Name
                    RelativePath = [string]$entry.path
                    Sha256 = ([string]$entry.sha256).ToLowerInvariant()
                }
            }
        }
    )

    if ($available.Count -eq 0) {
        if ([string]::IsNullOrWhiteSpace($Version)) {
            throw "No Windows x64 profiles were listed in the GitHub manifest."
        }
        throw "Version '$Version' was not found in the GitHub profile manifest."
    }

    $selected = $available |
        Sort-Object -Property @{ Expression = { [version]$_.Version }; Descending = $true } |
        Select-Object -First 1
    $rawUri = "https://raw.githubusercontent.com/$owner/$repository/$branch/$($selected.RelativePath)"
    $profilePath = Join-Path $WorkDirectory "windows-x64.json"
    [void](Invoke-WebRequest -Uri $rawUri -OutFile $profilePath -UseBasicParsing)

    $downloadedHash = (Get-FileHash -LiteralPath $profilePath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($downloadedHash -ne $selected.Sha256) { throw "The downloaded profile file SHA-256 does not match the GitHub manifest." }
    $parentUri = $rawUri.Substring(0, $rawUri.LastIndexOf('/'))
    $profilePath = Expand-ProfileParts -Path $profilePath -RemotePrefix $parentUri
    $metadata = Get-ProfileMetadata -Path $profilePath
    if ($metadata.Version -ne $selected.Version) { throw "The downloaded profile version does not match the manifest entry." }
    return $metadata
}

function Get-LocalProfile {
    if ([string]::IsNullOrWhiteSpace($LocalPath)) {
        throw "-LocalPath is required when -Source Local is selected."
    }
    if (-not (Test-Path -LiteralPath $LocalPath)) {
        throw "LocalPath does not exist: $LocalPath"
    }

    $item = Get-Item -LiteralPath $LocalPath
    if ($item.PSIsContainer) {
        $files = @(
            Get-ChildItem -LiteralPath $item.FullName -File -Recurse |
                Where-Object {
                    ($_.Name -ieq "manifest.json" -and $_.Directory.Name -eq "windows-x64") -or
                    $_.Name -ieq "windows-x64.json" -or
                    $_.Name -match '^.+-windows-x64\.json$'
                }
        )
        $candidates = @(
            foreach ($file in $files) {
                try { Get-ProfileMetadata -Path $file.FullName }
                catch { Write-Verbose "Skipping '$($file.FullName)': $($_.Exception.Message)" }
            }
        )
    } else {
        $candidates = @(Get-ProfileMetadata -Path $item.FullName)
    }

    if (-not [string]::IsNullOrWhiteSpace($Version)) {
        $candidates = @($candidates | Where-Object { $_.Version -eq $Version })
    }
    if ($candidates.Count -eq 0) {
        if ([string]::IsNullOrWhiteSpace($Version)) {
            throw "No valid Windows x64 profile was found under '$LocalPath'."
        }
        throw "No valid Windows x64 profile for version '$Version' was found under '$LocalPath'."
    }

    return $candidates |
        Sort-Object -Property @{ Expression = { [version]$_.Version }; Descending = $true } |
        Select-Object -First 1
}

$documentsPath = [Environment]::GetFolderPath([Environment+SpecialFolder]::MyDocuments)
if ([string]::IsNullOrWhiteSpace($CommandsDirectory)) {
    $CommandsDirectory = Join-Path $documentsPath "PEXData/CheckSig/command"
}
$CommandsDirectory = [System.IO.Path]::GetFullPath($CommandsDirectory)

$workDirectory = Join-Path ([System.IO.Path]::GetTempPath()) ("CheckSigProfile-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $workDirectory | Out-Null
try {
    if ($Source -eq "GitHub") {
        $metadata = Get-RemoteProfile -WorkDirectory $workDirectory
        if (-not [string]::IsNullOrWhiteSpace($Version) -and $metadata.Version -ne $Version) {
            throw "Downloaded profile version '$($metadata.Version)' does not match requested version '$Version'."
        }
    } else {
        $metadata = Get-LocalProfile
    }

    $destinationName = "$($metadata.Version)-windows-x64.json"
    New-Item -ItemType Directory -Path $CommandsDirectory -Force | Out-Null
    $destinationPath = Join-Path $CommandsDirectory $destinationName
    if ((Test-Path -LiteralPath $destinationPath) -and -not $Force) {
        throw "Destination already exists; pass -Force to replace it: $destinationPath"
    }

    $stagingPath = Join-Path $CommandsDirectory (".CheckSigProfile-" + [guid]::NewGuid().ToString("N") + ".tmp")
    try {
        Copy-Item -LiteralPath $metadata.Path -Destination $stagingPath
        if (Test-Path -LiteralPath $destinationPath) {
            # Windows PowerShell 5.1 binds $null to an empty backup path.
            # Use a real temporary backup so replacement stays atomic there too.
            $backupPath = $stagingPath + ".backup"
            [System.IO.File]::Replace($stagingPath, $destinationPath, $backupPath)
            Remove-Item -LiteralPath $backupPath -Force -ErrorAction SilentlyContinue
        } else {
            [System.IO.File]::Move($stagingPath, $destinationPath)
        }
    }
    finally {
        if (Test-Path -LiteralPath $stagingPath) {
            Remove-Item -LiteralPath $stagingPath -Force
        }
    }

    $installedHash = (Get-FileHash -LiteralPath $destinationPath -Algorithm SHA256).Hash.ToLowerInvariant()
    Write-Output "Installed Windows x64 profile $($metadata.Version) with $($metadata.SymbolCount) symbols."
    Write-Output "Profile SHA-256: $($metadata.Sha256)"
    Write-Output "File SHA-256:    $installedHash"
    Write-Output "CheckSig will detect this JSON automatically: $destinationPath"
}
finally {
    Remove-Item -LiteralPath $workDirectory -Recurse -Force -ErrorAction SilentlyContinue
}
