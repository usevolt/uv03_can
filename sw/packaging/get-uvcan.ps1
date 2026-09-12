<#
.SYNOPSIS
    Installs uvcan on Windows, and updates an install that is already there.

.DESCRIPTION
    Published on the public shelf of the Usevolt file server alongside uvcan
    itself, so a fresh machine can be pointed straight at it:

        irm https://files.usevolt.fi/pub/uvcan/get-uvcan.ps1 | iex

    It reads latest.json to find the current release, and stops right there if
    what is already installed is that release or newer -- so the same command
    is both the installer and the updater, and running it on a machine that is
    up to date costs one small request and changes nothing.

    Otherwise it downloads the release's Windows package, checks it against the
    SHA-256 the manifest publishes, and unpacks it over the install directory.
    The build number it installed is written to uvcan-version.txt beside
    uvcan.exe, which is what a later run compares against.

    Everything it fetches is public; no account is needed at any point.

    The Linux uvcan updates itself (uvcan --update). The Windows build cannot:
    it ships as a folder of files rather than one binary, so re-running this
    script is how a Windows install is updated.

.PARAMETER InstallDir
    Where to install. Defaults to %LOCALAPPDATA%\Usevolt\uvcan, which needs no
    administrator rights.

.PARAMETER Force
    Install even when the installed build is already current.

.PARAMETER NoPath
    Do not offer to put the install directory on the user's PATH.

.EXAMPLE
    irm https://files.usevolt.fi/pub/uvcan/get-uvcan.ps1 | iex

.EXAMPLE
    # with arguments, the script has to be saved first -- `iex` takes none
    irm https://files.usevolt.fi/pub/uvcan/get-uvcan.ps1 -OutFile get-uvcan.ps1
    .\get-uvcan.ps1 -InstallDir C:\Tools\uvcan
#>
[CmdletBinding()]
param(
    [string] $InstallDir = (Join-Path $env:LOCALAPPDATA 'Usevolt\uvcan'),
    [switch] $Force,
    [switch] $NoPath
)

$ErrorActionPreference = 'Stop'
$Base = 'https://files.usevolt.fi/pub/uvcan'

# Windows PowerShell 5.1 still defaults to TLS 1.0 on some builds, and the file
# server speaks 1.2 and up. Without this the very first request fails with a
# connection error that says nothing about why.
try {
    [Net.ServicePointManager]::SecurityProtocol =
        [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
} catch {
    # PowerShell 7 manages this itself and the property is read-only there
}

function Get-InstalledBuild {
    param([string] $Dir)
    # The build number of what is installed, or 0 when there is nothing there.
    # Kept in a file of our own rather than asked of uvcan.exe: running the
    # installed binary to find out whether to replace it is a good way to have
    # it locked by its own process when the moment comes to overwrite it.
    $f = Join-Path $Dir 'uvcan-version.txt'
    if (Test-Path -LiteralPath $f) {
        $t = (Get-Content -LiteralPath $f -First 1).Trim()
        $n = 0
        if ([int]::TryParse($t, [ref] $n)) { return $n }
    }
    return 0
}

Write-Host 'Looking up the current uvcan...'
try {
    $m = Invoke-RestMethod -Uri "$Base/latest.json" -UseBasicParsing
} catch {
    throw "Could not read $Base/latest.json : $($_.Exception.Message)"
}
if (-not $m.package_win) {
    throw 'The published manifest names no Windows package.'
}

$installed = Get-InstalledBuild -Dir $InstallDir
if (-not $Force -and $installed -ge [int] $m.version) {
    Write-Host "uvcan $($m.name) (build $($m.version)) is already installed in $InstallDir."
    Write-Host 'Nothing to do. Re-run with -Force to install it again.'
    return
}
if ($installed -gt 0) {
    Write-Host "Installed: build $installed.  Available: $($m.name) (build $($m.version))."
}

$tmp = Join-Path ([IO.Path]::GetTempPath()) ("uvcan-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tmp -Force | Out-Null
try {
    $zip = Join-Path $tmp $m.package_win
    Write-Host "Downloading uvcan $($m.name)..."
    Invoke-WebRequest -Uri "$Base/$($m.package_win)" -OutFile $zip -UseBasicParsing

    # Verified when the manifest carries a checksum. Both come from the same
    # place, so this catches a truncated download rather than a hostile one --
    # which is the failure that actually happens.
    if ($m.package_win_sha256) {
        $got = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash
        if ($got -ne $m.package_win_sha256.ToUpper()) {
            throw 'The download does not match its checksum; nothing was installed.'
        }
        Write-Host 'Checksum ok.'
    }

    Expand-Archive -LiteralPath $zip -DestinationPath $tmp -Force
    # The package holds one folder named after the release; its contents are
    # what goes into the install directory, so that the directory does not gain
    # a new nested folder with every version.
    $inner = Get-ChildItem -LiteralPath $tmp -Directory | Select-Object -First 1
    $src = if ($inner) { $inner.FullName } else { $tmp }

    if (-not (Test-Path -LiteralPath $InstallDir)) {
        New-Item -ItemType Directory -Path $InstallDir -Force | Out-Null
    }
    Write-Host "Installing into $InstallDir..."
    # Copied over the top rather than the directory being emptied first: a
    # user's own files next to uvcan.exe are not ours to delete, and the
    # package replaces everything it owns anyway.
    Copy-Item -Path (Join-Path $src '*') -Destination $InstallDir -Recurse -Force
    Set-Content -LiteralPath (Join-Path $InstallDir 'uvcan-version.txt') `
        -Value ([string] $m.version) -Encoding ASCII
} finally {
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host ''
Write-Host "uvcan $($m.name) (build $($m.version)) is installed."
Write-Host "  $InstallDir\uvcan.exe          the tool"
Write-Host "  $InstallDir\uvcan-ui.bat       double-click to open the UI"
Write-Host "  $InstallDir\install-association.bat   register .uvsys / .uvdev files"
Write-Host ''
Write-Host 'It needs the PEAK PCAN-USB driver from https://www.peak-system.com/ ,'
Write-Host 'which PCANBasic.dll here is only the API shim for.'

if (-not $NoPath) {
    # The user's own PATH, not the machine's: this install needs no
    # administrator rights and must not start needing them here.
    $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
    if (($userPath -split ';') -notcontains $InstallDir) {
        Write-Host ''
        $a = Read-Host "Add $InstallDir to your PATH? [y/N]"
        if ($a -match '^[Yy]') {
            $new = if ([string]::IsNullOrEmpty($userPath)) { $InstallDir }
                   else { "$userPath;$InstallDir" }
            [Environment]::SetEnvironmentVariable('Path', $new, 'User')
            Write-Host 'Added. Open a new terminal for it to take effect.'
        }
    }
}

Write-Host ''
Write-Host 'Run this same command again at any time to update to the newest uvcan.'
