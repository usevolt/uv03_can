<#
.SYNOPSIS
    Removes uvcan from Windows.

.DESCRIPTION
    Published on the public shelf beside get-uvcan.ps1, so it can be run on a
    machine that no longer has the package:

        irm https://files.usevolt.fi/pub/uvcan/uninstall-uvcan.ps1 | iex

    It undoes exactly what get-uvcan.ps1 and install-association.bat do: the
    .uvsys / .uvdev file associations under HKEY_CURRENT_USER, the install
    directory, and the PATH entry. Nothing needs administrator rights, because
    nothing was installed with them.

    Your own .uvdev / .uvsys package files are never touched. Neither are the
    saved account settings, unless -Purge is given.

.PARAMETER InstallDir
    Where uvcan was installed. Defaults to %LOCALAPPDATA%\Usevolt\uvcan, which
    is where get-uvcan.ps1 puts it.

.PARAMETER Purge
    Also delete the saved account settings in %APPDATA%\uvcan.

.PARAMETER KeepAssociations
    Leave the .uvsys / .uvdev registration alone. Only useful when a second
    copy of uvcan elsewhere on the machine should keep handling them.

.EXAMPLE
    irm https://files.usevolt.fi/pub/uvcan/uninstall-uvcan.ps1 | iex

.EXAMPLE
    # with arguments, the script has to be saved first -- `iex` takes none
    irm https://files.usevolt.fi/pub/uvcan/uninstall-uvcan.ps1 -OutFile uninstall-uvcan.ps1
    .\uninstall-uvcan.ps1 -InstallDir C:\Tools\uvcan -Purge
#>
[CmdletBinding()]
param(
    [string] $InstallDir = (Join-Path $env:LOCALAPPDATA 'Usevolt\uvcan'),
    [switch] $Purge,
    [switch] $KeepAssociations
)

$ErrorActionPreference = 'Stop'
$removedAny = $false

# --- file associations ------------------------------------------------------
# install-association.bat writes four keys per file type, all under
# HKCU\Software\Classes: the ProgID, its DefaultIcon, its shell\open\command,
# and the extension pointing at the ProgID. Removed here rather than by calling
# uninstall-association.bat, which lives inside the folder being deleted and
# ends on `pause` -- it would sit there waiting for a keypress nobody is there
# to give.
if (-not $KeepAssociations) {
    $pairs = @(
        @{ ProgId = 'Usevolt.uvsys'; Ext = '.uvsys' },
        @{ ProgId = 'Usevolt.uvdev'; Ext = '.uvdev' }
    )
    foreach ($p in $pairs) {
        $progPath = "HKCU:\Software\Classes\$($p.ProgId)"
        if (Test-Path -LiteralPath $progPath) {
            Remove-Item -LiteralPath $progPath -Recurse -Force
            Write-Host "Removed the $($p.ProgId) registration."
            $removedAny = $true
        }
        # The extension key is only ours while it still points at our ProgID.
        # If something else has since claimed .uvdev, leaving it alone is the
        # right thing -- deleting it would break whatever took over.
        $extPath = "HKCU:\Software\Classes\$($p.Ext)"
        if (Test-Path -LiteralPath $extPath) {
            $cur = (Get-ItemProperty -LiteralPath $extPath -Name '(default)' `
                    -ErrorAction SilentlyContinue).'(default)'
            if ($cur -eq $p.ProgId) {
                Remove-Item -LiteralPath $extPath -Recurse -Force
                Write-Host "Removed the $($p.Ext) association."
            } elseif ($cur) {
                Write-Host "Left $($p.Ext) alone: it now opens with '$cur'."
            }
        }
    }
    # nudge Explorer to forget the icons it cached for those extensions
    & ie4uinit.exe -show 2>$null
}

# --- the install directory --------------------------------------------------
if (Test-Path -LiteralPath $InstallDir) {
    # A running uvcan.exe holds its own file open and the delete would fail
    # halfway, leaving the folder half gone. Say so plainly instead.
    $running = Get-Process -Name 'uvcan' -ErrorAction SilentlyContinue
    if ($running) {
        throw "uvcan is still running (pid $($running.Id -join ', ')). Close it and run this again."
    }
    Remove-Item -LiteralPath $InstallDir -Recurse -Force
    Write-Host "Removed $InstallDir."
    $removedAny = $true

    # take the Usevolt folder with it when uvcan was the only thing in it
    $parent = Split-Path -Parent $InstallDir
    if ((Test-Path -LiteralPath $parent) -and
            -not (Get-ChildItem -LiteralPath $parent -Force)) {
        Remove-Item -LiteralPath $parent -Force
    }
} else {
    Write-Host "Nothing installed at $InstallDir."
}

# --- PATH -------------------------------------------------------------------
# The user's own PATH, which is the only one get-uvcan.ps1 ever writes to.
$userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
if ($userPath) {
    $kept = @($userPath -split ';' | Where-Object { $_ -and ($_ -ne $InstallDir) })
    if ($kept.Count -ne @($userPath -split ';' | Where-Object { $_ }).Count) {
        [Environment]::SetEnvironmentVariable('Path', ($kept -join ';'), 'User')
        Write-Host 'Removed the PATH entry. Open a new terminal for it to take effect.'
        $removedAny = $true
    }
}

# --- saved account settings -------------------------------------------------
$cfg = Join-Path $env:APPDATA 'uvcan'
if ($Purge) {
    if (Test-Path -LiteralPath $cfg) {
        Remove-Item -LiteralPath $cfg -Recurse -Force
        Write-Host "Removed the saved account settings in $cfg."
        $removedAny = $true
    }
} elseif (Test-Path -LiteralPath $cfg) {
    Write-Host "The saved account settings in $cfg were kept. Remove them with -Purge."
}

Write-Host ''
if ($removedAny) {
    Write-Host 'Done. uvcan is removed; your .uvdev and .uvsys files are untouched.'
} else {
    Write-Warning 'No uvcan install was found. If it is somewhere else, pass -InstallDir.'
}
