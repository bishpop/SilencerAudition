// =============================================================
//  Silencer v1.6  –  C++ / Dear ImGui  (Win32 + DirectX 11)
//  Theme: Modern Dashboard (Wide 2-Column, Mint Accent)
// =============================================================

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <d3d11.h>
#include <dxgi.h>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#if __has_include("font_poppins_regular.h") && __has_include("font_poppins_semibold.h")
#include "font_poppins_regular.h"
#include "font_poppins_semibold.h"
#define FONTS_EMBEDDED
#endif

#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <ctime>
#include <regex>
#include <algorithm>
#include <filesystem>

// ── Native Audio Engine ───────────────────────────────────────
#pragma warning(push)
#pragma warning(disable: 4244)
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#pragma warning(pop)

#include "ebur128.h"
#include <vorbis/vorbisenc.h>

namespace fs = std::filesystem;

#define UI_BG        IM_COL32( 15,  15,  18, 255)
#define UI_PANEL     IM_COL32( 24,  24,  28, 255)
#define UI_BORDER    IM_COL32( 45,  45,  50, 255)
#define UI_TEXT      IM_COL32(240, 240, 240, 255)
#define UI_SUBTEXT   IM_COL32(140, 140, 145, 255)
#define UI_MINT      IM_COL32( 75, 200, 145, 255) 
#define UI_MINT_DK   IM_COL32( 55, 170, 120, 255)
#define UI_RED       IM_COL32(235,  90,  90, 255)
#define UI_WARN      IM_COL32(235, 180,  90, 255)

static constexpr ImVec4 V_MINT    = {75/255.f, 200/255.f, 145/255.f, 1.f};
static constexpr ImVec4 V_TEXT    = {240/255.f, 240/255.f, 240/255.f, 1.f};
static constexpr ImVec4 V_SUBTEXT = {140/255.f, 140/255.f, 145/255.f, 1.f};
static constexpr ImVec4 V_RED     = {235/255.f, 90/255.f, 90/255.f, 1.f};
static constexpr ImVec4 V_WARN    = {235/255.f, 180/255.f, 90/255.f, 1.f};

enum class DialogStep { None, WaitingOffset, WaitingBpm, WaitingTime };
struct AppState {
    bool applyLufs = true; bool calcMadi = true; bool wavMode = false;
    int activeBitrate = 96;
    std::mutex statusMtx; std::string statusMsg = "Ready";
    void setStatus(const std::string& s) { std::lock_guard l(statusMtx); statusMsg = s; }
    std::string getStatus() { std::lock_guard l(statusMtx); return statusMsg; }
    std::mutex resMtx; double displayBpm = 0; long long displaySM = 0; long long displayTM = 0; bool hasResults = false;
    void setResults(double b, long long s, long long t) { std::lock_guard l(resMtx); displayBpm = b; displaySM = s; displayTM = t; hasResults = true; }
    void clearResults() { std::lock_guard l(resMtx); hasResults = false; }
    std::atomic<bool> busy{false};
    std::atomic<bool> dialogOpen{false}; std::atomic<bool> dialogConfirmed{false}; std::atomic<bool> dialogCancelled{false};
    DialogStep dialogStep{DialogStep::None}; char dialogBuf[64]{}; std::string dialogQuestion;
    double resultOffset=0, resultBpm=120, resultTime=0; std::thread worker;
};
static AppState g_app;

enum class DiagStatus { Unknown, OK, Warning, Error, Info };
struct DiagEntry { std::string label; std::string detail; DiagStatus status = DiagStatus::Unknown; };
struct Diagnostics {
    std::vector<DiagEntry> entries;
    bool hasError  = false;
    std::string timestamp;
    std::mutex mtx;
    bool autoScroll = true;

    void clear() { 
        std::lock_guard lock(mtx);
        entries.clear(); hasError = false; timestamp.clear(); 
    }
    void add(const std::string& l, const std::string& d, DiagStatus s) {
        std::lock_guard lock(mtx);
        entries.push_back({l, d, s});
        if (s == DiagStatus::Error) hasError = true;
        autoScroll = true;
    }
};
static Diagnostics g_diag;

// ──────────────────────────────────────────────────────────────
//  Audio Processing Functions
// ──────────────────────────────────────────────────────────────
struct NativeAudioData {
    std::vector<float> pcm; 
    int channels = 2;
    int sampleRate = 44100;
};

static bool LoadAudioToRAM(const std::string& filepath, NativeAudioData& outAudio, double startOffsetSec, double durationSec) {
    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, outAudio.channels, outAudio.sampleRate);
    ma_decoder decoder;
    if (ma_decoder_init_file(filepath.c_str(), &config, &decoder) != MA_SUCCESS) return false;

    if (startOffsetSec < 0) {
        ma_uint64 skipFrames = (ma_uint64)(std::abs(startOffsetSec) * 44100.0);
        ma_decoder_seek_to_pcm_frame(&decoder, skipFrames);
    }

    ma_uint64 framesToRead = (durationSec > 0) ? (ma_uint64)(durationSec * 44100.0) : (ma_uint64)-1;
    ma_uint64 framesRead = 0;
    outAudio.pcm.resize(framesToRead != (ma_uint64)-1 ? framesToRead * 2 : 10 * 1024 * 1024);
    
    float temp[4096 * 2];
    while (true) {
        ma_uint64 read = 0;
        ma_decoder_read_pcm_frames(&decoder, temp, 4096, &read);
        if (read == 0) break;

        ma_uint64 available = framesToRead != (ma_uint64)-1 ? framesToRead - framesRead : read;
        ma_uint64 toCopy = std::min(read, available);

        if (framesRead * 2 + toCopy * 2 > outAudio.pcm.size()) outAudio.pcm.resize(outAudio.pcm.size() * 2);
        std::memcpy(&outAudio.pcm[framesRead * 2], temp, toCopy * 2 * sizeof(float));
        framesRead += toCopy;

        if (framesToRead != (ma_uint64)-1 && framesRead >= framesToRead) break;
    }

    outAudio.pcm.resize(framesRead * 2);
    ma_decoder_uninit(&decoder);

    if (startOffsetSec > 0) {
        size_t silenceSamples = (size_t)(startOffsetSec * 44100.0) * 2;
        outAudio.pcm.insert(outAudio.pcm.begin(), silenceSamples, 0.0f);
    }
    return true;
}

