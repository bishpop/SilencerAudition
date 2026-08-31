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

// ──────────────────────────────────────────────────────────────
// Embedded Fonts
// ──────────────────────────────────────────────────────────────
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
#include <regex>
#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

// ──────────────────────────────────────────────────────────────
//  Modern Dark Palette (Reference matched)
// ──────────────────────────────────────────────────────────────
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

// ──────────────────────────────────────────────────────────────
//  Thread-safe log
// ──────────────────────────────────────────────────────────────
struct LogEntry {
    std::string text;
    ImVec4 color;
};

struct AppLog {
    std::vector<LogEntry> entries;
    std::mutex            mtx;
    bool                  autoScroll = true;

    void add(const std::string& text, ImVec4 color = V_TEXT) {
        std::lock_guard lock(mtx);
        std::istringstream ss(text);
        std::string line;
        while (std::getline(ss, line)) {
            entries.push_back({line, color});
        }
        autoScroll = true;
    }

    void clear() {
        std::lock_guard lock(mtx);
        entries.clear();
    }

    void draw() {
        std::lock_guard lock(mtx);
        for (auto& e : entries) {
            ImGui::PushStyleColor(ImGuiCol_Text, e.color);
            ImGui::TextUnformatted(e.text.c_str());
            ImGui::PopStyleColor();
        }
        if (autoScroll) {
            ImGui::SetScrollHereY(1.f);
            autoScroll = false;
        }
    }
};

// ──────────────────────────────────────────────────────────────
//  Application state
// ──────────────────────────────────────────────────────────────
enum class DialogStep { None, WaitingOffset, WaitingBpm, WaitingTime };

struct AppState {
    bool applyLufs = true;
    bool calcMadi  = true;
    bool wavMode   = false;

    std::mutex  statusMtx;
    std::string statusMsg = "Ready";

    void setStatus(const std::string& s) {
        std::lock_guard l(statusMtx);
        statusMsg = s;
    }

    std::string getStatus() {
        std::lock_guard l(statusMtx);
        return statusMsg;
    }

    std::mutex    resMtx;
    double        displayBpm = 0;
    long long     displaySM = 0;
    long long     displayTM = 0;
    bool          hasResults = false;

    void setResults(double b, long long s, long long t) {
        std::lock_guard l(resMtx);
        displayBpm = b;
        displaySM = s;
        displayTM = t;
        hasResults = true;
    }

    void clearResults() {
        std::lock_guard l(resMtx);
        hasResults = false;
    }

    AppLog log;
    std::atomic<bool> busy{false};

    std::atomic<bool> dialogOpen{false};
    std::atomic<bool> dialogConfirmed{false};
    std::atomic<bool> dialogCancelled{false};
    DialogStep dialogStep{DialogStep::None};
    char       dialogBuf[64]{};
    std::string dialogQuestion;

    double resultOffset=0, resultBpm=120, resultTime=0;
    std::thread worker;
};
static AppState g_app;

// ──────────────────────────────────────────────────────────────
//  Diagnostics system
// ──────────────────────────────────────────────────────────────
enum class DiagStatus { Unknown, OK, Warning, Error };

struct DiagEntry {
    std::string  label;
    std::string  detail;
    DiagStatus   status = DiagStatus::Unknown;
};

struct Diagnostics {
    std::vector<DiagEntry> entries;
    bool visible = false;
    bool hasError = false;

    void clear() {
        entries.clear();
        hasError = false;
    }

    void add(const std::string& l, const std::string& d, DiagStatus s) {
        entries.push_back({l, d, s});
        if (s == DiagStatus::Error) {
            hasError = true;
        }
    }
};
static Diagnostics g_diag;

// ──────────────────────────────────────────────────────────────
//  Process helpers
// ──────────────────────────────────────────────────────────────
struct ProcResult {
    bool success{};
    std::string output;
};

