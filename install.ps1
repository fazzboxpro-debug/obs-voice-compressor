# Installs only this plugin. Run after extracting the complete ZIP.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
try {
    $admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    if (-not $admin) {
        $arguments = '-NoProfile -ExecutionPolicy Bypass -File "' + $PSCommandPath + '"'
        Start-Process -FilePath 'powershell.exe' -Verb RunAs -WindowStyle Hidden -ArgumentList $arguments -Wait
        exit
    }
    if (Get-Process obs64 -ErrorAction SilentlyContinue) {
        throw 'Close OBS Studio, then run this installer again.'
    }
    $source = Join-Path $PSScriptRoot 'package\voice-compressor'
    if (-not (Test-Path -LiteralPath (Join-Path $source 'bin\64bit\voice-compressor.dll'))) {
        throw 'Plugin DLL missing. Extract the complete ZIP before installing.'
    }
    $pluginRoot = [IO.Path]::GetFullPath((Join-Path $env:ProgramData 'obs-studio\plugins'))
    $target = [IO.Path]::GetFullPath((Join-Path $pluginRoot 'voice-compressor'))
    if ($target -ne (Join-Path $pluginRoot 'voice-compressor')) { throw 'Invalid install path.' }
    if (Test-Path -LiteralPath $target) {
        $backupRoot = Join-Path $env:ProgramData 'obs-studio\voice-compressor-backups'
        New-Item -ItemType Directory -Path $backupRoot -Force | Out-Null
        Copy-Item -LiteralPath $target -Destination (Join-Path $backupRoot ([guid]::NewGuid().ToString())) -Recurse
    }
    New-Item -ItemType Directory -Path $target -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $source 'bin') -Destination $target -Recurse -Force
    Copy-Item -LiteralPath (Join-Path $source 'data') -Destination $target -Recurse -Force
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'LICENSE') -Destination $target -Force
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'licenses') -Destination $target -Recurse -Force
    [System.Windows.Forms.MessageBox]::Show('Installed. Open OBS > Audio Filters > + > Voice Compressor.','Voice Compressor') | Out-Null
} catch {
    [System.Windows.Forms.MessageBox]::Show($_.Exception.Message,'Voice Compressor - installation failed') | Out-Null
    exit 1
}