static void ApplyLufsNormalization(NativeAudioData& audio) {
    size_t frames = audio.pcm.size() / audio.channels;
    ebur128_state* st = ebur128_init(audio.channels, audio.sampleRate, EBUR128_MODE_I);
    ebur128_add_frames_float(st, audio.pcm.data(), frames);
    double loudness = 0.0;
    ebur128_loudness_global(st, &loudness);
    ebur128_destroy(&st);

    if (loudness != 0.0 && loudness > -70.0) {
        double gainDb = -8.0 - loudness;
        float gainLinear = (float)std::pow(10.0, gainDb / 20.0);
        for (float& sample : audio.pcm) sample *= gainLinear;
    }
}

static bool EncodeToOgg(const NativeAudioData& audio, const std::string& outPath) {
    FILE* out = fopen(outPath.c_str(), "wb");
    if (!out) return false;

    vorbis_info vi;
    vorbis_info_init(&vi);
    
    float vbrQuality = 0.4f; 
    switch (g_app.activeBitrate) {
        case 64:  vbrQuality = 0.0f; break;
        case 96:  vbrQuality = 0.2f; break;
        case 128: vbrQuality = 0.4f; break;
        case 160: vbrQuality = 0.5f; break;
        case 192: vbrQuality = 0.6f; break;
        case 256: vbrQuality = 0.8f; break;
        case 320: vbrQuality = 0.9f; break;
    }

    if (vorbis_encode_init_vbr(&vi, audio.channels, audio.sampleRate, vbrQuality) != 0) {
        vorbis_info_clear(&vi); fclose(out); return false;
    }

    vorbis_comment  vc; vorbis_comment_init(&vc);
    vorbis_dsp_state vd; vorbis_analysis_init(&vd, &vi);
    vorbis_block    vb; vorbis_block_init(&vd, &vb);

    ogg_stream_state os; ogg_stream_init(&os, rand());
    ogg_page  og;
    ogg_packet op;

    {
        ogg_packet header, header_comm, header_code;
        vorbis_analysis_headerout(&vd, &vc, &header, &header_comm, &header_code);
        ogg_stream_packetin(&os, &header);
        ogg_stream_packetin(&os, &header_comm);
        ogg_stream_packetin(&os, &header_code);
        while (ogg_stream_flush(&os, &og)) {
            fwrite(og.header, 1, og.header_len, out);
            fwrite(og.body,   1, og.body_len,   out);
        }
    }

    auto drain = [&]() {
        while (vorbis_analysis_blockout(&vd, &vb) == 1) {
            vorbis_analysis(&vb, nullptr);
            vorbis_bitrate_addblock(&vb);
            while (vorbis_bitrate_flushpacket(&vd, &op)) {
                ogg_stream_packetin(&os, &op);
                while (ogg_stream_pageout(&os, &og)) {
                    fwrite(og.header, 1, og.header_len, out);
                    fwrite(og.body,   1, og.body_len,   out);
                }
            }
        }
    };

    const int chunkSize   = 1024;
    int totalFrames       = (int)(audio.pcm.size() / audio.channels);
    int framesWritten     = 0;

    while (framesWritten < totalFrames) {
        int writeFrames   = std::min(chunkSize, totalFrames - framesWritten);
        float** buffer    = vorbis_analysis_buffer(&vd, writeFrames);
        for (int i = 0; i < writeFrames; ++i)
            for (int c = 0; c < audio.channels; ++c)
                buffer[c][i] = audio.pcm[(framesWritten + i) * audio.channels + c];
        vorbis_analysis_wrote(&vd, writeFrames);
        framesWritten += writeFrames;
        drain();
    }

    vorbis_analysis_wrote(&vd, 0);
    drain();

    while (ogg_stream_flush(&os, &og)) {
        fwrite(og.header, 1, og.header_len, out);
        fwrite(og.body,   1, og.body_len,   out);
    }

    ogg_stream_clear(&os);
    vorbis_block_clear(&vb);
    vorbis_dsp_clear(&vd);
    vorbis_comment_clear(&vc);
    vorbis_info_clear(&vi);
    fclose(out);
    return true;
}

static bool EncodeToWav(const NativeAudioData& audio, const std::string& outPath) {
    ma_encoder_config config = ma_encoder_config_init(ma_encoding_format_wav, ma_format_s16, audio.channels, audio.sampleRate);
    ma_encoder encoder;
    if (ma_encoder_init_file(outPath.c_str(), &config, &encoder) != MA_SUCCESS) return false;

    std::vector<short> s16(audio.pcm.size());
    for (size_t i = 0; i < audio.pcm.size(); ++i) {
        float v = std::clamp(audio.pcm[i], -1.0f, 1.0f);
        s16[i] = (short)(v * 32767.0f);
    }

    ma_uint64 framesWritten = 0;
    ma_encoder_write_pcm_frames(&encoder, s16.data(), s16.size() / audio.channels, &framesWritten);
    ma_encoder_uninit(&encoder);
    return true;
}

static std::string FmtBpm(double bpm) {
    if (std::abs(std::fmod(bpm, 1.0)) < 1e-9) return std::to_string((long long)bpm);
    std::ostringstream o; o << std::fixed << std::setprecision(3) << bpm;
    std::string s = o.str(); s.erase(s.find_last_not_of('0') + 1);
    if (s.back() == '.') s.pop_back(); return s;
}

static void ApplyMadiParity(long long start, long long& total, double bpm, double& trimSec) {
    if ((start % 2 == 0) == (total % 2 == 0)) --total;
    trimSec = (double)total * 4.0 * 60.0 / bpm;
}

