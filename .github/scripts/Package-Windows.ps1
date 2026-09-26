[CmdletBinding()]
param(
    [ValidateSet('x64')]
    [string] $Target = 'x64',
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release', 'MinSizeRel')]
    [string] $Configuration = 'RelWithDebInfo'
)

$ErrorActionPreference = 'Stop'

if ( $DebugPreference -eq 'Continue' ) {
    $VerbosePreference = 'Continue'
    $InformationPreference = 'Continue'
}

if ( $env:CI -eq $null ) {
    throw "Package-Windows.ps1 requires CI environment"
}

if ( ! [System.Environment]::Is64BitOperatingSystem ) {
    throw "Packaging requires a 64-bit system."
}

if ( $PSVersionTable.PSVersion -lt [version]'7.2.0' ) {
    throw 'PowerShell Core 7.2 or newer is required.'
}

function Package {
    trap {
        Write-Error $_
        exit 2
    }

    $ProjectRoot = (Resolve-Path -Path "$PSScriptRoot/../..").Path
    $BuildSpecFile = Join-Path $ProjectRoot 'buildspec.json'
    $BuildSpec = Get-Content -Path $BuildSpecFile -Raw | ConvertFrom-Json
    $ProductName = $BuildSpec.name
    $ProductVersion = $BuildSpec.version
    $OutputName = "${ProductName}-${ProductVersion}-windows-${Target}"
    $InstallRoot = Join-Path $ProjectRoot "release/$Configuration"
    $InstalledPluginRoot = Join-Path $InstallRoot $ProductName
    $InstalledBinRoot = Join-Path $InstalledPluginRoot 'bin'
    $InstalledDataRoot = Join-Path $InstalledPluginRoot 'data'
    $ReleaseRoot = Join-Path $ProjectRoot 'release'
    $StageRoot = Join-Path $ReleaseRoot ".staging/$OutputName"
    $OutputPath = Join-Path $ReleaseRoot "${OutputName}.zip"

    if ( ! ( Test-Path -LiteralPath $InstalledBinRoot -PathType Container ) ) {
        throw "The installed plugin bin directory does not exist: $InstalledBinRoot"
    }
    if ( ! ( Test-Path -LiteralPath $InstalledDataRoot -PathType Container ) ) {
        throw "The installed plugin data directory does not exist: $InstalledDataRoot"
    }

    $InstalledFiles = @(Get-ChildItem -LiteralPath $InstalledPluginRoot -Recurse -File)
    if ( $InstalledFiles.Count -eq 0 ) {
        throw "The installed plugin directory is empty: $InstalledPluginRoot"
    }

    Get-ChildItem -Path $ReleaseRoot -Filter "$($ProductName)-*-windows-*.zip" -File -ErrorAction SilentlyContinue |
        Remove-Item -Force
    Remove-Item -LiteralPath $StageRoot -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Path $StageRoot -Force | Out-Null

    # OBS expects these two directories at the root of the plugin ZIP:
    #   obs-plugins/64bit/<plugin>.dll
    #   data/<plugin>/...
    $PackageBinRoot = Join-Path $StageRoot 'obs-plugins'
    $PackageDataRoot = Join-Path $StageRoot 'data'
    New-Item -ItemType Directory -Path $PackageBinRoot, $PackageDataRoot -Force | Out-Null
    Copy-Item -Path (Join-Path $InstalledBinRoot '*') -Destination $PackageBinRoot -Recurse
    Copy-Item -Path (Join-Path $InstalledDataRoot '*') -Destination $PackageDataRoot -Recurse

    $PackageFiles = @(Get-ChildItem -LiteralPath $StageRoot -Recurse -File)
    if ( $PackageFiles.Count -eq 0 ) {
        throw "The package staging directory is empty: $StageRoot"
    }

    Write-Host "Creating $OutputPath"
    Push-Location $StageRoot
    try {
        Compress-Archive -Path .\* -DestinationPath $OutputPath -CompressionLevel Optimal -Force
    } finally {
        Pop-Location
    }

    $Zip = Get-Item -LiteralPath $OutputPath
    if ( $Zip.Length -le 0 ) {
        throw "The generated ZIP is empty: $OutputPath"
    }
    Write-Host "Created $($Zip.Name) ($($Zip.Length) bytes)"
}

Package
