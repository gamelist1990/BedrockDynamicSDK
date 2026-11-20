[CmdletBinding()]
param(
    [ValidateSet("GitHub", "Local")]
    [string]$Source = "GitHub",

    [string]$LocalPath,

    [string]$Destination,

    [switch]$Force
)

$ErrorActionPreference = "Stop"

function Get-CheckSigDllPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$ExtractionDirectory
    )

    if (Test-Path -LiteralPath $Path -PathType Container) {
        $matches = @(Get-ChildItem -LiteralPath $Path -Filter "CheckSig.dll" -File -Recurse)
    }
    elseif (Test-Path -LiteralPath $Path -PathType Leaf) {
        $item = Get-Item -LiteralPath $Path
        if ($item.Extension -ieq ".dll") {
            if ($item.Name -ine "CheckSig.dll") {
                throw "The local DLL must be named CheckSig.dll: $Path"
            }
            return $item.FullName
        }
        if ($item.Extension -ine ".zip") {
            throw "LocalPath must be CheckSig.dll, a ZIP artifact, or a directory: $Path"
        }

        return Get-CheckSigDllFromZip -ZipPath $item.FullName -ExtractionDirectory $ExtractionDirectory
    }
    else {
        throw "LocalPath does not exist: $Path"
    }

    if ($matches.Count -ne 1) {
        throw "Expected exactly one CheckSig.dll under '$Path'; found $($matches.Count). Pass the DLL itself when multiple copies exist."
    }

    return $matches[0].FullName
}

function Get-CheckSigDllFromZip {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ZipPath,

        [Parameter(Mandatory = $true)]
        [string]$ExtractionDirectory
    )

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [System.IO.Compression.ZipFile]::OpenRead($ZipPath)
    try {
        $matches = @($archive.Entries | Where-Object { $_.Name -ieq "CheckSig.dll" })
        if ($matches.Count -ne 1) {
            throw "Expected exactly one CheckSig.dll in ZIP '$ZipPath'; found $($matches.Count)."
        }
        if ($matches[0].Length -le 0 -or $matches[0].Length -gt 536870912) {
            throw "The CheckSig.dll entry has an invalid size in ZIP '$ZipPath'."
        }

        New-Item -ItemType Directory -Path $ExtractionDirectory -Force | Out-Null
        $dllPath = Join-Path $ExtractionDirectory "CheckSig.dll"
        $entryStream = $matches[0].Open()
        $fileStream = [System.IO.File]::Create($dllPath)
        try {
            $entryStream.CopyTo($fileStream)
        }
        finally {
            $fileStream.Dispose()
            $entryStream.Dispose()
        }

        return $dllPath
    }
    finally {
        $archive.Dispose()
    }
}

function Assert-X64Pe {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $stream = [System.IO.File]::OpenRead($Path)
    $reader = [System.IO.BinaryReader]::new($stream)
    try {
        if ($stream.Length -lt 0x40 -or $reader.ReadUInt16() -ne 0x5A4D) {
            throw "Not a valid PE file: $Path"
        }

        $stream.Position = 0x3C
        $peOffset = $reader.ReadInt32()
        if ($peOffset -lt 0x40 -or $peOffset -gt ($stream.Length - 26)) {
            throw "Invalid PE header offset in: $Path"
        }

        $stream.Position = $peOffset
        if ($reader.ReadUInt32() -ne 0x00004550) {
            throw "Missing PE signature: $Path"
        }

        $machine = $reader.ReadUInt16()
        if ($machine -ne 0x8664) {
            throw ("CheckSig must be x64 (PE machine 0x8664); found 0x{0:X4}: {1}" -f $machine, $Path)
        }

        $stream.Position = $peOffset + 20
        $optionalHeaderSize = $reader.ReadUInt16()
        $characteristics = $reader.ReadUInt16()
        if ($optionalHeaderSize -lt 2 -or ($characteristics -band 0x2000) -eq 0) {
            throw "The PE file is not an x64 DLL: $Path"
        }

        $stream.Position = $peOffset + 24
        if ($reader.ReadUInt16() -ne 0x020B) {
            throw "CheckSig must use the PE32+ optional header: $Path"
        }
    }
    finally {
        $reader.Dispose()
        $stream.Dispose()
    }
}