static std::string ClipboardDefaultOffset() {
    std::string def = "0.0";
    if (!OpenClipboard(nullptr)) return def;
    HANDLE h = GetClipboardData(CF_TEXT);
    if (h) {
        char* t = (char*)GlobalLock(h);
        if (t) {
            try {
                SIZE_T size = GlobalSize(h);
                std::string clip(t, strnlen(t, size));
                std::regex re(R"(-?\d+[.,]\d+)");
                std::smatch m;
                if (std::regex_search(clip, m, re)) {
                    def = m[0].str();
                    std::replace(def.begin(), def.end(), ',', '.');
                }
            } catch (...) {} 
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return def;
}

static std::string OpenFileDialog() {
    char buf[MAX_PATH]{};
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = "Audio Files\0*.flac;*.wav;*.ogg;*.mp3\0\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
    ofn.lpstrTitle = "Select Audio File";
    if (GetOpenFileNameA(&ofn)) return buf;
    return "";
}

static bool PromptValue(const std::string& q, const std::string& def, DialogStep step, double& out) {
    g_app.dialogQuestion = q;
    snprintf(g_app.dialogBuf, sizeof(g_app.dialogBuf), "%s", def.c_str());
    g_app.dialogStep = step;
    g_app.dialogConfirmed = false;
    g_app.dialogCancelled = false;
    
    g_app.dialogOpen.store(true, std::memory_order_release);
    while (g_app.dialogOpen.load(std::memory_order_acquire)) { Sleep(30); }
    
    if (g_app.dialogCancelled) return false;
    if (step == DialogStep::WaitingOffset) out = g_app.resultOffset;
    else if (step == DialogStep::WaitingBpm) out = g_app.resultBpm;
    else out = g_app.resultTime;
    
    return true;
}

static void ProcessFile(std::string inputFile) {
    g_app.busy = true; g_app.clearResults(); g_app.setStatus("Configuring...");
    fs::path p(inputFile);
    
    g_diag.add("---", "--------------------------------------", DiagStatus::Info);
    g_diag.add("Task", "Processing: " + p.filename().string(), DiagStatus::Info);

    double offset = 0.0;
    if (!PromptValue("Enter Manual Offset (e.g. 1.234)", ClipboardDefaultOffset(), DialogStep::WaitingOffset, offset)) {
        g_app.setStatus("Cancelled."); 
        g_diag.add("Task", "Operation cancelled by user.", DiagStatus::Warning);
        g_app.busy = false; return;
    }
    
    double bpm = 120.0, time = 0.0;
    if (g_app.calcMadi) {
        if (!PromptValue("Enter BPM", "120.0", DialogStep::WaitingBpm, bpm)) {
            g_app.setStatus("Cancelled.");
            g_diag.add("Task", "Operation cancelled by user.", DiagStatus::Warning);
            g_app.busy = false; return;
        }
        if (!PromptValue("Enter Time (First Space)", "0.0", DialogStep::WaitingTime, time)) {
            g_app.setStatus("Cancelled."); 
            g_diag.add("Task", "Operation cancelled by user.", DiagStatus::Warning);
            g_app.busy = false; return;
        }
    }

    double trimSec = -1.0;
    std::string outName = p.stem().string();
    
    if (g_app.calcMadi) {
        ma_decoder tempDec;
        if (ma_decoder_init_file(inputFile.c_str(), nullptr, &tempDec) != MA_SUCCESS) {
            g_diag.add("Error", "Could not read file length for Madi calculation.", DiagStatus::Error);
            g_app.setStatus("Error."); g_app.busy = false; return;
        }
        ma_uint64 len; ma_decoder_get_length_in_pcm_frames(&tempDec, &len);
        double songLenSec = (double)len / tempDec.outputSampleRate;
        ma_decoder_uninit(&tempDec);

        long long sm = (long long)std::ceil((bpm / 60.0) * (time + offset) / 4.0 - 1.0);
        long long tm = (long long)std::ceil((bpm * songLenSec) / (4.0 * 60.0));
        ApplyMadiParity(sm, tm, bpm, trimSec);
        g_app.setResults(bpm, sm, tm);
        
        outName = "S_" + p.stem().string() + "_" + FmtBpm(bpm) + "_" + std::to_string(sm) + "_" + std::to_string(tm);
        g_diag.add("Madi", "Tempo: " + FmtBpm(bpm) + " BPM | SM: " + std::to_string(sm) + " | TM: " + std::to_string(tm), DiagStatus::OK);
    } else {
        outName = "edited_" + outName;
    }

    std::string outFile = p.parent_path().string() + "\\" + outName + (g_app.wavMode ? ".wav" : ".ogg");
    
    if (g_app.wavMode) {
        g_diag.add("Config", "Output format set to WAV (pcm_s16le)", DiagStatus::Info);
    } else {
        g_diag.add("Config", "Output format set to Ogg Vorbis (" + std::to_string(g_app.activeBitrate) + " kbps)", DiagStatus::Info);
    }

    g_app.setStatus("Decoding & applying offset...");
    NativeAudioData audio;
    if (!LoadAudioToRAM(inputFile, audio, offset, trimSec)) {
        g_diag.add("Error", "Failed to decode audio file.", DiagStatus::Error);
        g_app.setStatus("Decode Error."); g_app.busy = false; return;
    }
    g_diag.add("Audio", "Decoded source and applied offset (" + std::to_string(offset) + "s)", DiagStatus::OK);

    if (g_app.applyLufs) {
        g_app.setStatus("Analyzing LUFS..."); 
        ApplyLufsNormalization(audio);
        g_diag.add("Audio", "LUFS Normalization (-8) applied", DiagStatus::OK);
    }

    g_app.setStatus("Encoding output...");
    bool success = g_app.wavMode ? EncodeToWav(audio, outFile) : EncodeToOgg(audio, outFile);

    if (success) {
        std::error_code ec;
        uintmax_t fsize = fs::file_size(outFile, ec);
        std::string sizeStr = ec ? "? KB" : (std::to_string(fsize / 1024) + " KB");
        
        g_diag.add("Success", "Saved: " + outName + (g_app.wavMode ? ".wav" : ".ogg"), DiagStatus::OK);
        g_diag.add("Output Size", sizeStr, DiagStatus::OK);
        g_app.setStatus("Done.");
    } else {
        g_diag.add("Error", "Encoding failed.", DiagStatus::Error);
        g_app.setStatus("Error.");
    }
    g_app.busy = false;
}

static void LaunchWorker(const std::string& file) {
    if (g_app.busy) return;
    if (g_app.worker.joinable()) g_app.worker.join();
    g_app.worker = std::thread(ProcessFile, file);
}

// ──────────────────────────────────────────────────────────────
//  Diagnostics Implementation
// ──────────────────────────────────────────────────────────────
static std::string BuildDiagReport() {
    std::lock_guard lock(g_diag.mtx);
    std::ostringstream r;
    r << "=== Silencer Diagnostics & Log ===\n";
    if (!g_diag.timestamp.empty()) r << "Timestamp : " << g_diag.timestamp << "\n";
    r << "\n";
    for (auto& e : g_diag.entries) {
        const char* tag =
            e.status == DiagStatus::OK      ? "[OK]  " :
            e.status == DiagStatus::Warning ? "[WARN]" :
            e.status == DiagStatus::Error   ? "[ERR] " : 
            e.status == DiagStatus::Info    ? "[INFO]" : "[?]   ";
        char buf[512];
        snprintf(buf, sizeof(buf), "%-6s  %-22s -> %s\n", tag, e.label.c_str(), e.detail.c_str());
        r << buf;
    }
    return r.str();
}

static void RunDiagnostics() {
    g_diag.clear();
    g_diag.add("System", "Initializing Silencer Diagnostics...", DiagStatus::Info);

    {
        time_t now = time(nullptr); char tbuf[32] = {}; struct tm tmi{};
        localtime_s(&tmi, &now);
        strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tmi);
        std::lock_guard lock(g_diag.mtx);
        g_diag.timestamp = tbuf;
    }

    g_diag.add("miniaudio",  std::string("v") + ma_version_string(), DiagStatus::OK);
    const char* vv = vorbis_version_string();
    g_diag.add("libvorbis",  vv ? std::string(vv) : "linked (no version string)", DiagStatus::OK);

    int ebMaj = 0, ebMin = 0, ebPatch = 0;
    ebur128_get_version(&ebMaj, &ebMin, &ebPatch);
    g_diag.add("libebur128", std::to_string(ebMaj) + "." + std::to_string(ebMin) + "." + std::to_string(ebPatch), DiagStatus::OK);

    {
        NativeAudioData testAudio;
        const int testFrames = 22050;
        testAudio.pcm.resize(testFrames * 2);
        for (int i = 0; i < testFrames; ++i) {
            float s = 0.25f * std::sin(2.f * 3.14159265f * 440.f * i / 44100.f);
            testAudio.pcm[i * 2]     = s;
            testAudio.pcm[i * 2 + 1] = s;
        }

        char tempDir[MAX_PATH] = {}, tempBase[MAX_PATH] = {};
        GetTempPathA(MAX_PATH, tempDir);
        GetTempFileNameA(tempDir, "slc", 0, tempBase);
        std::string tempOgg = std::string(tempBase) + ".ogg";
        DeleteFileA(tempBase);

        if (!EncodeToOgg(testAudio, tempOgg)) {
            g_diag.add("OGG encode test", "FAILED — EncodeToOgg returned false", DiagStatus::Error);
        } else {
            std::error_code ec;
            uintmax_t fsize = fs::file_size(tempOgg, ec);
            if (ec || fsize < 64) {
                g_diag.add("OGG encode test", "FAILED — file missing or too small", DiagStatus::Error);
            } else {
                g_diag.add("OGG encode test", "OK — " + std::to_string(fsize) + " bytes written", DiagStatus::OK);

                ma_decoder dec{};
                if (ma_decoder_init_file(tempOgg.c_str(), nullptr, &dec) != MA_SUCCESS) {
                    g_diag.add("OGG decode test", "FAILED — miniaudio could not open OGG", DiagStatus::Error);
                } else {
                    ma_uint64 len = 0;
                    ma_decoder_get_length_in_pcm_frames(&dec, &len);
                    int sr = dec.outputSampleRate;
                    ma_decoder_uninit(&dec);
                    if (len > 0) {
                        char dbuf[128];
                        snprintf(dbuf, sizeof(dbuf), "OK — %llu frames @ %d Hz decoded", (unsigned long long)len, sr);
                        g_diag.add("OGG decode test", dbuf, DiagStatus::OK);
                    } else {
                        g_diag.add("OGG decode test", "FAILED — 0 frames decoded", DiagStatus::Error);
                    }
                }
            }
            DeleteFileA(tempOgg.c_str());
        }
    }

    {
        char tempDir2[MAX_PATH] = {}, ioPath[MAX_PATH] = {};
        GetTempPathA(MAX_PATH, tempDir2);
        GetTempFileNameA(tempDir2, "slc", 0, ioPath);
        FILE* tf = fopen(ioPath, "wb");
        if (tf) {
            fputs("ok", tf); fclose(tf); DeleteFileA(ioPath);
            g_diag.add("File I/O", std::string("OK — write succeeded in temp dir"), DiagStatus::OK);
        } else {
            g_diag.add("File I/O", std::string("FAILED — cannot write to temp dir"), DiagStatus::Error);
        }
    }

    // ── OS version ───────────────────────────────────────────────
    {
        OSVERSIONINFOEXA osi{}; osi.dwOSVersionInfoSize = sizeof(osi);
        HMODULE ntdll = GetModuleHandleA("ntdll.dll");
        if (ntdll) {
            using fn_t = LONG(WINAPI*)(OSVERSIONINFOEXA*); 
            auto fn = (fn_t)GetProcAddress(ntdll, "RtlGetVersion");
            if (fn) fn(&osi);
        }
        
        // Prüfe, ob es sich anhand der Build-Nummer um Windows 11 handelt
        std::string osName = "Windows " + std::to_string(osi.dwMajorVersion) + "." + std::to_string(osi.dwMinorVersion);
        if (osi.dwMajorVersion == 10 && osi.dwMinorVersion == 0) {
            osName = (osi.dwBuildNumber >= 22000) ? "Windows 11" : "Windows 10";
        }

        // Lese die "DisplayVersion" (z.B. "24H2") aus der Registry
        std::string displayVersion = "";
        HKEY hKey;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            char valBuf[64]{};
            DWORD valSize = sizeof(valBuf);
            // Ab Windows 10 20H2 und Windows 11 heißt der Wert "DisplayVersion"
            if (RegQueryValueExA(hKey, "DisplayVersion", nullptr, nullptr, (LPBYTE)valBuf, &valSize) == ERROR_SUCCESS) {
                displayVersion = valBuf;
            } 
            // Fallback für ältere Windows 10 Versionen (z.B. "1909", "2004")
            else if (RegQueryValueExA(hKey, "ReleaseId", nullptr, nullptr, (LPBYTE)valBuf, &valSize) == ERROR_SUCCESS) {
                displayVersion = valBuf;
            }
            RegCloseKey(hKey);
        }

        char osBuf[128];
        if (!displayVersion.empty()) {
            snprintf(osBuf, sizeof(osBuf), "%s %s (build %lu)", osName.c_str(), displayVersion.c_str(), osi.dwBuildNumber);
        } else {
            snprintf(osBuf, sizeof(osBuf), "%s (build %lu)", osName.c_str(), osi.dwBuildNumber);
        }
        
        g_diag.add("OS", osBuf, DiagStatus::OK);
    }

    {
        MEMORYSTATUSEX ms{}; ms.dwLength = sizeof(ms);
        GlobalMemoryStatusEx(&ms);
        char memBuf[64];
        snprintf(memBuf, sizeof(memBuf), "%.1f GB free / %.1f GB total", ms.ullAvailPhys / 1073741824.0, ms.ullTotalPhys / 1073741824.0);
        g_diag.add("System Memory", memBuf, ms.ullAvailPhys < (256ULL * 1024 * 1024) ? DiagStatus::Warning : DiagStatus::OK);
    }
}

