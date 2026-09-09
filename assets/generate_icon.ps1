Add-Type -AssemblyName System.Drawing

$dir = Join-Path (Get-Location).Path 'assets'
if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }

$w = 256
$bmp = New-Object System.Drawing.Bitmap $w, $w
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$g.Clear([System.Drawing.Color]::Transparent)

function Rounded($x, $y, $w, $h, $r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = [float]$r
    $p.AddArc($x, $y, $d, $d, 180, 90)
    $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure()
    return $p
}

$grad = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
    [System.Drawing.Point]::new(0, 0),
    [System.Drawing.Point]::new($w, $w),
    [System.Drawing.Color]::FromArgb(255, 30, 90, 220),
    [System.Drawing.Color]::FromArgb(255, 20, 50, 160))
$g.FillPath($grad, (Rounded 8 8 ($w - 16) ($w - 16) 56))

$eyeBrush = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::White)
$g.FillEllipse($eyeBrush, 40, 84, 76, 76)
$g.FillEllipse($eyeBrush, 140, 84, 76, 76)
$pupil = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(255, 30, 90, 220))
$g.FillEllipse($pupil, 62, 106, 32, 32)
$g.FillEllipse($pupil, 162, 106, 32, 32)

$icon = [System.Drawing.Icon]::FromHandle($bmp.GetHicon())
$out = $dir + '\icon.ico'
$fs = [System.IO.File]::Create($out)
$icon.Save($fs)
$fs.Close()
$bmp.Dispose(); $g.Dispose()
Write-Output ("saved: " + $out)