function Get-GitHubArtifactDll {
    param(
        [Parameter(Mandatory = $true)]
        [string]$WorkDirectory
    )

    $owner = "gamelist1990"
    $repository = "BedrockDynamicSDK"
    $headers = @{
        Accept = "application/vnd.github+json"
        "X-GitHub-Api-Version" = "2022-11-28"
        "User-Agent" = "BedrockDynamicSDK-CheckSig-Downloader"
    }

    $apiToken = $env:GH_TOKEN
    if ([string]::IsNullOrWhiteSpace($apiToken)) {
        $apiToken = $env:GITHUB_TOKEN
    }
    if (-not [string]::IsNullOrWhiteSpace($apiToken)) {
        $headers.Authorization = "Bearer $apiToken"
    }

    $runsUri = "https://api.github.com/repos/$owner/$repository/actions/workflows/build-mods.yml/runs?branch=main&per_page=50"
    $runs = Invoke-RestMethod -Method Get -Uri $runsUri -Headers $headers
    $successfulRuns = @(
        $runs.workflow_runs |
            Where-Object { $_.status -eq "completed" -and $_.conclusion -eq "success" } |
            Sort-Object -Property created_at -Descending
    )

    foreach ($run in $successfulRuns) {
        $artifactsUri = "https://api.github.com/repos/$owner/$repository/actions/runs/$($run.id)/artifacts?per_page=100"
        $artifactList = Invoke-RestMethod -Method Get -Uri $artifactsUri -Headers $headers
        $artifact = @(
            $artifactList.artifacts |
                Where-Object { $_.name -eq "CheckSig-windows-x64" -and -not $_.expired } |
                Sort-Object -Property created_at -Descending
        ) | Select-Object -First 1

        if ($null -eq $artifact) {
            continue
        }

        $archivePath = Join-Path $WorkDirectory "CheckSig-windows-x64.zip"
        Invoke-WebRequest -UseBasicParsing -Method Get -Uri $artifact.archive_download_url -Headers $headers -OutFile $archivePath | Out-Null

        $artifactDigest = [string]$artifact.digest
        if ($artifactDigest -match "^sha256:[0-9a-fA-F]{64}$") {
            $expectedArchiveHash = $artifactDigest.Substring(7)
            $archiveHash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash
            if ($archiveHash -ine $expectedArchiveHash) {
                throw "GitHub artifact ZIP SHA-256 did not match its API digest (run $($run.id))."
            }
        }

        $dllPath = Get-CheckSigDllFromZip -ZipPath $archivePath -ExtractionDirectory (Join-Path $WorkDirectory "github-artifact")

        return [pscustomobject]@{
            DllPath = $dllPath
            RunId = $run.id
            Commit = $run.head_sha
            Artifact = $artifact.name
        }
    }

    throw "No unexpired CheckSig-windows-x64 artifact was found in the latest 50 successful All mod DLLs runs on main."
}

$documentsPath = [Environment]::GetFolderPath([Environment+SpecialFolder]::MyDocuments)
if ([string]::IsNullOrWhiteSpace($Destination)) {
    if ([string]::IsNullOrWhiteSpace($documentsPath)) {
        throw "The Documents folder could not be resolved. Pass -Destination explicitly."
    }
    $destinationPath = Join-Path $documentsPath "PEXData/CheckSig/bin/CheckSig.dll"
}
elseif ([System.IO.Path]::GetExtension($Destination) -ieq ".dll") {
    $destinationPath = [System.IO.Path]::GetFullPath($Destination)
}
else {
    $destinationPath = Join-Path ([System.IO.Path]::GetFullPath($Destination)) "CheckSig.dll"
}

$destinationDirectory = Split-Path -Parent $destinationPath

$workDirectory = Join-Path ([System.IO.Path]::GetTempPath()) ("CheckSig-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $workDirectory -Force | Out-Null

try {
    if ($Source -eq "GitHub") {
        $artifact = Get-GitHubArtifactDll -WorkDirectory $workDirectory
        $dllPath = $artifact.DllPath
        Write-Output "Downloaded CheckSig x64 from GitHub Actions run $($artifact.RunId) ($($artifact.Commit))."
    }
    else {
        if ([string]::IsNullOrWhiteSpace($LocalPath)) {
            throw "-LocalPath is required when -Source Local is selected."
        }
        $dllPath = Get-CheckSigDllPath -Path $LocalPath -ExtractionDirectory (Join-Path $workDirectory "local-artifact")
        Write-Output "Using local CheckSig DLL: $dllPath"
    }

    Assert-X64Pe -Path $dllPath
    $sha256 = (Get-FileHash -LiteralPath $dllPath -Algorithm SHA256).Hash.ToLowerInvariant()

    New-Item -ItemType Directory -Path $destinationDirectory -Force | Out-Null
    if (Test-Path -LiteralPath $destinationPath -PathType Container) {
        throw "Destination points to a directory. Pass either a DLL path or its parent directory: $destinationPath"
    }
    if ((Test-Path -LiteralPath $destinationPath -PathType Leaf) -and -not $Force) {
        throw "Destination already exists. Pass -Force to replace it: $destinationPath"
    }

    $stagingPath = Join-Path $destinationDirectory (".CheckSig-" + [guid]::NewGuid().ToString("N") + ".tmp")
    try {
        Copy-Item -LiteralPath $dllPath -Destination $stagingPath
        if (Test-Path -LiteralPath $destinationPath -PathType Leaf) {
            Move-Item -LiteralPath $stagingPath -Destination $destinationPath -Force
        }
        else {
            Move-Item -LiteralPath $stagingPath -Destination $destinationPath
        }
    }
    finally {
        if (Test-Path -LiteralPath $stagingPath) {
            Remove-Item -LiteralPath $stagingPath -Force
        }
    }

    Write-Output "Installed CheckSig x64: $destinationPath"
    Write-Output "SHA256: $sha256"
}
finally {
    if (Test-Path -LiteralPath $workDirectory) {
        Remove-Item -LiteralPath $workDirectory -Recurse -Force
    }
}