// ──────────────────────────────────────────────────────────────
//  Custom drawing helpers (Modern, Wide UI)
// ──────────────────────────────────────────────────────────────
static ImFont* g_fontRegular  = nullptr;
static ImFont* g_fontSemiBold = nullptr;

static void BeginPanel(const char* id, ImVec2 size = {0, 0}) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, UI_PANEL);
    ImGui::PushStyleColor(ImGuiCol_Border, UI_BORDER);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {22.f, 22.f});
    ImGui::BeginChild(id, size, true, ImGuiWindowFlags_NoScrollbar);
}

static void EndPanel() { ImGui::EndChild(); ImGui::PopStyleVar(2); ImGui::PopStyleColor(2); }

static void PanelTitle(const char* title) {
    if (g_fontSemiBold) ImGui::PushFont(g_fontSemiBold);
    ImGui::PushStyleColor(ImGuiCol_Text, UI_SUBTEXT); ImGui::TextUnformatted(title);
    ImGui::PopStyleColor(); if (g_fontSemiBold) ImGui::PopFont();
    ImGui::Spacing(); ImGui::Dummy({0, 4.f});
}

static bool DrawToggle(const char* id, bool* v, float w = 48.f, float h = 26.f) {
    ImDrawList* dl = ImGui::GetWindowDrawList(); ImVec2 pos = ImGui::GetCursorScreenPos();
    float r = h * 0.5f; ImGui::InvisibleButton(id, {w, h});
    bool clicked = ImGui::IsItemClicked(); if (clicked) *v = !*v;
    ImU32 trackCol = *v ? UI_MINT : UI_BORDER; dl->AddRectFilled(pos, {pos.x + w, pos.y + h}, trackCol, r);
    float cx = *v ? pos.x + w - r : pos.x + r; dl->AddCircleFilled({cx, pos.y + r}, r - 4.f, IM_COL32(255, 255, 255, 255));
    return clicked;
}