static ProcResult RunProcess(const std::string& exe, const std::string& args) {
    ProcResult res;
    std::string cmd = "\"" + exe + "\" " + args;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    
    HANDLE rOut{}, wOut{};
    CreatePipe(&rOut, &wOut, &sa, 0);
    SetHandleInformation(rOut, HANDLE_FLAG_INHERIT, 0);
    
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wOut;
    si.hStdError = wOut;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    
    PROCESS_INFORMATION pi{};
    std::vector<char> buf(cmd.begin(), cmd.end());
    buf.push_back('\0');
    
    if (!CreateProcessA(nullptr, buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(rOut);
        CloseHandle(wOut);
        res.output = "CreateProcess failed (" + exe + ")";
        return res;
    }
    
    CloseHandle(wOut);
    
    char tmp[4096];
    DWORD n;
    while (ReadFile(rOut, tmp, sizeof(tmp) - 1, &n, nullptr) && n) {
        tmp[n] = '\0';
        res.output += tmp;
    }
    
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code;
    GetExitCodeProcess(pi.hProcess, &code);
    res.success = (code == 0);
    
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(rOut);
    return res;
}

// ── FFmpeg Search Implementation ──────────────────────────────
static std::string FindTool(const std::string& name) {
    auto exists = [](const std::string& p) {
        return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES;
    };
    
    char exeBuf[MAX_PATH]{};
    GetModuleFileNameA(nullptr, exeBuf, MAX_PATH);
    fs::path exeDir = fs::path(exeBuf).parent_path();
    
    // 1. Check directory next to executable
    std::string c = (exeDir / name).string();
    if (exists(c)) return c;
    
    // 2. Check system PATH environment variable
    char pathEnv[32768]{};
    GetEnvironmentVariableA("PATH", pathEnv, sizeof(pathEnv));
    std::istringstream ss(pathEnv);
    std::string dir;
    
    while (std::getline(ss, dir, ';')) {
        if (dir.empty()) continue;
        c = (fs::path(dir) / name).string();
        if (exists(c)) return c;
    }
    
    // 3. Check common fallback directories
    static const char* common[] = {
        "C:\\ffmpeg\\bin", "C:\\ffmpeg",
        "C:\\Program Files\\ffmpeg\\bin", "C:\\Program Files\\ffmpeg",
        "C:\\tools\\ffmpeg\\bin", "C:\\tools\\ffmpeg", nullptr
    };
    for (int i = 0; common[i]; ++i) {
        c = (fs::path(common[i]) / name).string();
        if (exists(c)) return c;
    }
    
    return name;
}

static const std::string& FfmpegPath() {
    static std::string p = FindTool("ffmpeg.exe");
    return p;
}

static const std::string& FfprobePath() {
    static std::string p = FindTool("ffprobe.exe");
    return p;
}

static const std::string& OggEncPath() {
    static std::string p = FindTool("oggenc2.exe");
    return p;
}

static ProcResult RunFfmpeg(const std::string& a) {
    return RunProcess(FfmpegPath(), a);
}

static ProcResult RunFfprobe(const std::string& a) {
    return RunProcess(FfprobePath(), a);
}

// Diagnostics check runner
static void RunDiagnostics() {
    g_diag.clear();
    auto fileExists = [](const std::string& p) {
        return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES;
    };
    
    // Check ffmpeg
    std::string ffmpeg = FfmpegPath();
    if (!fileExists(ffmpeg)) {
        g_diag.add("ffmpeg.exe", "Not found (Checked app folder, PATH & C:\\ffmpeg)", DiagStatus::Error);
    } else {
        g_diag.add("ffmpeg.exe", "Found at: " + ffmpeg, DiagStatus::OK);
    }
    
    // Check ffprobe
    std::string ffprobe = FfprobePath();
    if (!fileExists(ffprobe)) {
        g_diag.add("ffprobe.exe", "Not found (Checked app folder, PATH & C:\\ffmpeg)", DiagStatus::Error);
    } else {
        g_diag.add("ffprobe.exe", "Found at: " + ffprobe, DiagStatus::OK);
    }

    // Check oggenc2
    std::string oggenc = OggEncPath();
    if (!fileExists(oggenc)) {
        g_diag.add("oggenc2.exe", "Not found (Required for OGG / TBM export)", DiagStatus::Warning);
    } else {
        g_diag.add("oggenc2.exe", "Found at: " + oggenc, DiagStatus::OK);
    }
}

// ──────────────────────────────────────────────────────────────
//  Audio helpers
// ──────────────────────────────────────────────────────────────
static double GetDuration(const std::string& f) {
    auto r = RunFfprobe("-v error -show_entries format=duration -of default=noprint_wrappers=1:nokey=1 \"" + f + "\"");
    try {
        return std::stod(r.output);
    } catch (...) {
        return -1.0;
    }
}

static double GetBpm(const std::string& f) {
    auto r = RunFfmpeg("-i \"" + f + "\" -af bpm -f null -");
    std::vector<double> v;
    std::regex re(R"(BPM:\s*([\d\.]+))");
    
    for (auto it = std::sregex_iterator(r.output.begin(), r.output.end(), re); it != std::sregex_iterator{}; ++it) {
        try {
            double x = std::stod((*it)[1].str());
            if (x > 40 && x < 300) {
                v.push_back(x);
            }
        } catch (...) {}
    }
    
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    size_t n = v.size();
    return (n % 2 == 0) ? (v[n / 2 - 1] + v[n / 2]) / 2.0 : v[n / 2];
}

static std::string FmtBpm(double bpm) {
    if (std::abs(std::fmod(bpm, 1.0)) < 1e-9) {
        return std::to_string((long long)bpm);
    }
    std::ostringstream o;
    o << std::fixed << std::setprecision(3) << bpm;
    std::string s = o.str();
    s.erase(s.find_last_not_of('0') + 1);
    if (s.back() == '.') s.pop_back();
    return s;
}

static std::string FmtSize(const std::string& p) {
    try {
        double mb = (double)fs::file_size(p) / (1024.0 * 1024.0);
        std::ostringstream o;
        o << std::fixed << std::setprecision(2) << mb;
        return "  (" + o.str() + " MB)";
    } catch (...) {
        return "";
    }
}

static bool ParseEbur128(const std::string& out, double& lufs) {
    std::smatch m;
    std::regex re(R"(Integrated loudness:\s+I:\s+(-?[\d\.]+))");
    if (!std::regex_search(out, m, re)) return false;
    
    try {
        lufs = std::stod(m[1].str());
        return true;
    } catch (...) {
        return false;
    }
}

static void ApplyMadiParity(long long start, long long& total, double bpm, double& trimSec) {
    if ((start % 2 == 0) == (total % 2 == 0)) {
        --total;
    }
    trimSec = (double)total * 4.0 * 60.0 / bpm;
}

static void LogFileDetails(const std::string& file) {
    try {
        fs::path p(file);
        double sizeMb = (double)fs::file_size(p) / (1024.0 * 1024.0);
        
        auto r = RunFfprobe("-v error -select_streams a:0 -show_entries stream=channels,sample_rate,bit_rate:format=duration -of default=noprint_wrappers=1:nokey=1 \"" + file + "\"");
        
        std::string dur{"N/A"}, sr{"N/A"}, br{"N/A"}, ch{"N/A"};
        std::istringstream ss(r.output);
        std::string line;
        
        while (std::getline(ss, line)) {
            line.erase(line.find_last_not_of("\r\n") + 1);
            try {
                double v = std::stod(line);
                if (v > 8000 && sr == "N/A" && line.find('.') == std::string::npos) {
                    sr = std::to_string((long long)v) + " Hz";
                } else if (v > 10000 && br == "N/A") {
                    std::ostringstream o;
                    o << std::fixed << std::setprecision(0) << v / 1000.0;
                    br = o.str() + " kbps";
                } else if (v > 0 && v < 10 && ch == "N/A") {
                    ch = ((int)v == 1 ? "Mono" : (int)v == 2 ? "Stereo" : std::to_string((int)v) + " Ch");
                } else if (dur == "N/A") {
                    int h = (int)(v / 3600);
                    int m = (int)((v - h * 3600) / 60);
                    int s = (int)(v - h * 3600 - m * 60);
                    char buf[16];
                    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", h, m, s);
                    dur = buf;
                }
            } catch (...) {}
        }
        
        std::ostringstream os;
        os << std::fixed << std::setprecision(2) << sizeMb;
        
        g_app.log.add("FILE LOADED", V_MINT);
        g_app.log.add("Name: " + p.filename().string() + " | " + dur + " | " + os.str() + " MB", V_TEXT);
    } catch (...) {}
}

static void LogConversionResult(const ProcResult& res, const std::string& outFile) {
    if (res.success) {
        g_app.log.add("Success. " + FmtSize(outFile), V_MINT);
        g_app.setStatus("Done.");
    } else {
        g_app.log.add("Conversion failed.", V_RED);
        g_app.setStatus("Error.");
    }
}

static bool PromptValue(const std::string& q, const std::string& def, DialogStep step, double& out) {
    g_app.dialogQuestion = q;
    snprintf(g_app.dialogBuf, sizeof(g_app.dialogBuf), "%s", def.c_str());
    
    g_app.dialogStep = step;
    g_app.dialogConfirmed = false;
    g_app.dialogCancelled = false;
    
    g_app.dialogOpen.store(true, std::memory_order_release);
    while (g_app.dialogOpen.load(std::memory_order_acquire)) {
        Sleep(30);
    }
    
    if (g_app.dialogCancelled) return false;
    
    if (step == DialogStep::WaitingOffset) {
        out = g_app.resultOffset;
    } else if (step == DialogStep::WaitingBpm) {
        out = g_app.resultBpm;
    } else {
        out = g_app.resultTime;
    }
    
    return true;
}

// ──────────────────────────────────────────────────────────────
//  Core conversion
// ──────────────────────────────────────────────────────────────
struct ConvParams {
    std::string seekArg;
    std::string filterArg;
    std::string trimArg;
    std::string newFileName;
};

static bool BuildConvParams(const std::string& inputFile, double offset, double bpm, double time, const std::string& origName, const std::string& ext, ConvParams& out) {
    std::vector<std::string> filters;
    
    if (g_app.applyLufs) {
        auto ar = RunFfmpeg("-i \"" + inputFile + "\" -af ebur128 -f null -");
        double lufs = 0.0;
        if (ParseEbur128(ar.output, lufs)) {
            double gain = -8.0 - lufs;
            std::ostringstream o;
            o << std::fixed << std::setprecision(2) << gain;
            filters.push_back("volume=" + o.str() + "dB");
        }
    }
    
    if (offset > 0) {
        int ms = (int)(offset * 1000);
        filters.insert(filters.begin(), "adelay=" + std::to_string(ms) + "|" + std::to_string(ms));
    }
    
    if (offset < 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "-ss %.3f", std::abs(offset));
        out.seekArg = buf;
    }
    
    double trimSec = -1.0;
    if (g_app.calcMadi) {
        double songLen = GetDuration(inputFile);
        if (songLen <= 0) return false;
        
        long long sm = (long long)std::ceil((bpm / 60.0) * (time + offset) / 4.0 - 1.0);
        long long tm = (long long)std::ceil((bpm * songLen) / (4.0 * 60.0));
        
        ApplyMadiParity(sm, tm, bpm, trimSec);
        g_app.setResults(bpm, sm, tm);
        
        out.newFileName = "S_" + origName + "_" + FmtBpm(bpm) + "_" + std::to_string(sm) + "_" + std::to_string(tm) + ext;
    } else {
        out.newFileName = "edited_" + origName + ext;
    }
    
    if (!filters.empty()) {
        std::string j;
        for (size_t i = 0; i < filters.size(); ++i) {
            if (i > 0) j += ",";
            j += filters[i];
        }
        out.filterArg = "-af \"" + j + "\"";
    }
    
    if (trimSec > 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "-t %.6f", trimSec);
        out.trimArg = buf;
    }
    
    return true;
}

