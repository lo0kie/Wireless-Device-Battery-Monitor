# Fetches the WebView2 SDK the settings window needs (third_party is not in the repository).
# Pinned to the same version and checksum as .github/workflows/build.yml.
$ErrorActionPreference = "Stop"
$root = Join-Path (Split-Path $PSScriptRoot -Parent) "third_party\webview2"
if (Test-Path (Join-Path $root "include\WebView2.h")) {
    Write-Host "WebView2 SDK already present in $root"
    exit 0
}
$ver = "1.0.4370.26"
$nupkg = Join-Path $root "wv2.zip"
New-Item -ItemType Directory -Force $root | Out-Null
Write-Host "Downloading WebView2 SDK $ver..."
Invoke-WebRequest "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/$ver/microsoft.web.webview2.$ver.nupkg" -OutFile $nupkg
$hash = (Get-FileHash $nupkg -Algorithm SHA256).Hash
if ($hash -ne "1C0DACAC84B70109B3FD42F90C53A0BA942ABC3A87590D7A4E0CCD86E4A06CF4") {
    Remove-Item $nupkg -Force
    throw "WebView2 SDK checksum mismatch: $hash"
}
Expand-Archive $nupkg -DestinationPath (Join-Path $root "pkg") -Force
New-Item -ItemType Directory -Force (Join-Path $root "include"), (Join-Path $root "x64") | Out-Null
Copy-Item (Join-Path $root "pkg\build\native\include\WebView2.h"),
          (Join-Path $root "pkg\build\native\include\WebView2EnvironmentOptions.h") (Join-Path $root "include")
Copy-Item (Join-Path $root "pkg\build\native\x64\WebView2Loader.dll") (Join-Path $root "x64")
Remove-Item $nupkg -Force
Remove-Item (Join-Path $root "pkg") -Recurse -Force
Write-Host "WebView2 SDK ready in $root"