static void DrawDashedRect(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float radius = 12.f, float dash = 8.f, float gap = 6.f) {
    auto seg = [&](ImVec2 p1, ImVec2 p2) {
        float dx = p2.x - p1.x; float dy = p2.y - p1.y; float len = std::sqrt(dx * dx + dy * dy);
        dx /= len; dy /= len; float t = 0;
        while (t < len) {
            float t2 = std::min(t + dash, len); dl->AddLine({p1.x + dx * t, p1.y + dy * t}, {p1.x + dx * t2, p1.y + dy * t2}, col, 2.0f);
            t += dash + gap;
        }
    };
    float r = radius; seg({a.x + r, a.y}, {b.x - r, a.y}); seg({a.x + r, b.y}, {b.x - r, b.y});
    seg({a.x, a.y + r}, {a.x, b.y - r}); seg({b.x, a.y + r}, {b.x, b.y - r});
    auto arc = [&](ImVec2 c, float sa, float ea) {
        int steps = 6; float step = (ea - sa) / steps;
        for (int i = 0; i < steps; i += 2) {
            float a1 = sa + i * step; float a2 = sa + (i + 1) * step;
            dl->AddLine({c.x + r * std::cos(a1), c.y + r * std::sin(a1)}, {c.x + r * std::cos(a2), c.y + r * std::sin(a2)}, col, 2.0f);
        }
    };
    constexpr float PI = 3.14159265f;
    arc({a.x + r, a.y + r}, PI, PI * 1.5f); arc({b.x - r, a.y + r}, PI * 1.5f, PI * 2.f);
    arc({b.x - r, b.y - r}, 0, PI * 0.5f); arc({a.x + r, b.y - r}, PI * 0.5f, PI);
}