static void DoConvertToWav(const std::string& f) {
    fs::path p(f);
    std::string out = (p.parent_path() / p.stem()).string() + ".wav";
    RunFfmpeg("-i \"" + f + "\" -c:a pcm_s16le -ar 44100 -rf64 never -map_metadata -1 -fflags +bitexact -y \"" + out + "\"");
}

static void DoConvertWavToWav(const std::string& f, double offset, double bpm, double time) {
    fs::path p(f);
    ConvParams cp;
    if (!BuildConvParams(f, offset, bpm, time, p.stem().string(), ".wav", cp)) return;
    
    std::string out = p.parent_path().string() + "\\" + cp.newFileName;
    LogConversionResult(RunFfmpeg(cp.seekArg + " -i \"" + f + "\" " + cp.filterArg + " " + cp.trimArg + " -c:a pcm_s16le -ar 44100 -ac 2 -rf64 never -map_metadata -1 -fflags +bitexact -y \"" + out + "\""), out);
}

static void DoConvertWavToOgg(const std::string& f, double offset, double bpm, double time) {
    fs::path p(f);
    ConvParams cp;
    if (!BuildConvParams(f, offset, bpm, time, p.stem().string(), ".ogg", cp)) return;
    
    std::string outOgg = p.parent_path().string() + "\\" + cp.newFileName;
    std::string tempWav = p.parent_path().string() + "\\_silencer_temp.wav";
    
    g_app.log.add("Rendering intermediate audio...", V_SUBTEXT);
    auto res1 = RunFfmpeg(cp.seekArg + " -i \"" + f + "\" " + cp.filterArg + " " + cp.trimArg + " -c:a pcm_s16le -ar 44100 -ac 2 -rf64 never -map_metadata -1 -y \"" + tempWav + "\"");
    
    if (!res1.success) {
        g_app.log.add("Failed to process intermediate audio.", V_RED);
        g_app.setStatus("Error.");
        return;
    }
    
    g_app.log.add("Encoding standard Xiph.Org Vorbis (oggenc2)...", V_SUBTEXT);
    std::string oggCmd = "-q 2 -o \"" + outOgg + "\" \"" + tempWav + "\"";
    auto res2 = RunProcess(OggEncPath(), oggCmd);
    
    DeleteFileA(tempWav.c_str());
    
    if (res2.success) {
        LogConversionResult(res2, outOgg);
    } else {
        g_app.log.add("oggenc2 failed to encode.", V_RED);
        g_app.setStatus("Error.");
    }
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

static void ProcessFile(std::string inputFile) {
    g_app.busy = true;
    g_app.log.clear();
    g_app.clearResults();
    g_app.setStatus("Processing...");
    
    LogFileDetails(inputFile);
    
    fs::path p(inputFile);
    std::string ext = p.extension().string();
    for (auto& c : ext) {
        c = (char)tolower((unsigned char)c);
    }
    
    std::string activeFile = inputFile;
    
    if (ext == ".flac" || ext == ".ogg" || ext == ".mp3") {
        g_app.log.add("Converting to intermediate WAV for processing...", V_WARN);
        DoConvertToWav(inputFile);
        
        activeFile = (p.parent_path() / p.stem()).string() + ".wav";
        ext = ".wav";
        
        if (GetFileAttributesA(activeFile.c_str()) == INVALID_FILE_ATTRIBUTES) {
            g_app.log.add("Error: Intermediate WAV was not created.", V_RED);
            g_app.setStatus("Error.");
            g_app.busy = false;
            return;
        }
    }
    
    if (ext != ".wav") {
        g_app.log.add("Unsupported file type.", V_RED);
        g_app.setStatus("Invalid file.");
        g_app.busy = false;
        return;
    }
    
    double offset = 0.0;
    if (!PromptValue("Enter Manual Offset (e.g. 1.234)", ClipboardDefaultOffset(), DialogStep::WaitingOffset, offset)) {
        g_app.setStatus("Cancelled.");
        g_app.busy = false;
        return;
    }
    
    double bpm = 120.0, time = 0.0;
    if (g_app.calcMadi) {
        g_app.setStatus("Detecting BPM...");
        double det = GetBpm(activeFile);
        if (det > 0) bpm = det;
        
        if (!PromptValue("Enter BPM", FmtBpm(bpm), DialogStep::WaitingBpm, bpm)) {
            g_app.setStatus("Cancelled.");
            g_app.busy = false;
            return;
        }
        
        if (!PromptValue("Enter Time (First Space)", "0.0", DialogStep::WaitingTime, time)) {
            g_app.setStatus("Cancelled.");
            g_app.busy = false;
            return;
        }
    }
    
    g_app.setStatus("Processing final output...");
    if (g_app.wavMode) {
        DoConvertWavToWav(activeFile, offset, bpm, time);
    } else {
        DoConvertWavToOgg(activeFile, offset, bpm, time);
    }
    
    g_app.busy = false;
}

static std::string OpenFileDialog() {
    char buf[MAX_PATH]{};
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = "Audio Files\0*.flac;*.wav;*.ogg;*.mp3\0WAV Files\0*.wav\0"
                      "OGG Files\0*.ogg\0FLAC Files\0*.flac\0MP3 Files\0*.mp3\0\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
    ofn.lpstrTitle = "Select Audio File";
    
    if (GetOpenFileNameA(&ofn)) {
        return buf;
    }
    return "";
}

static void LaunchWorker(const std::string& file) {
    if (g_app.busy) return;
    
    auto exists = [](const std::string& p) {
        return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES;
    };
    
    if (!exists(FfmpegPath()) || !exists(FfprobePath())) {
        g_app.log.clear();
        g_app.log.add("Error: ffmpeg.exe / ffprobe.exe not found!", V_RED);
        g_app.setStatus("Missing ffmpeg.");
        return;
    }
    
    if (!g_app.wavMode && !exists(OggEncPath())) {
        g_app.log.clear();
        g_app.log.add("Error: oggenc2.exe not found!", V_RED);
        g_app.log.add("Please place oggenc2.exe next to Silencer.exe for OGG support.", V_SUBTEXT);
        g_app.setStatus("Missing oggenc2.");
        return;
    }
    
    if (g_app.worker.joinable()) {
        g_app.worker.join();
    }
    
    g_app.worker = std::thread(ProcessFile, file);
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

static void EndPanel() {
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

static void PanelTitle(const char* title) {
    if (g_fontSemiBold) ImGui::PushFont(g_fontSemiBold);
    ImGui::PushStyleColor(ImGuiCol_Text, UI_SUBTEXT);
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    if (g_fontSemiBold) ImGui::PopFont();
    ImGui::Spacing();
    ImGui::Dummy({0, 4.f});
}

static bool DrawToggle(const char* id, bool* v, float w = 48.f, float h = 26.f) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float r = h * 0.5f;
    
    ImGui::InvisibleButton(id, {w, h});
    bool clicked = ImGui::IsItemClicked();
    if (clicked) *v = !*v;
    
    ImU32 trackCol = *v ? UI_MINT : UI_BORDER;
    dl->AddRectFilled(pos, {pos.x + w, pos.y + h}, trackCol, r);
    
    float cx = *v ? pos.x + w - r : pos.x + r;
    dl->AddCircleFilled({cx, pos.y + r}, r - 4.f, IM_COL32(255, 255, 255, 255));
    return clicked;
}

static void DrawDashedRect(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float radius = 12.f, float dash = 8.f, float gap = 6.f) {
    auto seg = [&](ImVec2 p1, ImVec2 p2) {
        float dx = p2.x - p1.x;
        float dy = p2.y - p1.y;
        float len = std::sqrt(dx * dx + dy * dy);
        dx /= len;
        dy /= len;
        float t = 0;
        
        while (t < len) {
            float t2 = std::min(t + dash, len);
            dl->AddLine({p1.x + dx * t, p1.y + dy * t}, {p1.x + dx * t2, p1.y + dy * t2}, col, 2.0f);
            t += dash + gap;
        }
    };
    
    float r = radius;
    seg({a.x + r, a.y}, {b.x - r, a.y});
    seg({a.x + r, b.y}, {b.x - r, b.y});
    seg({a.x, a.y + r}, {a.x, b.y - r});
    seg({b.x, a.y + r}, {b.x, b.y - r});
    
    auto arc = [&](ImVec2 c, float sa, float ea) {
        int steps = 6;
        float step = (ea - sa) / steps;
        for (int i = 0; i < steps; i += 2) {
            float a1 = sa + i * step;
            float a2 = sa + (i + 1) * step;
            dl->AddLine({c.x + r * std::cos(a1), c.y + r * std::sin(a1)},
                        {c.x + r * std::cos(a2), c.y + r * std::sin(a2)}, col, 2.0f);
        }
    };
    
    constexpr float PI = 3.14159265f;
    arc({a.x + r, a.y + r}, PI, PI * 1.5f);
    arc({b.x - r, a.y + r}, PI * 1.5f, PI * 2.f);
    arc({b.x - r, b.y - r}, 0, PI * 0.5f);
    arc({a.x + r, b.y - r}, PI * 0.5f, PI);
}

// ──────────────────────────────────────────────────────────────
//  Main UI Rendering (1080x720 Wide Layout)
// ──────────────────────────────────────────────────────────────
static void RenderUI() {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize(io.DisplaySize);
    
    ImGui::PushStyleColor(ImGuiCol_WindowBg, UI_BG);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {32.f, 32.f});
    
    ImGui::Begin("##root", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoScrollbar);
    
    bool busy = g_app.busy.load();
    float winW = ImGui::GetContentRegionAvail().x;
    float winH = ImGui::GetContentRegionAvail().y;

    // ── HEADER ────────────────────────────────────────────────
    if (g_fontSemiBold) ImGui::PushFont(g_fontSemiBold);
    ImGui::PushStyleColor(ImGuiCol_Text, UI_TEXT);
    ImGui::SetWindowFontScale(1.8f);
    ImGui::Text("SILENCER");
    ImGui::SetWindowFontScale(1.f);
    ImGui::PopStyleColor();
    
    if (g_fontSemiBold) ImGui::PopFont();
    
    ImGui::SameLine();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 10.f);
    ImGui::PushStyleColor(ImGuiCol_Text, UI_SUBTEXT);
    ImGui::Text("v1.6");
    ImGui::PopStyleColor();

    // Diagnostics Button (Top Right)
    {
        const char* diagLabel = g_diag.hasError ? "! Diagnostics" : "Diagnostics";
        // FIX: Nutze UI_MINT als Standardfarbe statt UI_BORDER, damit er nicht grau ist
        ImU32 diagCol = g_diag.hasError ? UI_RED : UI_MINT; 
        
        ImGui::SameLine(winW - 110.f);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 2.f);
        
        ImGui::PushStyleColor(ImGuiCol_Button, UI_PANEL);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, UI_BORDER);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, UI_MINT_DK);
        ImGui::PushStyleColor(ImGuiCol_Text, diagCol);
        ImGui::PushStyleColor(ImGuiCol_Border, diagCol);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
        
        if (g_fontSemiBold) ImGui::PushFont(g_fontSemiBold);
        if (ImGui::Button(diagLabel, {110.f, 32.f})) {
            RunDiagnostics();
            g_diag.visible = !g_diag.visible;
        }
        if (g_fontSemiBold) ImGui::PopFont();
        
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(5);
    }

    ImGui::Dummy({0, 16.f});

    // ── 2-COLUMN LAYOUT SETUP ─────────────────────────────────
    float leftWidth = 420.f;
    float gap = 24.f;
    float rightWidth = winW - leftWidth - gap;
    
    ImGui::BeginGroup(); // Left Column

    // Settings Panel
    BeginPanel("##settings", {leftWidth, 230.f});
    PanelTitle("Configuration");
    
    struct TD {
        const char* id;
        bool* v;
        const char* label;
        const char* desc;
    };
    
    TD toggles[] = {
        {"##lufs", &g_app.applyLufs, "LUFS Normalization", "-8 LUFS Target"},
        {"##madi", &g_app.calcMadi, "Calculate Madi", "StartMadi / TotalMadi"},
        {"##wav",  &g_app.wavMode,  "WAV Output", "pcm_s16le mode"},
    };
    
    if (busy) ImGui::BeginDisabled();
    
    for (auto& t : toggles) {
        float rowY = ImGui::GetCursorPosY();
        ImGui::SetCursorPosY(rowY + 3.f);
        
        if (g_fontSemiBold) ImGui::PushFont(g_fontSemiBold);
        ImGui::PushStyleColor(ImGuiCol_Text, UI_TEXT);
        ImGui::Text("%s", t.label);
        ImGui::PopStyleColor();
        if (g_fontSemiBold) ImGui::PopFont();
        
        ImGui::SameLine(leftWidth - 48.f - 40.f);
        ImGui::SetCursorPosY(rowY);
        DrawToggle(t.id, t.v);
        
        ImGui::Spacing();
        ImGui::Dummy({0, 4.f});
    }
    
    if (busy) ImGui::EndDisabled();
    EndPanel();

    ImGui::Dummy({0, gap});

    // Status / Result Panel
    BeginPanel("##results", {leftWidth, winH - 230.f - gap - 64.f});
    PanelTitle("Output Details");
    
    bool hasRes;
    double dBpm;
    long long dSm, dTm;
    {
        std::lock_guard l(g_app.resMtx);
        hasRes = g_app.hasResults;
        dBpm = g_app.displayBpm;
        dSm = g_app.displaySM;
        dTm = g_app.displayTM;
    }
    
    auto drawMetric = [](const char* lbl, const std::string& val) {
        ImGui::PushStyleColor(ImGuiCol_Text, UI_SUBTEXT);
        ImGui::TextUnformatted(lbl);
        ImGui::PopStyleColor();
        
        ImGui::SameLine(180.f);
        
        if (g_fontSemiBold) ImGui::PushFont(g_fontSemiBold);
        ImGui::PushStyleColor(ImGuiCol_Text, UI_MINT);
        ImGui::TextUnformatted(val.c_str());
        ImGui::PopStyleColor();
        if (g_fontSemiBold) ImGui::PopFont();
        
        ImGui::Spacing();
        ImGui::Dummy({0, 6.f});
    };

    if (hasRes) {
        drawMetric("Tempo (BPM)", FmtBpm(dBpm));
        drawMetric("Start Madi", std::to_string(dSm));
        drawMetric("Total Madi", std::to_string(dTm));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, UI_BORDER);
        ImGui::Text("Awaiting file processing...");
        ImGui::PopStyleColor();
    }
    
    EndPanel();
    ImGui::EndGroup(); // End Left Column
    
    ImGui::SameLine(0, gap);
    
    ImGui::BeginGroup(); // Right Column

    // Diagnostics Panel (Expands dynamically on the right if toggled)
    if (g_diag.visible && !g_diag.entries.empty()) {
        float diagH = (float)g_diag.entries.size() * 44.f + 40.f;
        BeginPanel("##diag_panel", {rightWidth, diagH});
        PanelTitle("System Diagnostics & Tool Paths");
        
        for (auto& e : g_diag.entries) {
            ImU32 col; const char* statusTxt;
            switch (e.status) {
                case DiagStatus::OK:      col = UI_MINT;   statusTxt = "[OK]"; break;
                case DiagStatus::Warning: col = UI_WARN; statusTxt = "[WARN]"; break;
                case DiagStatus::Error:   col = UI_RED;    statusTxt = "[ERR]"; break;
                default:                  col = UI_SUBTEXT; statusTxt = "[?]"; break;
            }
            
            if (g_fontSemiBold) ImGui::PushFont(g_fontSemiBold);
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            ImGui::Text("%-7s", statusTxt);
            ImGui::PopStyleColor();
            if (g_fontSemiBold) ImGui::PopFont();
            
            ImGui::SameLine();
            if (g_fontSemiBold) ImGui::PushFont(g_fontSemiBold);
            ImGui::PushStyleColor(ImGuiCol_Text, UI_TEXT);
            ImGui::Text("%s", e.label.c_str());
            ImGui::PopStyleColor();
            if (g_fontSemiBold) ImGui::PopFont();
            
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, UI_SUBTEXT);
            ImGui::TextUnformatted(("-> " + e.detail).c_str());
            ImGui::PopStyleColor();
        }
        EndPanel();
        ImGui::Dummy({0, gap});
    }

    // Dropzone Panel
    float dzH = g_diag.visible ? 140.f : 200.f;
    ImVec2 dzPos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##dz", {rightWidth, dzH});
    bool dzHover = ImGui::IsItemHovered();
    
    if (ImGui::IsItemClicked() && !busy) {
        std::string f = OpenFileDialog();
        if (!f.empty()) {
            LaunchWorker(f);
        }
    }
    
    ImU32 dzBg = dzHover ? UI_BORDER : UI_PANEL;
    ImU32 dzBdr = dzHover ? UI_MINT : UI_BORDER;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    
    dl->AddRectFilled(dzPos, {dzPos.x + rightWidth, dzPos.y + dzH}, dzBg, 12.f);
    DrawDashedRect(dl, dzPos, {dzPos.x + rightWidth, dzPos.y + dzH}, dzBdr, 12.f, 10.f, 8.f);
    
    float cx = dzPos.x + rightWidth * 0.5f;
    float cy = dzPos.y + dzH * 0.5f;
    const char* mainTxt = busy ? "Processing file..." : "Drop audio file here";
    const char* subTxt  = ".wav   .flac   .ogg   .mp3";
    
    if (g_fontSemiBold) ImGui::PushFont(g_fontSemiBold);
    ImVec2 ms = ImGui::CalcTextSize(mainTxt);
    dl->AddText({cx - ms.x * 0.5f, cy - 14.f}, dzHover ? UI_TEXT : UI_MINT, mainTxt);
    if (g_fontSemiBold) ImGui::PopFont();
    
    ImVec2 ss = ImGui::CalcTextSize(subTxt);
    dl->AddText({cx - ss.x * 0.5f, cy + 14.f}, UI_SUBTEXT, subTxt);
    
    ImGui::Dummy({0, gap});

    // Log Panel
    float diagOffset = g_diag.visible ? (float)g_diag.entries.size() * 44.f + 40.f + gap : 0.f;
    BeginPanel("##log", {rightWidth, winH - dzH - gap - 64.f - diagOffset});
    PanelTitle("Console Log");
    
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0,0,0,0));
    if (ImGui::BeginChild("##log_inner", {-1.f, -1.f}, false, ImGuiWindowFlags_HorizontalScrollbar)) {
        g_app.log.draw();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    
    EndPanel();
    ImGui::EndGroup(); // End Right Column
    
    // ── STATUS BAR ────────────────────────────────────────────
    std::string status = g_app.getStatus();
    ImVec4 sc = busy ? V_WARN : status == "Done." ? V_MINT : status == "Error." ? V_RED : V_SUBTEXT;
    ImVec2 dotPos = {ImGui::GetWindowPos().x + 32.f, ImGui::GetWindowPos().y + io.DisplaySize.y - 32.f};
    
    ImU32 dotCol;
    if (busy) {
        float t = (float)ImGui::GetTime();
        float a = 0.4f + 0.6f * (std::sin(t * 5.f) * .5f + .5f);
        dotCol = IM_COL32(235, 180, 90, (int)(a * 255));
    } else if (status == "Done.") {
        dotCol = UI_MINT;
    } else if (status == "Error.") {
        dotCol = UI_RED;
    } else {
        dotCol = UI_SUBTEXT;
    }
    
    dl->AddCircleFilled(dotPos, 5.f, dotCol);
    ImGui::SetCursorScreenPos({dotPos.x + 16.f, dotPos.y - 8.f});
    
    if (busy) {
        int dots = (int)(ImGui::GetTime() * 2) % 4;
        status += std::string(dots, '.');
    }
    
    if (g_fontSemiBold) ImGui::PushFont(g_fontSemiBold);
    ImGui::TextColored(sc, "%s", status.c_str());
    if (g_fontSemiBold) ImGui::PopFont();
    
    // ── DIALOG OVERLAY ────────────────────────────────────────
    if (g_app.dialogOpen.load(std::memory_order_acquire)) {
        ImDrawList* bg = ImGui::GetBackgroundDrawList();
        bg->AddRectFilled({0,0}, io.DisplaySize, IM_COL32(0,0,0,180));
        
        ImGui::SetNextWindowSize({460.f, 210.f}, ImGuiCond_Always);
        ImGui::SetNextWindowPos({io.DisplaySize.x * .5f, io.DisplaySize.y * .5f}, ImGuiCond_Always, {.5f, .5f});
        ImGui::SetNextWindowFocus();
        
        ImGui::PushStyleColor(ImGuiCol_WindowBg, UI_PANEL);
        ImGui::PushStyleColor(ImGuiCol_Border, UI_BORDER);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 16.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {28.f, 24.f});
        
        ImGui::Begin("##inputdlg", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar);

        ImDrawList* ddl = ImGui::GetWindowDrawList();
        ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
        ddl->AddRectFilled({wp.x + 12.f, wp.y}, {wp.x + ws.x - 12.f, wp.y + 2.f}, UI_MINT, 1.f);
        
        ImGui::Dummy({0, 6.f});

        if (g_fontSemiBold) ImGui::PushFont(g_fontSemiBold);
        ImGui::PushStyleColor(ImGuiCol_Text, UI_MINT);
        ImGui::SetWindowFontScale(1.1f);
        ImGui::TextUnformatted(g_app.dialogQuestion.c_str());
        ImGui::SetWindowFontScale(1.f);
        ImGui::PopStyleColor();
        if (g_fontSemiBold) ImGui::PopFont();
        
        ImGui::Spacing();
        ImGui::Dummy({0, 10.f});

        static bool needsFocus = false, lastOpen = false;
        bool curOpen2 = true;
        if (!lastOpen && curOpen2) needsFocus = true;
        lastOpen = curOpen2;

        ImGui::PushStyleColor(ImGuiCol_FrameBg, UI_BG);
        ImGui::PushStyleColor(ImGuiCol_Text, UI_TEXT);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {12.f, 10.f});
        ImGui::SetNextItemWidth(-1.f);
        
        if (needsFocus) {
            ImGui::SetKeyboardFocusHere();
            needsFocus = false;
        }
        
        bool enter = ImGui::InputText("##dlginput", g_app.dialogBuf, sizeof(g_app.dialogBuf), ImGuiInputTextFlags_EnterReturnsTrue);
        
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
        
        ImGui::Spacing();
        ImGui::Dummy({0, 12.f});

        auto commit = [&]() {
            double v = 0.0;
            try {
                v = std::stod(g_app.dialogBuf);
            } catch (...) {}
            
            switch (g_app.dialogStep) {
                case DialogStep::WaitingOffset: g_app.resultOffset = v; break;
                case DialogStep::WaitingBpm:    g_app.resultBpm = v; break;
                case DialogStep::WaitingTime:   g_app.resultTime = v; break;
                default: break;
            }
            lastOpen = false;
            g_app.dialogConfirmed.store(true);
            g_app.dialogOpen.store(false, std::memory_order_release);
        };

        float btnW = (ImGui::GetContentRegionAvail().x - 12.f) * .5f;
        
        ImGui::PushStyleColor(ImGuiCol_Button, UI_MINT_DK);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, UI_MINT);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, UI_MINT);
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(20, 20, 20, 255));
        
        if (g_fontSemiBold) ImGui::PushFont(g_fontSemiBold);
        if (ImGui::Button("Confirm", {btnW, 36.f}) || enter) {
            commit();
        }
        if (g_fontSemiBold) ImGui::PopFont();
        
        ImGui::PopStyleColor(4);
        
        ImGui::SameLine(0.f, 12.f);
        
        ImGui::PushStyleColor(ImGuiCol_Button, UI_BORDER);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(60, 60, 65, 255));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(70, 70, 75, 255));
        ImGui::PushStyleColor(ImGuiCol_Text, UI_TEXT);
        
        if (ImGui::Button("Cancel", {btnW, 36.f})) {
            lastOpen = false;
            g_app.dialogCancelled.store(true);
            g_app.dialogOpen.store(false, std::memory_order_release);
        }
        
        ImGui::PopStyleColor(4);
        
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
    }

    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

