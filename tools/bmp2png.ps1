# convert every shot_*.bmp in the tools folder to png
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
$dir = Split-Path -Parent $MyInvocation.MyCommand.Path
Get-ChildItem $dir -Filter "shot_*.bmp" | ForEach-Object {
    $png = [IO.Path]::ChangeExtension($_.FullName, ".png")
    $b = [System.Drawing.Bitmap]::FromFile($_.FullName)
    $b.Save($png, [System.Drawing.Imaging.ImageFormat]::Png)
    $b.Dispose()
    Write-Output ("{0} -> {1} ({2} bytes)" -f $_.Name, [IO.Path]::GetFileName($png), (Get-Item $png).Length)
}
