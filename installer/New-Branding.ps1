# Copyright (c) 2026 Ibrahim Bouillet. SPDX-License-Identifier: MIT
# Generate setup artwork from the existing ALARM icon. No network or image tools.
[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$outputDir = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
$icon = [Drawing.Image]::FromFile((Join-Path $PSScriptRoot '..\assets\alarm.png'))
function New-Canvas([int]$Width, [int]$Height) {
    $bitmap = [Drawing.Bitmap]::new($Width, $Height, [Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    $graphics.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $graphics.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $graphics.TextRenderingHint = [Drawing.Text.TextRenderingHint]::AntiAliasGridFit
    return @($bitmap, $graphics)
}
function Write-Text($Graphics, [string]$Text, [single]$Size, [single]$X, [single]$Y, [Drawing.Color]$Color, [switch]$Bold) {
    $style = if ($Bold) { [Drawing.FontStyle]::Bold } else { [Drawing.FontStyle]::Regular }
    $font = [Drawing.Font]::new('Segoe UI', $Size, $style, [Drawing.GraphicsUnit]::Pixel)
    $brush = [Drawing.SolidBrush]::new($Color)
    try { $Graphics.DrawString($Text, $font, $brush, $X, $Y) }
    finally { $brush.Dispose(); $font.Dispose() }
}
try {
    $side, $g = New-Canvas 492 942
    try {
        $gradient = [Drawing.Drawing2D.LinearGradientBrush]::new([Drawing.Rectangle]::new(0, 0, 492, 942), [Drawing.Color]::FromArgb(17, 63, 43), [Drawing.Color]::FromArgb(7, 27, 21), 68.0)
        $g.FillRectangle($gradient, 0, 0, 492, 942); $gradient.Dispose()
        $line = [Drawing.Pen]::new([Drawing.Color]::FromArgb(50, 107, 225, 161), 2.0)
        $g.DrawEllipse($line, 155, 480, 480, 480)
        $g.DrawEllipse($line, 105, 430, 580, 580)
        $g.DrawEllipse($line, 55, 380, 680, 680)
        $line.Dispose()
        $g.DrawImage($icon, [Drawing.Rectangle]::new(60, 76, 144, 144))
        Write-Text $g 'ALARM' 57 54 262 ([Drawing.Color]::White) -Bold
        Write-Text $g 'Every day,' 28 58 340 ([Drawing.Color]::FromArgb(205, 239, 220))
        Write-Text $g 'at your time.' 28 58 379 ([Drawing.Color]::FromArgb(205, 239, 220))
        $clock = [Drawing.Pen]::new([Drawing.Color]::FromArgb(126, 236, 177), 8.0)
        $clock.StartCap = [Drawing.Drawing2D.LineCap]::Round
        $clock.EndCap = [Drawing.Drawing2D.LineCap]::Round
        $g.DrawEllipse($clock, 211, 583, 160, 160)
        $g.DrawLine($clock, 291, 663, 291, 611)
        $g.DrawLine($clock, 291, 663, 335, 638)
        $clock.Dispose()
        Write-Text $g 'WINDOWS / 64-BIT' 18 58 846 ([Drawing.Color]::FromArgb(153, 205, 174)) -Bold
        $side.Save((Join-Path $outputDir 'wizard-side.bmp'), [Drawing.Imaging.ImageFormat]::Bmp)
    } finally { $g.Dispose(); $side.Dispose() }
    $header, $g = New-Canvas 192 192
    try {
        $g.Clear([Drawing.Color]::FromArgb(16, 41, 31))
        $g.DrawImage($icon, [Drawing.Rectangle]::new(20, 20, 152, 152))
        $header.Save((Join-Path $outputDir 'wizard-icon.bmp'), [Drawing.Imaging.ImageFormat]::Bmp)
    } finally { $g.Dispose(); $header.Dispose() }
} finally { $icon.Dispose() }