// ──────────────────────────────────────────────────────────────
//  DirectX 11 Boilerplate
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
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate = {60, 1};
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    
    D3D_FEATURE_LEVEL fl;
    const D3D_FEATURE_LEVEL fla[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        fla, 2, D3D11_SDK_VERSION, &sd, &g_pSwap, &g_pDevice, &fl, &g_pContext))) {
        return false;
    }
    
    CreateRTV();
    return true;
}

static void CleanupD3D() {
    if (g_pRTV) { g_pRTV->Release(); g_pRTV = nullptr; }
    if (g_pSwap) { g_pSwap->Release(); g_pSwap = nullptr; }
    if (g_pContext) { g_pContext->Release(); g_pContext = nullptr; }
    if (g_pDevice) { g_pDevice->Release(); g_pDevice = nullptr; }
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) {
        return 1;
    }
    
    switch (msg) {
        case WM_SIZE:
            if (g_pDevice && wParam != SIZE_MINIMIZED) {
                if (g_pRTV) {
                    g_pRTV->Release();
                    g_pRTV = nullptr;
                }
                g_pSwap->ResizeBuffers(0, LOWORD(lParam), HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
                CreateRTV();
            }
            return 0;
            
        case WM_DROPFILES: {
            HDROP hd = (HDROP)wParam;
            char buf[MAX_PATH]{};
            if (DragQueryFileA(hd, 0, buf, MAX_PATH) && !g_app.busy.load()) {
                LaunchWorker(buf);
            }
            DragFinish(hd);
            return 0;
        }
        
        case WM_GETMINMAXINFO: {
            MINMAXINFO* mmi = (MINMAXINFO*)lParam;
            mmi->ptMinTrackSize = {1080, 720};
            mmi->ptMaxTrackSize = {1080, 720};
            return 0;
        }
        
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
            break;
            
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, WndProc, 0, 0, hInst,
                   LoadIconW(hInst, MAKEINTRESOURCEW(101)), nullptr,
                   nullptr, nullptr, L"SilencerImGui",
                   LoadIconW(hInst, MAKEINTRESOURCEW(101))};
    RegisterClassExW(&wc);

    DWORD style = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"Silencer v1.6 - by Sanya", style,
        CW_USEDEFAULT, CW_USEDEFAULT, 1080, 720, nullptr, nullptr, hInst, nullptr);

    DragAcceptFiles(hwnd, TRUE);
    
    if (!InitD3D(hwnd)) {
        CleanupD3D();
        return 1;
    }
    
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;

    // ── Font Loading ────────────────────────────────────────────
