Add-Type -AssemblyName System.Drawing

$pngPath = "bee.png"
$icoPath = "resources\bee.ico"

$png = [System.Drawing.Image]::FromFile((Resolve-Path $pngPath))

$icon = [System.Drawing.Icon]::FromHandle($png.GetHicon())

$fileStream = [System.IO.File]::Create($icoPath)
$icon.Save($fileStream)
$fileStream.Close()

$png.Dispose()
$icon.Dispose()

Write-Host "Successfully converted bee.png to bee.ico"
