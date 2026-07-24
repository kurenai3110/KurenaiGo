# KurenaiEngine2Dは矩形スプライトしか描画できないため、石は透過PNGとして事前生成する。
# GDI+(System.Drawing)でアンチエイリアスを効かせた円を描き、Assets/へ出力する。
# 実行例: powershell -ExecutionPolicy Bypass -File Tools\generate_stone_textures.ps1

Add-Type -AssemblyName System.Drawing

$assetsDir = Join-Path $PSScriptRoot "..\Assets"
if (-not (Test-Path $assetsDir)) {
    New-Item -ItemType Directory -Path $assetsDir | Out-Null
}

function New-StonePng {
    param(
        [string]$OutPath,
        [System.Drawing.Color]$FillColor,
        [System.Drawing.Color]$EdgeColor,
        [float]$EdgeWidth
    )

    $size = 128
    $bmp = New-Object -TypeName System.Drawing.Bitmap -ArgumentList $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.Clear([System.Drawing.Color]::Transparent)

    $margin = $EdgeWidth + 2
    $side = $size - (2 * $margin)
    $rect = New-Object -TypeName System.Drawing.RectangleF -ArgumentList $margin, $margin, $side, $side

    $brush = New-Object -TypeName System.Drawing.SolidBrush -ArgumentList $FillColor
    $g.FillEllipse($brush, $rect)

    if ($EdgeWidth -gt 0) {
        $pen = New-Object -TypeName System.Drawing.Pen -ArgumentList $EdgeColor, $EdgeWidth
        $g.DrawEllipse($pen, $rect)
        $pen.Dispose()
    }

    $bmp.Save($OutPath, [System.Drawing.Imaging.ImageFormat]::Png)
    $brush.Dispose()
    $g.Dispose()
    $bmp.Dispose()
}

$blackFill = [System.Drawing.Color]::FromArgb(255, 20, 20, 22)
$blackEdge = [System.Drawing.Color]::Black
New-StonePng -OutPath (Join-Path $assetsDir "stone_black.png") -FillColor $blackFill -EdgeColor $blackEdge -EdgeWidth 0

$whiteFill = [System.Drawing.Color]::FromArgb(255, 250, 250, 248)
$whiteEdge = [System.Drawing.Color]::FromArgb(255, 120, 110, 95)
New-StonePng -OutPath (Join-Path $assetsDir "stone_white.png") -FillColor $whiteFill -EdgeColor $whiteEdge -EdgeWidth 3

Write-Output "SAVED"
