<#
  24x24 汉字点阵取模脚本（生成 font_cn_data.c 和预览图）

  用法（PowerShell 7）：
      pwsh -File make_cn_font.ps1
      pwsh -File make_cn_font.ps1 -Chars "温度湿度"        # 只生成指定的字

  说明：
    * 用 Windows 自带字体（默认黑体 SimHei）把字渲染成 1bpp 点阵；
      数据格式：行优先、每行 3 字节、MSB 在左，共 24*3 = 72 字节/字。
    * 同时输出 cn_preview.png，可直接看点阵对不对。
    * 生成的文件用 GBK 编码保存，与工程内其它文件一致（Keil ARMCC v5 按 GBK 解析）。
#>
param(
    [string]$Chars  = '维特智能模块横滚俯仰航向加速度角温串口电脑液晶秒正在查找已到未请检连接静止校准数据显示中文字库无，：传感器采集样频率周期时缓存储读写上报输出入波特命令帮助正常异等待成功失败断开开始停止保存清除复位陀螺磁场四元数欧拉偏姿态原始值换算标定单位通讯完成错误设备地址寄存器号位组包帧头校验→全内减只回增大容宽小带束植版移结压本',
    [int]$Cell      = 24,
    [string]$FontName = 'SimHei',
    [string]$OutC   = '',
    [string]$OutPng = ''
)

Add-Type -AssemblyName System.Drawing

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
if ([string]::IsNullOrEmpty($OutC))   { $OutC   = Join-Path $here 'font_cn_data.c' }
if ([string]::IsNullOrEmpty($OutPng)) { $OutPng = Join-Path $here 'cn_preview.png' }

$gbk = [System.Text.Encoding]::GetEncoding(936)