// ──────────────────────────────────────────────────────────────
//  Main UI Rendering
// ──────────────────────────────────────────────────────────────
static void RenderUI() {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, UI_BG); ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {32.f, 32.f});
    
    ImGui::Begin("##root", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar);
    
    bool busy = g_app.busy.load();
    float winH = ImGui::GetContentRegionAvail().y; 
    float winW = ImGui::GetContentRegionAvail().x; 
    
    if (g_fontSemiBold) ImGui::PushFont(g_fontSemiBold);
    ImGui::PushStyleColor(ImGuiCol_Text, UI_TEXT); ImGui::SetWindowFontScale(1.8f);
    ImGui::Text("SILENCER"); ImGui::SetWindowFontScale(1.f); ImGui::PopStyleColor();
    if (g_fontSemiBold) ImGui::PopFont();
    
    ImGui::SameLine(); ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 10.f);
    ImGui::PushStyleColor(ImGuiCol_Text, UI_SUBTEXT); ImGui::Text("v1.7"); ImGui::PopStyleColor();

    ImGui::Dummy({0, 16.f});
    
    // Spalten-Konfiguration
    float leftWidth = 420.f; 
    float gap = 24.f; 
    float rightWidth = winW - leftWidth - gap;

    // Feste Höhen berechnen für bündige Ausrichtung am unteren Rand
    float settingsH = 380.f; 
    float resultsH  = winH - settingsH - gap - 64.f;
    float dropzoneH = 140.f;
    float logH      = winH - dropzoneH - gap - 64.f;

    // ================= LINKS =================
    ImGui::BeginGroup(); 
    BeginPanel("##settings", {leftWidth, settingsH}); 
    PanelTitle("Configuration");
    struct TD { const char* id; bool* v; const char* label; const char* desc; };
    TD toggles[] = { 
        {"##lufs", &g_app.applyLufs, "LUFS Normalization", "-8 LUFS Target"}, 
        {"##madi", &g_app.calcMadi, "Calculate Madi", "StartMadi / TotalMadi"}, 
        {"##wav", &g_app.wavMode, "WAV Output", "pcm_s16le mode"}, 
    };
    
    if (busy) ImGui::BeginDisabled();
    for (auto& t : toggles) {
        float rowY = ImGui::GetCursorPosY(); ImGui::SetCursorPosY(rowY + 3.f);
        if (g_fontSemiBold) ImGui::PushFont(g_fontSemiBold);
        ImGui::PushStyleColor(ImGuiCol_Text, UI_TEXT); ImGui::Text("%s", t.label); ImGui::PopStyleColor();
        if (g_fontSemiBold) ImGui::PopFont();
        ImGui::SameLine(leftWidth - 48.f - 40.f); ImGui::SetCursorPosY(rowY);
        DrawToggle(t.id, t.v); ImGui::Spacing(); ImGui::Dummy({0, 4.f});
    }
    if (busy) ImGui::EndDisabled();

    // ── Bitraten UI ───────────────────────────────────────
    ImGui::Dummy({0, 8.f});
    PanelTitle("Ogg Vorbis Bitrate");
    
    if (busy || g_app.wavMode) ImGui::BeginDisabled();
    auto DrawBitrateBtn = [](const char* label, int bitrate, float w) {
        bool active = (g_app.activeBitrate == bitrate);
        ImGui::PushStyleColor(ImGuiCol_Button, active ? IM_COL32(35, 45, 40, 255) : UI_PANEL);
        ImGui::PushStyleColor(ImGuiCol_Border, active ? UI_MINT : UI_BORDER);
        ImGui::PushStyleColor(ImGuiCol_Text, active ? UI_MINT : UI_TEXT);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.f);
        
        if (active && g_fontSemiBold) ImGui::PushFont(g_fontSemiBold);
        if (ImGui::Button(label, {w, 28.f})) g_app.activeBitrate = bitrate;
        if (active && g_fontSemiBold) ImGui::PopFont();
        
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(3);
    };

    float btnW = (leftWidth - 44.f - 16.f) / 3.f; 
    ImGui::TextColored(V_SUBTEXT, "Low Quality");
    DrawBitrateBtn("64 kbps", 64, btnW); ImGui::SameLine();
    DrawBitrateBtn("96 kbps", 96, btnW); ImGui::SameLine();
    DrawBitrateBtn("128 kbps", 128, btnW); ImGui::Dummy({0, 2.f});
    ImGui::TextColored(V_SUBTEXT, "Standard Quality");
    DrawBitrateBtn("160 kbps", 160, btnW); ImGui::SameLine();
    DrawBitrateBtn("192 kbps", 192, btnW); ImGui::Dummy({0, 2.f});
    ImGui::TextColored(V_SUBTEXT, "High Quality");
    DrawBitrateBtn("256 kbps", 256, btnW); ImGui::SameLine();
    DrawBitrateBtn("320 kbps", 320, btnW); ImGui::Dummy({0, 6.f});
    ImGui::TextColored(V_MINT, "* Recommended for Audition: 96 kbps");

    if (busy || g_app.wavMode) ImGui::EndDisabled();
    EndPanel();

    ImGui::Dummy({0, gap});
    BeginPanel("##results", {leftWidth, resultsH}); 
    PanelTitle("Output Details");
    bool hasRes; double dBpm; long long dSm, dTm;
    { std::lock_guard l(g_app.resMtx); hasRes = g_app.hasResults; dBpm = g_app.displayBpm; dSm = g_app.displaySM; dTm = g_app.displayTM; }
    auto drawMetric = [](const char* lbl, const std::string& val) {
        ImGui::PushStyleColor(ImGuiCol_Text, UI_SUBTEXT); ImGui::TextUnformatted(lbl); ImGui::PopStyleColor();
        ImGui::SameLine(180.f);
        if (g_fontSemiBold) ImGui::PushFont(g_fontSemiBold);
        ImGui::PushStyleColor(ImGuiCol_Text, UI_MINT); ImGui::TextUnformatted(val.c_str()); ImGui::PopStyleColor();
        if (g_fontSemiBold) ImGui::PopFont(); ImGui::Spacing(); ImGui::Dummy({0, 6.f});
    };

    if (hasRes) { drawMetric("Tempo (BPM)", FmtBpm(dBpm)); drawMetric("Start Madi", std::to_string(dSm)); drawMetric("Total Madi", std::to_string(dTm)); } 
    else { ImGui::PushStyleColor(ImGuiCol_Text, UI_BORDER); ImGui::Text("Awaiting file processing..."); ImGui::PopStyleColor(); }
    EndPanel(); 
    ImGui::EndGroup(); // ENDE LINKS
    
    // ================= RECHTS =================
    ImGui::SameLine(0, gap); 
    ImGui::BeginGroup();

    BeginPanel("##diag_panel", {rightWidth, logH});
    if (g_fontSemiBold) ImGui::PushFont(g_fontSemiBold);
    ImGui::PushStyleColor(ImGuiCol_Text, UI_SUBTEXT);
    ImGui::TextUnformatted("System & Conversion Log");
    ImGui::PopStyleColor();
    if (g_fontSemiBold) ImGui::PopFont();

    ImGui::SameLine();
    {
        // KORREKTUR: Die tatsächliche Breite beider Buttons inkl. Padding (115 + 6 + 58)
        float btnAreaW = 179.f; 
        float moveX = ImGui::GetContentRegionAvail().x - btnAreaW;
        if (moveX > 0.f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + moveX);
    }
    ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(38, 38, 44, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  UI_BORDER);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,   UI_MINT_DK);
    ImGui::PushStyleColor(ImGuiCol_Text,           UI_SUBTEXT);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,  {8.f, 3.f});
    if (ImGui::Button("Run Diagnostics", {115.f, 0.f})) { RunDiagnostics(); }
    ImGui::SameLine(0.f, 6.f);
    {
        static float copiedTimer = 0.f;
        copiedTimer -= ImGui::GetIO().DeltaTime;
        bool showCopied = copiedTimer > 0.f;
        if (showCopied) ImGui::PushStyleColor(ImGuiCol_Text, UI_MINT);
        if (ImGui::Button(showCopied ? " Copied!" : " Copy ", {58.f, 0.f})) {
            ImGui::SetClipboardText(BuildDiagReport().c_str());
            copiedTimer = 1.8f;
        }
        if (showCopied) ImGui::PopStyleColor();
    }
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);

    ImGui::Spacing(); ImGui::Dummy({0, 2.f});
    std::string currentTs;
    { std::lock_guard l(g_diag.mtx); currentTs = g_diag.timestamp; }
    if (!currentTs.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, UI_SUBTEXT);
        ImGui::Text("Session started: %s", currentTs.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Dummy({0, 4.f});

    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
    if (ImGui::BeginChild("##diag_entries", {-1.f, -1.f}, false, ImGuiWindowFlags_HorizontalScrollbar)) {
        std::lock_guard lock(g_diag.mtx);
        for (auto& e : g_diag.entries) {
            ImU32 col; const char* statusTxt;
            switch (e.status) {
                case DiagStatus::OK:      col = UI_MINT;    statusTxt = "[OK]  "; break;
                case DiagStatus::Warning: col = UI_WARN;    statusTxt = "[WARN]"; break;
                case DiagStatus::Error:   col = UI_RED;     statusTxt = "[ERR] "; break;
                case DiagStatus::Info:    col = UI_TEXT;    statusTxt = "[INFO]"; break;
                default:                  col = UI_SUBTEXT; statusTxt = "[?]   "; break;
            }
            if (g_fontSemiBold) ImGui::PushFont(g_fontSemiBold);
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            ImGui::TextUnformatted(statusTxt);
            ImGui::PopStyleColor();
            if (g_fontSemiBold) ImGui::PopFont();
            
            ImGui::SameLine(0.f, 6.f);
            if (g_fontSemiBold) ImGui::PushFont(g_fontSemiBold);
            ImGui::PushStyleColor(ImGuiCol_Text, UI_TEXT);
            ImGui::Text("%-18s", e.label.c_str());
            ImGui::PopStyleColor();
            if (g_fontSemiBold) ImGui::PopFont();
            
            ImGui::SameLine(0.f, 4.f);
            ImGui::PushStyleColor(ImGuiCol_Text, (e.status == DiagStatus::Info) ? UI_SUBTEXT : UI_TEXT);
            ImGui::Text("-> %s", e.detail.c_str());
            ImGui::PopStyleColor();
        }
        if (g_diag.autoScroll) {
            ImGui::SetScrollHereY(1.0f);
            g_diag.autoScroll = false;
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    EndPanel();

    ImGui::Dummy({0, gap});
    ImVec2 dzPos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##dz", {rightWidth, dropzoneH}); bool dzHover = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked() && !busy) { std::string f = OpenFileDialog(); if (!f.empty()) LaunchWorker(f); }
    
    ImU32 dzBg = dzHover ? UI_BORDER : UI_PANEL; ImU32 dzBdr = dzHover ? UI_MINT : UI_BORDER; ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(dzPos, {dzPos.x + rightWidth, dzPos.y + dropzoneH}, dzBg, 12.f); 
    DrawDashedRect(dl, dzPos, {dzPos.x + rightWidth, dzPos.y + dropzoneH}, dzBdr, 12.f, 10.f, 8.f);
    
    float cx = dzPos.x + rightWidth * 0.5f; float cy = dzPos.y + dropzoneH * 0.5f;
    const char* mainTxt = busy ? "Processing file..." : "Drop audio file here"; const char* subTxt  = ".wav   .flac   .ogg   .mp3";
    
    if (g_fontSemiBold) ImGui::PushFont(g_fontSemiBold);
    ImVec2 ms = ImGui::CalcTextSize(mainTxt); dl->AddText({cx - ms.x * 0.5f, cy - 14.f}, dzHover ? UI_TEXT : UI_MINT, mainTxt);
    if (g_fontSemiBold) ImGui::PopFont(); ImVec2 ss = ImGui::CalcTextSize(subTxt); dl->AddText({cx - ss.x * 0.5f, cy + 14.f}, UI_SUBTEXT, subTxt);
    
    ImGui::EndGroup(); // ENDE RECHTS
    
    // ================= FOOTER / STATUS =================
    std::string status = g_app.getStatus();
    ImVec4 sc = busy ? V_WARN : status == "Done." ? V_MINT : status == "Error." ? V_RED : V_SUBTEXT;
    ImVec2 dotPos = {ImGui::GetWindowPos().x + 32.f, ImGui::GetWindowPos().y + io.DisplaySize.y - 20.f};
    
    ImU32 dotCol;
    if (busy) { float t = (float)ImGui::GetTime(); float a = 0.4f + 0.6f * (std::sin(t * 5.f) * .5f + .5f); dotCol = IM_COL32(235, 180, 90, (int)(a * 255)); } 
    else if (status == "Done.") dotCol = UI_MINT; 
    else if (status == "Error.") dotCol = UI_RED; 
    else dotCol = UI_SUBTEXT;
    
    dl->AddCircleFilled(dotPos, 5.f, dotCol); ImGui::SetCursorScreenPos({dotPos.x + 16.f, dotPos.y - 8.f});
    if (busy) status += std::string((int)(ImGui::GetTime() * 2) % 4, '.');
    if (g_fontSemiBold) ImGui::PushFont(g_fontSemiBold);
    ImGui::TextColored(sc, "%s", status.c_str());
    if (g_fontSemiBold) ImGui::PopFont();
    
    // ================= DIALOGS =================
    if (g_app.dialogOpen.load(std::memory_order_acquire)) {
        ImDrawList* bg = ImGui::GetBackgroundDrawList(); bg->AddRectFilled({0,0}, io.DisplaySize, IM_COL32(0,0,0,180));
        ImGui::SetNextWindowSize({460.f, 210.f}, ImGuiCond_Always); ImGui::SetNextWindowPos({io.DisplaySize.x * .5f, io.DisplaySize.y * .5f}, ImGuiCond_Always, {.5f, .5f}); ImGui::SetNextWindowFocus();
        ImGui::PushStyleColor(ImGuiCol_WindowBg, UI_PANEL); ImGui::PushStyleColor(ImGuiCol_Border, UI_BORDER); ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 16.f); ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {28.f, 24.f});
        ImGui::Begin("##inputdlg", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar);

        ImDrawList* ddl = ImGui::GetWindowDrawList(); ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
        ddl->AddRectFilled({wp.x + 12.f, wp.y}, {wp.x + ws.x - 12.f, wp.y + 2.f}, UI_MINT, 1.f); ImGui::Dummy({0, 6.f});

        if (g_fontSemiBold) ImGui::PushFont(g_fontSemiBold);
        ImGui::PushStyleColor(ImGuiCol_Text, UI_MINT); ImGui::SetWindowFontScale(1.1f); ImGui::TextUnformatted(g_app.dialogQuestion.c_str()); ImGui::SetWindowFontScale(1.f); ImGui::PopStyleColor();
        if (g_fontSemiBold) ImGui::PopFont(); ImGui::Spacing(); ImGui::Dummy({0, 10.f});

        static bool needsFocus = false, lastOpen = false; bool curOpen2 = true;
        if (!lastOpen && curOpen2) needsFocus = true; lastOpen = curOpen2;

        ImGui::PushStyleColor(ImGuiCol_FrameBg, UI_BG); ImGui::PushStyleColor(ImGuiCol_Text, UI_TEXT); ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {12.f, 10.f}); ImGui::SetNextItemWidth(-1.f);
        if (needsFocus) { ImGui::SetKeyboardFocusHere(); needsFocus = false; }
        bool enter = ImGui::InputText("##dlginput", g_app.dialogBuf, sizeof(g_app.dialogBuf), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::PopStyleVar(); ImGui::PopStyleColor(2); ImGui::Spacing(); ImGui::Dummy({0, 12.f});

        auto commit = [&]() {
            double v = 0.0; try { v = std::stod(g_app.dialogBuf); } catch (...) {}
            switch (g_app.dialogStep) { case DialogStep::WaitingOffset: g_app.resultOffset = v; break; case DialogStep::WaitingBpm: g_app.resultBpm = v; break; case DialogStep::WaitingTime: g_app.resultTime = v; break; default: break; }
            lastOpen = false; g_app.dialogConfirmed.store(true); g_app.dialogOpen.store(false, std::memory_order_release);
        };

        float dialogBtnW = (ImGui::GetContentRegionAvail().x - 12.f) * .5f;
        ImGui::PushStyleColor(ImGuiCol_Button, UI_MINT_DK); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, UI_MINT); ImGui::PushStyleColor(ImGuiCol_ButtonActive, UI_MINT); ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(20, 20, 20, 255));
        if (g_fontSemiBold) ImGui::PushFont(g_fontSemiBold);
        if (ImGui::Button("Confirm", {dialogBtnW, 36.f}) || enter) commit();
        if (g_fontSemiBold) ImGui::PopFont(); ImGui::PopStyleColor(4); ImGui::SameLine(0.f, 12.f);
        ImGui::PushStyleColor(ImGuiCol_Button, UI_BORDER); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(60, 60, 65, 255)); ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(70, 70, 75, 255)); ImGui::PushStyleColor(ImGuiCol_Text, UI_TEXT);
        if (ImGui::Button("Cancel", {dialogBtnW, 36.f})) { lastOpen = false; g_app.dialogCancelled.store(true); g_app.dialogOpen.store(false, std::memory_order_release); }
        ImGui::PopStyleColor(4); ImGui::End(); ImGui::PopStyleVar(2); ImGui::PopStyleColor(2);
    }
    ImGui::End(); ImGui::PopStyleVar(); ImGui::PopStyleColor();
}