#ifdef FONTS_EMBEDDED
    ImFontConfig font_cfg;
    font_cfg.FontDataOwnedByAtlas = false;
    
    g_fontRegular  = io.Fonts->AddFontFromMemoryTTF(
        (void*)poppins_regular_compressed_data, 
        poppins_regular_compressed_size, 
        18.f, 
        &font_cfg
    );
    
    g_fontSemiBold = io.Fonts->AddFontFromMemoryTTF(
        (void*)poppins_semibold_compressed_data, 
        poppins_semibold_compressed_size, 
        18.f, 
        &font_cfg
    );
#else
    char exeBuf[MAX_PATH]{};
    GetModuleFileNameA(nullptr, exeBuf, MAX_PATH);
    fs::path exeDir = fs::path(exeBuf).parent_path();
    
    std::string pathReg = (exeDir / "Poppins-Regular.ttf").string();
    std::string pathSemi = (exeDir / "Poppins-SemiBold.ttf").string();
    
    if (GetFileAttributesA(pathReg.c_str()) != INVALID_FILE_ATTRIBUTES) {
        g_fontRegular = io.Fonts->AddFontFromFileTTF(pathReg.c_str(), 18.f);
    }
    
    if (GetFileAttributesA(pathSemi.c_str()) != INVALID_FILE_ATTRIBUTES) {
        g_fontSemiBold = io.Fonts->AddFontFromFileTTF(pathSemi.c_str(), 18.f);
    }
#endif

    if (!g_fontRegular) {
        io.Fonts->AddFontDefault();
    }
    
    io.Fonts->Build();

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pDevice, g_pContext);

    RunDiagnostics();

    constexpr float clear[4] = {15/255.f, 15/255.f, 18/255.f, 1.f}; // UI_BG
    MSG msg{};
    
    while (true) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) goto done;
        }
        
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        
        RenderUI();
        
        ImGui::Render();
        g_pContext->OMSetRenderTargets(1, &g_pRTV, nullptr);
        g_pContext->ClearRenderTargetView(g_pRTV, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwap->Present(1, 0);
    }
    
done:
    if (g_app.worker.joinable()) {
        g_app.dialogCancelled.store(true);
        g_app.dialogOpen.store(false, std::memory_order_release);
        g_app.worker.join();
    }
    
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    
    CleanupD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, hInst);
    
    return 0;
}