# ---------- 渲染单个字，返回 24x24 的位数组（$true = 亮点） ----------
function Get-GlyphBits {
    param([string]$Ch, [int]$Cell, [string]$FontName)

    $canvas = $Cell * 2
    $size = 23
    $bmp = $null
    $inkBox = $null

    while ($size -ge 14) {
        $bmp = New-Object System.Drawing.Bitmap($canvas, $canvas)
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.Clear([System.Drawing.Color]::Black)
        $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::SingleBitPerPixelGridFit
        $f = New-Object System.Drawing.Font($FontName, [float]$size, [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)
        $g.DrawString($Ch, $f, [System.Drawing.Brushes]::White, 2, 2)
        $g.Dispose(); $f.Dispose()

        # 找墨迹包围盒
        $minX = $canvas; $minY = $canvas; $maxX = -1; $maxY = -1
        for ($y = 0; $y -lt $canvas; $y++) {
            for ($x = 0; $x -lt $canvas; $x++) {
                if ($bmp.GetPixel($x, $y).R -gt 127) {
                    if ($x -lt $minX) { $minX = $x }
                    if ($y -lt $minY) { $minY = $y }
                    if ($x -gt $maxX) { $maxX = $x }
                    if ($y -gt $maxY) { $maxY = $y }
                }
            }
        }

        if ($maxX -ge 0) {
            $w = $maxX - $minX + 1
            $h = $maxY - $minY + 1
            if (($w -le $Cell) -and ($h -le $Cell)) { $inkBox = @($minX, $minY, $w, $h); break }
        }
        $bmp.Dispose()
        $size--
    }

    $bits = [bool[,]]::new($Cell, $Cell)
    if ($inkBox) {
        $offX = [int](($Cell - $inkBox[2]) / 2)
        $offY = [int](($Cell - $inkBox[3]) / 2)
        if ($offY -lt 0) { $offY = 0 }
        for ($y = 0; $y -lt $inkBox[3]; $y++) {
            for ($x = 0; $x -lt $inkBox[2]; $x++) {
                if ($bmp.GetPixel($inkBox[0] + $x, $inkBox[1] + $y).R -gt 127) {
                    $bits[($y + $offY), ($x + $offX)] = $true
                }
            }
        }
    }
    if ($bmp) { $bmp.Dispose() }
    return ,$bits          # 逗号：防止 PowerShell 把二维数组展平
}

# ---------- 逐字取模 ----------
$list = @()
foreach ($ch in $Chars.ToCharArray()) {
    if ([int]$ch -lt 0x80) { continue }                  # 只用汉字，ASCII 走 font24
    $codeBytes = $gbk.GetBytes([string]$ch)
    if ($codeBytes.Length -ne 2) { continue }
    $code = ([int]$codeBytes[0] -shl 8) -bor [int]$codeBytes[1]
    if ($list | Where-Object { $_.Code -eq $code }) { continue }   # 去重
    $list += [pscustomobject]@{ Char = $ch; Code = $code; Bits = (Get-GlyphBits -Ch ([string]$ch) -Cell $Cell -FontName $FontName) }
}

Write-Host ("取模完成：共 " + $list.Count + " 个字")

# ---------- 生成 C 文件 ----------
$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine('/**')
[void]$sb.AppendLine('  ******************************************************************************')
[void]$sb.AppendLine('  * @file    font_cn_data.c')
[void]$sb.AppendLine('  * @brief   LCD 中文点阵数据（24x24，行优先，每行 3 字节，MSB 在左）')
[void]$sb.AppendLine('  *          本文件由 User/fonts/make_cn_font.ps1 自动生成，请勿手工修改。')
[void]$sb.AppendLine('  *          要增加汉字：改脚本的 -Chars 参数后重新运行即可。')
[void]$sb.AppendLine('  ******************************************************************************')
[void]$sb.AppendLine('  */')
[void]$sb.AppendLine('')
[void]$sb.AppendLine('#include "./fonts/font_cn.h"')
[void]$sb.AppendLine('')

$idx = 0
foreach ($item in $list) {
    [void]$sb.AppendLine(("/* '{0}'  GBK=0x{1:X4}  {2}/72 字节 */" -f $item.Char, $item.Code, ($idx * 72)))
    [void]$sb.AppendLine(("static const uint8_t CN_G{0:D3}[CN_FONT_HEIGHT * CN_FONT_BYTES_PER_ROW] = {{" -f $idx))
    for ($y = 0; $y -lt $Cell; $y++) {
        $bytes = @(0, 0, 0)
        for ($x = 0; $x -lt $Cell; $x++) {
            if ($item.Bits[$y, $x]) {
                $bi = [int][math]::Floor($x / 8)
                $bytes[$bi] = $bytes[$bi] -bor (0x80 -shr ($x % 8))
            }
        }
        [void]$sb.AppendLine(("  {0}, {1}, {2}," -f $bytes[0], $bytes[1], $bytes[2]))
    }
    [void]$sb.AppendLine('};')
    [void]$sb.AppendLine('')
    $idx++
}

[void]$sb.AppendLine('const CN_GlyphTypeDef g_cnGlyphTable[] = {')
$idx = 0
foreach ($item in $list) {
    [void]$sb.AppendLine(("  {{ 0x{0:X4}, CN_G{1:D3} }},   /* '{2}' */" -f $item.Code, $idx, $item.Char))
    $idx++
}
[void]$sb.AppendLine('};')
[void]$sb.AppendLine('')
[void]$sb.AppendLine('const uint16_t g_cnGlyphCount = sizeof(g_cnGlyphTable) / sizeof(g_cnGlyphTable[0]);')
[void]$sb.AppendLine('')
[void]$sb.AppendLine('/************************ (C) COPYRIGHT *****END OF FILE************************/')

[System.IO.File]::WriteAllText($OutC, $sb.ToString(), $gbk)
Write-Host ("已生成: " + $OutC)

# ---------- 生成预览图 ----------
$scale = 4
$colMax = 10
$rows = [int][math]::Ceiling($list.Count / $colMax)
$pad = 6
$cw = $Cell * $scale + $pad * 2
$chh = $Cell * $scale + $pad * 2 + 14
$png = New-Object System.Drawing.Bitmap(($cw * $colMax), ($chh * $rows))
$pg = [System.Drawing.Graphics]::FromImage($png)
$pg.Clear([System.Drawing.Color]::FromArgb(30, 30, 30))
$pen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(70, 70, 70))
$font = New-Object System.Drawing.Font('Consolas', 9, [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)

$k = 0
foreach ($item in $list) {
    $cx = ($k % $colMax) * $cw
    $cy = [int][math]::Floor($k / $colMax) * $chh
    $pg.FillRectangle([System.Drawing.Brushes]::Black, ($cx + $pad), ($cy + $pad), ($Cell * $scale), ($Cell * $scale))
    for ($y = 0; $y -lt $Cell; $y++) {
        for ($x = 0; $x -lt $Cell; $x++) {
            if ($item.Bits[$y, $x]) {
                $pg.FillRectangle([System.Drawing.Brushes]::White, ($cx + $pad + $x * $scale), ($cy + $pad + $y * $scale), $scale, $scale)
            }
        }
    }
    $pg.DrawRectangle($pen, ($cx + $pad), ($cy + $pad), ($Cell * $scale), ($Cell * $scale))
    $pg.DrawString(("#{0}" -f $k), $font, [System.Drawing.Brushes]::LightGreen, ($cx + $pad), ($cy + $pad + $Cell * $scale + 1))
    $k++
}
$pg.Dispose()
$png.Save($OutPng, [System.Drawing.Imaging.ImageFormat]::Png)
$png.Dispose()
Write-Host ("已生成预览图: " + $OutPng)
