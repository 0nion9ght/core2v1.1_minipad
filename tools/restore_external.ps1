# ============================================================================
# restore_external.ps1 - re-create external/ with the pinned third-party libs.
#
# This repository only contains the firmware sources: LVGL, M5GFX and M5Unified
# are not committed. Run this once after cloning, then build normally.
#
#   pwsh -File tools/restore_external.ps1
#
# The versions below are exactly the ones the firmware was developed against
# (pinned by commit, not by branch), so the build is reproducible.
# ============================================================================
[CmdletBinding()]
param(
    [string]$Root = (Join-Path $PSScriptRoot '..')
)

$ErrorActionPreference = 'Stop'

# name       = directory to create under external/
# repo       = GitHub owner/repo
# commit     = pinned commit (as recorded by the component registry metadata)
# rename_to  = upstream archive root differs from the component name we use
$libs = @(
    @{ name = 'lvgl';      repo = 'lvgl/lvgl';          commit = '80ca777e37a2b176770726a02e07a6fb79ef0b39' }
    @{ name = 'M5GFX';     repo = 'm5stack/M5GFX';      commit = 'd91077b9a607b59404e4e4a49f775c792bfae382' }
    @{ name = 'm5unified'; repo = 'm5stack/M5Unified';  commit = '3eaaf828adfd0923c71ccc2e233a0199d9958faa' }
)

$external = Join-Path (Resolve-Path $Root) 'external'
New-Item -ItemType Directory -Force -Path $external | Out-Null

foreach ($lib in $libs) {
    $dest = Join-Path $external $lib.name
    if (Test-Path $dest) {
        Write-Host "[skip] external/$($lib.name) already exists"
        continue
    }

    $zip = Join-Path $env:TEMP "$($lib.name)-$($lib.commit).zip"
    $url = "https://github.com/$($lib.repo)/archive/$($lib.commit).zip"
    Write-Host "[get ] $($lib.repo) @ $($lib.commit.Substring(0, 8))"
    Invoke-WebRequest -Uri $url -OutFile $zip

    $tmp = Join-Path $env:TEMP "extract-$($lib.name)"
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    Expand-Archive -Path $zip -DestinationPath $tmp
    $inner = Get-ChildItem $tmp -Directory | Select-Object -First 1
    Move-Item $inner.FullName $dest
    Remove-Item -Recurse -Force $tmp
    Remove-Item -Force $zip

    # The upstream manifests declare registry dependencies (lvgl's optional
    # freetype/png codecs, M5Unified's m5stack/m5gfx) which would make the IDF
    # component manager download a second copy into managed_components/.
    # Here the libraries are plain local components from EXTRA_COMPONENT_DIRS,
    # so drop the manifests.
    $yml = Join-Path $dest 'idf_component.yml'
    if (Test-Path $yml) {
        Remove-Item -Force $yml
        Write-Host "       [edit] dropped external/$($lib.name)/idf_component.yml"
    }
}

Write-Host ''
Write-Host 'external/ restored. Next steps:'
Write-Host '  idf.py set-target esp32     # creates sdkconfig from sdkconfig.defaults'
Write-Host '  idf.py build'
Write-Host '  idf.py -p <PORT> flash'
Write-Host ''
Write-Host 'Note: put your Wi-Fi credentials into sdkconfig (CONFIG_APP_WIFI_SSID/'
Write-Host 'CONFIG_APP_WIFI_PASSWORD); sdkconfig is git-ignored on purpose.'
