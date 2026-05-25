<div align="center">

<img src="assets/silencer.png" width="80" alt="Silencer Icon"/>

# Silencer
### Audio Conversion Tool for Audition — v1.5

[![Download](https://img.shields.io/badge/Download-v1.3.2-a6e3a1?style=for-the-badge&logo=github)](https://github.com/0x53616E/Silencer/releases/download/v1.3.2/Silencer.v1.3.2.exe)
[![Website](https://img.shields.io/badge/Website-sanyaproject-b4befe?style=for-the-badge)](https://tinyurl.com/sanyaproject)
[![Platform](https://img.shields.io/badge/Platform-Windows%2010%2F11-0078D4?style=for-the-badge&logo=windows)](https://github.com/0x53616E/Silencer)
[![.NET Framework](https://img.shields.io/badge/.NET%20Framework-4.8-512BD4?style=for-the-badge&logo=dotnet)](https://dotnet.microsoft.com/en-us/download/dotnet-framework/net48)
[![Language](https://img.shields.io/badge/Language-C%23-239120?style=for-the-badge&logo=csharp)](https://github.com/0x53616E/Silencer)
[![FFmpeg](https://img.shields.io/badge/Requires-FFmpeg-007808?style=for-the-badge&logo=ffmpeg)](https://www.gyan.dev/ffmpeg/builds/)
[![License](https://img.shields.io/badge/License-MIT-cba6f7?style=for-the-badge)](https://github.com/0x53616E/Silencer/blob/main/LICENSE)

</div>

---

## Table of Contents

- [Installation](#1-installation)
- [Usage](#2-usage)
  - [First Launch](#first-launch)
  - [Converting to WAV](#converting-to-wav)
  - [Converting WAV to OGG](#converting-wav-to-ogg)
  - [Finding Offset, BPM & First Space](#finding-offset-bpm--first-space)
- [Reading the Output](#3-reading-the-output)
- [Requirements](#requirements)

---

## 1. Installation

Download the latest release from one of the following sources:

| Source | Link |
|--------|------|
| **GitHub Releases** | [Silencer.v1.4.exe](https://github.com/0x53616E/Silencer/releases/download/1.4/Silencer.v1.4.exe) |
| **Website** | [tinyurl.com/sanyaproject](https://tinyurl.com/sanyaproject) |

No installation required — just run the `.exe` directly.

<img src="assets/desktop_icon.jpg" alt="Desktop Icon" width="120"/>

---

## 2. Usage

### First Launch

On the very first launch, Silencer will ask you to enter your **Creditor name**. This name will be displayed in the bottom-left corner of the UI for the lifetime of the application.

<img src="assets/first_launch.jpg" alt="First Launch - Creditor Input"/>

Once entered, you will see the main interface:

<img src="assets/user_interface.jpg" alt="User Interface"/>

---

### Converting to WAV

Silencer supports the following input formats:

| Format | Action |
|--------|--------|
| `.wav` | Converted directly to `.ogg` |
| `.flac` `.mp3` `.ogg` | **Automatically converted to `.wav` first** |

> **Note:** Silencing (WAV → OGG with MADI values) is only possible from a `.wav` file.  
> All other formats will be converted to `.wav` automatically — simply drag in the new `.wav` afterwards.

**To start a conversion**, either:
- Drag & drop any supported file onto the window, or
- Click the **Silence File** button and select a file

<img src="assets/conversion_to_wav.jpg" alt="Conversion to WAV"/>

---

### Converting WAV to OGG

Select or drop your `.wav` file. If **Calculate MADI Values** is enabled *(recommended)*, the tool will ask for three values before processing:

#### 1. Manual Offset
The audio offset in seconds (e.g. `-0.078`).

<img src="assets/manual_offset.jpg" alt="Manual Offset Input"/>

#### 2. BPM
The tempo of the track.

<img src="assets/bpm.jpg" alt="BPM Input"/>

#### 3. First Space Value
The position of the first beat/space.

<img src="assets/start_madi.jpg" alt="First Space Input"/>

After confirming all values, Silencer will generate a new `.ogg` file in the **same folder** as your source `.wav`.

<img src="assets/output.jpg" alt="Output Console"/>

---

### Finding Offset, BPM & First Space

All three values can be read directly from **ArrowVortex**:

<img src="assets/AV_Overview.jpg" alt="ArrowVortex Overview"/>

| Value | Where to find it in ArrowVortex |
|-------|--------------------------------|
| **Offset** | Song offset field (top bar) |
| **BPM** | BPM field (top bar) |
| **First Space** | Beat position of the first measure |

---

## 3. Reading the Output

The output filename follows this pattern:

```
S_<original_name> <BPM> <StartMadi> <TotalMadi>.ogg
```

Example: `S_MySong 128 1 64.ogg`

<img src="assets/final_song.jpg" alt="Final Output File"/>

| Part | Description |
|------|-------------|
| `S_` | Prefix added by Silencer |
| `BPM` | Tempo used during conversion |
| `StartMadi` | Calculated start MADI value |
| `TotalMadi` | Calculated total MADI value |

---

## Requirements

| Requirement | Details |
|-------------|---------|
| **OS** | Windows 10 / 11 |
| **FFmpeg** | Must be on your `C:\` drive **or** in the same folder as `Silencer.exe` |

**Download FFmpeg (Windows):** [gyan.dev/ffmpeg/builds](https://www.gyan.dev/ffmpeg/builds/)

> Silencer will automatically search for `ffmpeg.exe` — no manual path configuration needed.

---

<div align="center">

Copyright © 2026 Sanya. All rights reserved.

</div>
