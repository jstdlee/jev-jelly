# Adds a "Jelly" shortcut with the jelly icon to the desktop and the Start menu (Windows, no admin needed).
#   powershell -ExecutionPolicy Bypass -File install-desktop.ps1            install
#   powershell -ExecutionPolicy Bypass -File install-desktop.ps1 -Remove    take it away again
#   ... -Startup                                                              also start Jelly when you sign in
param([switch]$Remove, [switch]$Startup)
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe = @("$here\jelly.exe", "$here\..\jelly.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
$places = @([Environment]::GetFolderPath('Desktop'), [Environment]::GetFolderPath('Programs'))
if ($Startup) { $places += [Environment]::GetFolderPath('Startup') }
foreach ($dir in $places) {
  $lnk = Join-Path $dir 'Jelly.lnk'
  if ($Remove) { Remove-Item -ErrorAction SilentlyContinue $lnk; continue }
  if (-not $exe) { Write-Error "jelly.exe not found next to this script"; exit 1 }
  $exe = (Resolve-Path $exe).Path
  $s = (New-Object -ComObject WScript.Shell).CreateShortcut($lnk)
  $s.TargetPath = $exe
  $s.WorkingDirectory = Split-Path -Parent $exe
  $s.IconLocation = "$exe,0"
  $s.Description = 'A squishy jelly friend that lives on your desktop'
  $s.Save()
}
if ($Remove) { 'removed' } else { "installed: $($places -join ', ')" }
