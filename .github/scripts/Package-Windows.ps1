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
    $OutputPath = Join-Path $ProjectRoot "release/${OutputName}.zip"

    if ( ! ( Test-Path -LiteralPath $InstallRoot -PathType Container ) ) {
        throw "The install directory does not exist: $InstallRoot"
    }

    $Files = @(Get-ChildItem -LiteralPath $InstallRoot -Recurse -File)
    if ( $Files.Count -eq 0 ) {
        throw "The install directory is empty: $InstallRoot"
    }

    Get-ChildItem -Path (Join-Path $ProjectRoot 'release') -Filter "$($ProductName)-*-windows-*.zip" -File -ErrorAction SilentlyContinue |
        Remove-Item -Force

    Write-Host "Creating $OutputPath"
    Push-Location $InstallRoot
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
