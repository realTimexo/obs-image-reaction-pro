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
    throw "Build-Windows.ps1 requires CI environment"
}

if ( ! [System.Environment]::Is64BitOperatingSystem ) {
    throw "A 64-bit system is required to build the project."
}

if ( $PSVersionTable.PSVersion -lt [version]'7.2.0' ) {
    throw 'PowerShell Core 7.2 or newer is required.'
}

function Build {
    trap {
        Pop-Location -StackName BuildTemp -ErrorAction SilentlyContinue
        Write-Error $_
        Log-Group
        exit 2
    }

    $ScriptHome = $PSScriptRoot
    $ProjectRoot = (Resolve-Path -Path "$PSScriptRoot/../..").Path

    $UtilityFunctions = Get-ChildItem -Path "$PSScriptRoot/utils.pwsh/*.ps1" -Recurse
    foreach ( $Utility in $UtilityFunctions ) {
        . $Utility.FullName
    }

    $BuildSpec = Get-Content -Path (Join-Path $ProjectRoot 'buildspec.json') -Raw | ConvertFrom-Json
    $ProductName = $BuildSpec.name
    Push-Location -StackName BuildTemp
    Ensure-Location $ProjectRoot

    $CmakeArgs = @('--preset', "windows-ci-${Target}")
    $CmakeBuildArgs = @('--build', '--preset', "windows-${Target}", '--config', $Configuration, '--parallel', '--', '/consoleLoggerParameters:Summary', '/noLogo')
    $CmakeInstallArgs = @('--install', "build_${Target}", '--prefix', (Join-Path $ProjectRoot "release/$Configuration"), '--config', $Configuration)

    if ( $DebugPreference -eq 'Continue' ) {
        $CmakeArgs += '--debug-output'
        $CmakeBuildArgs += '--verbose'
        $CmakeInstallArgs += '--verbose'
    }

    Log-Group "Configuring ${ProductName}..."
    Invoke-External cmake @CmakeArgs

    Log-Group "Building ${ProductName}..."
    Invoke-External cmake @CmakeBuildArgs

    Log-Group "Installing ${ProductName}..."
    Invoke-External cmake @CmakeInstallArgs

    $InstallRoot = Join-Path $ProjectRoot "release/$Configuration"
    if ( ! ( Test-Path -LiteralPath $InstallRoot -PathType Container ) ) {
        throw "The install directory was not created: $InstallRoot"
    }
    if ( ! ( Get-ChildItem -LiteralPath $InstallRoot -Recurse -File | Select-Object -First 1 ) ) {
        throw "The install directory is empty: $InstallRoot"
    }

    Pop-Location -StackName BuildTemp
    Log-Group
}

Build
