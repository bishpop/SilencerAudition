# Silencer

**Silencer** is a fast, native C++ audio processing and conversion tool built with a modern Dear ImGui interface. It is specifically optimized for rhythm game file preparation (such as Audition), offering automatic LUFS normalization, seamless BPM/Madi calculations, and robust Ogg Vorbis encoding.

Developed by Sanya.

## ✨ Features

* **Smart Audio Encoding:** Convert common audio formats (`.wav`, `.mp3`, `.flac`, `.ogg`) into game-ready Ogg Vorbis or WAV (pcm_s16le).
* **Dynamic Bitrate Control:** Easily toggle between Low, Standard, and High-quality OGG VBR targets (64 kbps to 320 kbps). *Note: 96 kbps is highlighted as the recommended standard for Audition.*
* **LUFS Normalization:** Automatically normalizes output tracks to a strict -8 LUFS target to ensure consistent playback volume across all files.
* **Automated Madi Calculation:** Calculates exact `Start Madi` and `Total Madi` values based on user-provided BPM and timing offsets, formatting the final filename automatically.
* **Drag & Drop Workflow:** No complex menus—just drop your audio file directly into the application window to begin configuration.
* **Unified Diagnostic Log:** Real-time, scrollable console tracking system initialization, file I/O success, offset application, and final output sizes.

## 🚀 How to Use

1. **Configure Settings:** Toggle LUFS Normalization, Madi Calculation, or WAV output mode on the left panel.
2. **Select Bitrate:** Choose your desired OGG Vorbis quality (disabled if WAV output is selected).
3. **Drop a File:** Drag any supported audio file into the dashed dropzone area.
4. **Input Metrics:** If Madi calculation is active, a dialog will prompt you for the manual offset (auto-pasted from clipboard if available), BPM, and Time.
5. **Process:** Silencer will decode, apply offsets, normalize, encode, and save the new file in the exact same directory as the source file.

## 🛠️ Technology Stack

Silencer is lightweight and relies on heavily tested, open-source libraries:

* **Core & UI:** C++, [Dear ImGui](https://github.com/ocornut/imgui) (Win32 + DirectX 11 backend)
* **Audio Decoding/Playback:** [miniaudio](https://miniaudio.net/)
* **OGG Encoding:** [libvorbis](https://xiph.org/vorbis/)
* **Loudness Normalization:** [libebur128](https://github.com/jiixyj/libebur128)
* **Typography:** [Poppins Font](https://fonts.google.com/specimen/Poppins) (SIL Open Font License)

## 🏗️ Building from Source

This project uses CMake. The required third-party libraries (miniaudio, ImGui, etc.) are vendored directly into the repository for straightforward compilation.

1. Clone the repository:
   ```bash
   git clone [https://github.com/bishpop/SilencerAudition.git](https://github.com/bishpop/SilencerAudition.git)