// ──────────────────────────────────────────────────────────────
//  DirectX 11 & Windows Boilerplate
// ──────────────────────────────────────────────────────────────
static ID3D11Device*           g_pDevice = nullptr;
static ID3D11DeviceContext*    g_pContext = nullptr;
static IDXGISwapChain*         g_pSwap = nullptr;
static ID3D11RenderTargetView* g_pRTV = nullptr;

static void CreateRTV() {
    ID3D11Texture2D* bb = nullptr;
    g_pSwap->GetBuffer(0, IID_PPV_ARGS(&bb));
    g_pDevice->CreateRenderTargetView(bb, nullptr, &g_pRTV);
    bb->Release();
}

static bool InitD3D(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{}; sd.BufferCount = 2; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferDesc.RefreshRate = {60, 1};
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow = hwnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL fl; const D3D_FEATURE_LEVEL fla[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, fla, 2, D3D11_SDK_VERSION, &sd, &g_pSwap, &g_pDevice, &fl, &g_pContext))) return false;
    CreateRTV(); return true;
}

static void CleanupD3D() {
    if (g_pRTV) { g_pRTV->Release(); g_pRTV = nullptr; }
    if (g_pSwap) { g_pSwap->Release(); g_pSwap = nullptr; }
    if (g_pContext) { g_pContext->Release(); g_pContext = nullptr; }
    if (g_pDevice) { g_pDevice->Release(); g_pDevice = nullptr; }
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return 1;
    switch (msg) {
        case WM_SIZE:
            if (g_pDevice && wParam != SIZE_MINIMIZED) {
                if (g_pRTV) { g_pRTV->Release(); g_pRTV = nullptr; }
                g_pSwap->ResizeBuffers(0, LOWORD(lParam), HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0); CreateRTV();
            } return 0;
        case WM_DROPFILES: {
            HDROP hd = (HDROP)wParam; char buf[MAX_PATH]{};
            if (DragQueryFileA(hd, 0, buf, MAX_PATH) && !g_app.busy.load()) LaunchWorker(buf);
            DragFinish(hd); return 0;
        }
        case WM_GETMINMAXINFO: {
            // KORREKTUR: Fenstergröße auf minimal 1140x760 angehoben
            MINMAXINFO* mmi = (MINMAXINFO*)lParam; mmi->ptMinTrackSize = {1140, 760}; mmi->ptMaxTrackSize = {1140, 760}; return 0;
        }
        case WM_SYSCOMMAND: if ((wParam & 0xfff0) == SC_KEYMENU) return 0; break;
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, WndProc, 0, 0, hInst, LoadIconW(hInst, MAKEINTRESOURCEW(101)), nullptr, nullptr, nullptr, L"SilencerImGui", LoadIconW(hInst, MAKEINTRESOURCEW(101))};
    RegisterClassExW(&wc);
    DWORD style = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    
    // KORREKTUR: Initiale Fenstergröße beim Start auf 1140x760 angehoben
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"Silencer v1.7 - by Sanya", style, CW_USEDEFAULT, CW_USEDEFAULT, 1140, 760, nullptr, nullptr, hInst, nullptr);
    DragAcceptFiles(hwnd, TRUE);
    if (!InitD3D(hwnd)) { CleanupD3D(); return 1; }
    
    ShowWindow(hwnd, SW_SHOWDEFAULT); UpdateWindow(hwnd);
    IMGUI_CHECKVERSION(); ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard; io.IniFilename = nullptr;

