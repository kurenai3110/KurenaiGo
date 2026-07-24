# 着手音(stone_place.wav)・対局終了音(game_end.wav)を、外部音源を使わずに
# 正弦波の合成のみで生成するスクリプト。16bit PCMモノラルのWAVファイルとして
# Assets\Sounds\へ出力する(実行にはビルド後処理で同フォルダがexeと同じ場所へコピーされる)。
#
# 実行方法: powershell -ExecutionPolicy Bypass -File Tools\generate_sound_effects.ps1

$ErrorActionPreference = "Stop"

$sampleRate = 44100
$outputDir = Join-Path $PSScriptRoot "..\Assets\Sounds"
if (-not (Test-Path $outputDir)) {
    New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
}

function Write-WavFile {
    param(
        [string]$Path,
        [double[]]$Samples # -1.0 .. 1.0
    )

    $bitsPerSample = 16
    $channels = 1
    $byteRate = $sampleRate * $channels * $bitsPerSample / 8
    $blockAlign = $channels * $bitsPerSample / 8
    $dataSize = $Samples.Length * 2
    $riffSize = 36 + $dataSize

    $stream = New-Object System.IO.FileStream($Path, [System.IO.FileMode]::Create)
    $writer = New-Object System.IO.BinaryWriter($stream)
    try {
        $writer.Write([System.Text.Encoding]::ASCII.GetBytes("RIFF"))
        $writer.Write([int32]$riffSize)
        $writer.Write([System.Text.Encoding]::ASCII.GetBytes("WAVE"))

        $writer.Write([System.Text.Encoding]::ASCII.GetBytes("fmt "))
        $writer.Write([int32]16)
        $writer.Write([int16]1)  # PCM
        $writer.Write([int16]$channels)
        $writer.Write([int32]$sampleRate)
        $writer.Write([int32]$byteRate)
        $writer.Write([int16]$blockAlign)
        $writer.Write([int16]$bitsPerSample)

        $writer.Write([System.Text.Encoding]::ASCII.GetBytes("data"))
        $writer.Write([int32]$dataSize)
        foreach ($sample in $Samples) {
            $clamped = [Math]::Max(-1.0, [Math]::Min(1.0, $sample))
            $writer.Write([int16]($clamped * 32767))
        }
    }
    finally {
        $writer.Close()
        $stream.Close()
    }
}

function Add-DecayingTone {
    param(
        [double[]]$Buffer,
        [int]$StartSample,
        [double]$DurationSeconds,
        [double]$Frequency,
        [double]$DecayRate, # 大きいほど速く減衰する
        [double]$Amplitude
    )

    $count = [int]($DurationSeconds * $sampleRate)
    for ($i = 0; $i -lt $count; $i++) {
        $t = $i / $sampleRate
        $index = $StartSample + $i
        if ($index -ge $Buffer.Length) { break }
        $envelope = [Math]::Exp(-$DecayRate * $t)
        $Buffer[$index] += $Amplitude * $envelope * [Math]::Sin(2 * [Math]::PI * $Frequency * $t)
    }
}

# 着手音: 木の碁石が打たれる「パチッ」という音を、高めの周波数2本の急速減衰する
# 正弦波の合成で近似する(実際の碁石の打音を録音したものではなく、電子的に合成した音)
$placeDuration = 0.12
$placeSamples = New-Object double[] ([int]($placeDuration * $sampleRate) + 1)
Add-DecayingTone -Buffer $placeSamples -StartSample 0 -DurationSeconds $placeDuration -Frequency 2200 -DecayRate 55 -Amplitude 0.55
Add-DecayingTone -Buffer $placeSamples -StartSample 0 -DurationSeconds $placeDuration -Frequency 3400 -DecayRate 70 -Amplitude 0.35
Write-WavFile -Path (Join-Path $outputDir "stone_place.wav") -Samples $placeSamples

# 対局終了音: 上昇する2音のチャイム(C5 -> G5)
$totalDuration = 0.55
$endSamples = New-Object double[] ([int]($totalDuration * $sampleRate) + 1)
Add-DecayingTone -Buffer $endSamples -StartSample 0 -DurationSeconds 0.30 -Frequency 523.25 -DecayRate 6 -Amplitude 0.5
Add-DecayingTone -Buffer $endSamples -StartSample ([int](0.15 * $sampleRate)) -DurationSeconds 0.40 -Frequency 783.99 -DecayRate 5 -Amplitude 0.5
Write-WavFile -Path (Join-Path $outputDir "game_end.wav") -Samples $endSamples

Write-Output "Generated: $outputDir\stone_place.wav, $outputDir\game_end.wav"
