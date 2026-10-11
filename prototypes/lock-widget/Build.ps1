# Copyright (c) 2026 Ibrahim Bouillet. SPDX-License-Identifier: MIT
param([switch]$Candidate,[string]$OutputRoot)
$ErrorActionPreference = 'Stop'
$env:DOTNET_GENERATE_ASPNET_CERTIFICATE = 'false'
$env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'
$prototypeRoot = $PSScriptRoot
$packageRoot = if ($Candidate) { Join-Path (Split-Path (Split-Path $prototypeRoot)) 'out\Widgets' } else { Join-Path $prototypeRoot 'package' }
if ($OutputRoot) { $packageRoot = [IO.Path]::GetFullPath($OutputRoot) }
$dotnet = Get-Command dotnet -CommandType Application -ErrorAction SilentlyContinue
if (!$dotnet) { throw '.NET SDK 10 or newer is required. Install it and add dotnet to PATH.' }
& $dotnet.Source publish (Join-Path $prototypeRoot 'ALARM.WidgetPrototype.csproj') -c Release -o $packageRoot --verbosity minimal ('-p:AlarmWidgetCandidate=' + $Candidate.IsPresent.ToString().ToLowerInvariant())
if ($LASTEXITCODE -ne 0) { throw 'Prototype publish failed' }
$assetsRoot = Join-Path $packageRoot 'Assets'
[void](New-Item -ItemType Directory -Force -Path $assetsRoot)
[void](New-Item -ItemType Directory -Force -Path (Join-Path $packageRoot 'Public'))
Add-Type -AssemblyName System.Drawing
# Preserve the original ALARM bitmap byte-for-byte; the host alone scales
# the embedded source to 28px. Package icon variants derive from that source.
$alarmLogoPath = Join-Path (Split-Path (Split-Path $prototypeRoot)) 'assets\alarm.png'
Copy-Item -LiteralPath $alarmLogoPath -Destination (Join-Path $assetsRoot 'Alarm.png') -Force
Copy-Item -LiteralPath $alarmLogoPath -Destination (Join-Path $packageRoot 'Web\alarm.png') -Force
$alarmLogo = [System.Drawing.Bitmap]::new($alarmLogoPath)
try {
foreach ($entry in @(@('StoreLogo.png',50),@('Square44x44Logo.png',44),@('Square150x150Logo.png',150))) {
    $size = [int]$entry[1]
    $bitmap = New-Object System.Drawing.Bitmap($size,$size)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $graphics.Clear([System.Drawing.Color]::Transparent)
    $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $graphics.DrawImage($alarmLogo,0,0,$size,$size)
    $bitmap.Save((Join-Path $assetsRoot $entry[0]),[System.Drawing.Imaging.ImageFormat]::Png)
    $graphics.Dispose(); $bitmap.Dispose()
}
# Static material image, generated from this app's own canvas. Windows owns
# actual background transparency, corner clipping and above-lock rendering.
$surface = [System.Drawing.Bitmap]::new(320,180)
$surfaceGraphics = [System.Drawing.Graphics]::FromImage($surface)
$surfaceGraphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$material = [System.Drawing.Drawing2D.LinearGradientBrush]::new(
    [System.Drawing.Point]::new(0,0),[System.Drawing.Point]::new(320,180),
    [System.Drawing.Color]::FromArgb(245,27,40,53),[System.Drawing.Color]::FromArgb(245,33,39,57))
$surfaceGraphics.FillRectangle($material,0,0,320,180)
$glint = [System.Drawing.Pen]::new([System.Drawing.Color]::FromArgb(34,233,244,255),1)
$surfaceGraphics.DrawBezier($glint,0,33,110,4,210,97,320,35)
$surfaceGraphics.DrawBezier($glint,0,161,120,123,214,189,320,144)
$surfaceGraphics.DrawLine($glint,0,1,319,1)
$surface.Save((Join-Path $assetsRoot 'GlassSurface.png'),[System.Drawing.Imaging.ImageFormat]::Png)
$glint.Dispose(); $material.Dispose(); $surfaceGraphics.Dispose(); $surface.Dispose()
# Picker preview is an illustration, not evidence of the Windows host's final rendering.
$bitmap = New-Object System.Drawing.Bitmap(300,180)
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$background = [System.Drawing.Bitmap]::new((Join-Path $assetsRoot 'GlassSurface.png'))
$graphics.DrawImage($background,0,0,300,180)
$background.Dispose()
$brush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(231,241,255))
$large = New-Object System.Drawing.Font('Segoe UI',30,[System.Drawing.FontStyle]::Bold)
$small = New-Object System.Drawing.Font('Segoe UI',12)
$previewTitle = if ($Candidate) { 'ALARM' } else { 'ALARM (prototype)' }
$graphics.DrawImage($alarmLogo,254,18,28,28)
$graphics.DrawString($previewTitle,$small,$brush,16,12)
$graphics.DrawString('06:00',$large,$brush,16,40)
$graphics.DrawString('Alarm enabled',$small,$brush,16,100)
$graphics.DrawString('Illustrative preview',$small,$brush,16,139)
$bitmap.Save((Join-Path $assetsRoot 'SmallPreview.png'),[System.Drawing.Imaging.ImageFormat]::Png)
$small.Dispose(); $large.Dispose(); $brush.Dispose(); $graphics.Dispose(); $bitmap.Dispose()
} finally { $alarmLogo.Dispose() }
$manifestName = if ($Candidate) { 'CandidateManifest.xml' } else { 'AppxManifest.xml' }
Copy-Item -LiteralPath (Join-Path $prototypeRoot $manifestName) -Destination (Join-Path $packageRoot 'AppxManifest.xml') -Force
Write-Output "Widget provider ready: $packageRoot"