#ifdef FONTS_EMBEDDED
    ImFontConfig font_cfg; font_cfg.FontDataOwnedByAtlas = false;
    g_fontRegular  = io.Fonts->AddFontFromMemoryTTF((void*)poppins_regular_compressed_data, poppins_regular_compressed_size, 18.f, &font_cfg);
    g_fontSemiBold = io.Fonts->AddFontFromMemoryTTF((void*)poppins_semibold_compressed_data, poppins_semibold_compressed_size, 18.f, &font_cfg);
#else
    char exeBuf[MAX_PATH]{}; GetModuleFileNameA(nullptr, exeBuf, MAX_PATH); fs::path exeDir = fs::path(exeBuf).parent_path();
    std::string pathReg = (exeDir / "Poppins-Regular.ttf").string(); std::string pathSemi = (exeDir / "Poppins-SemiBold.ttf").string();
    if (GetFileAttributesA(pathReg.c_str()) != INVALID_FILE_ATTRIBUTES) g_fontRegular = io.Fonts->AddFontFromFileTTF(pathReg.c_str(), 18.f);
    if (GetFileAttributesA(pathSemi.c_str()) != INVALID_FILE_ATTRIBUTES) g_fontSemiBold = io.Fonts->AddFontFromFileTTF(pathSemi.c_str(), 18.f);
#endif

    if (!g_fontRegular) io.Fonts->AddFontDefault();
    io.Fonts->Build(); ImGui_ImplWin32_Init(hwnd); ImGui_ImplDX11_Init(g_pDevice, g_pContext);
    RunDiagnostics();

    constexpr float clear[4] = {15/255.f, 15/255.f, 18/255.f, 1.f}; MSG msg{};
    while (true) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessage(&msg); if (msg.message == WM_QUIT) goto done;
        }
        ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();
        RenderUI();
        ImGui::Render(); g_pContext->OMSetRenderTargets(1, &g_pRTV, nullptr); g_pContext->ClearRenderTargetView(g_pRTV, clear); ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData()); g_pSwap->Present(1, 0);
    }
done:
    if (g_app.worker.joinable()) { g_app.dialogCancelled.store(true); g_app.dialogOpen.store(false, std::memory_order_release); g_app.worker.join(); }
    ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
    CleanupD3D(); DestroyWindow(hwnd); UnregisterClassW(wc.lpszClassName, hInst);
    return 0;
}