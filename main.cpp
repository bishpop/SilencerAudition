// =============================================================
//  Silencer v1.5  –  Midnight Elegance Theme (C++ / ImGui)
//
//  Build requirements:
//    - Dear ImGui (https://github.com/ocornut/imgui)
//    - ffmpeg.exe + ffprobe.exe next to the .exe
//    - MSVC 2022 or MinGW-w64
//    - Link: d3d11.lib dxgi.lib comdlg32.lib shell32.lib
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
//  Midnight Elegance Palette
// ──────────────────────────────────────────────────────────────
#define C_BG_DARK       IM_COL32(10, 5, 22, 255)     // Frame / Dark backgrounds
#define C_BG_MAIN       IM_COL32(18, 10, 40, 255)    // Toolbar / Main background
#define C_BG_LIGHT      IM_COL32(31, 21, 61, 255)    // Omnibox / Card backgrounds
#define C_BG_HOVER      IM_COL32(46, 31, 89, 255)    // Hover elements

#define C_TEXT_MAIN     IM_COL32(240, 241, 245, 255) // Primary readable text
#define C_TEXT_MUTED    IM_COL32(147, 138, 169, 255) // Subtext / Outlines

#define C_ACCENT_GOLD   IM_COL32(212, 175, 55, 255)  // Luxurious Gold
#define C_SUCCESS       IM_COL32(80, 180, 120, 255)  // Classy Emerald
#define C_ERROR         IM_COL32(220, 80, 80, 255)   // Muted Red

static constexpr ImVec4 V_TEXT_MAIN   = {240.f/255.f, 241.f/255.f, 245.f/255.f, 1.f};
static constexpr ImVec4 V_TEXT_MUTED  = {147.f/255.f, 138.f/255.f, 169.f/255.f, 1.f};
static constexpr ImVec4 V_ACCENT_GOLD = {212.f/255.f, 175.f/255.f, 55.f/255.f, 1.f};
static constexpr ImVec4 V_SUCCESS     = {80.f/255.f,  180.f/255.f, 120.f/255.f, 1.f};
static constexpr ImVec4 V_ERROR       = {220.f/255.f, 80.f/255.f,  80.f/255.f,  1.f};
static constexpr ImVec4 V_DEFAULT     = V_TEXT_MAIN;

// ──────────────────────────────────────────────────────────────
//  Thread-safe log
// ──────────────────────────────────────────────────────────────
struct LogEntry { std::string text; ImVec4 color; };

struct AppLog {
    std::vector<LogEntry> entries;
    std::mutex            mtx;
    bool                  autoScroll = true;

    void add(const std::string& text, ImVec4 color = V_DEFAULT) {
        std::lock_guard lock(mtx);
        std::istringstream ss(text); std::string line;
        while (std::getline(ss, line))
            entries.push_back({line, color});
        autoScroll = true;
    }
    void clear() { std::lock_guard lock(mtx); entries.clear(); }
    void draw()  {
        std::lock_guard lock(mtx);
        for (auto& e : entries) {
            ImGui::PushStyleColor(ImGuiCol_Text, e.color);
            ImGui::TextUnformatted(e.text.c_str());
            ImGui::PopStyleColor();
        }
        if (autoScroll) { ImGui::SetScrollHereY(1.f); autoScroll = false; }
    }
};

// ──────────────────────────────────────────────────────────────
//  Application state
// ──────────────────────────────────────────────────────────────
enum class DialogStep { None, WaitingOffset, WaitingBpm, WaitingTime };

struct AppState {
    // settings
    bool applyLufs = true;
    bool calcMadi  = true;
    bool wavMode   = false;

    // status
    std::mutex  statusMtx;
    std::string statusMsg = "Ready";
    void setStatus(const std::string& s) { std::lock_guard l(statusMtx); statusMsg = s; }
    std::string getStatus()              { std::lock_guard l(statusMtx); return statusMsg; }

    // results for display cards
    std::mutex    resMtx;
    double        displayBpm       = 0.0;
    long long     displayStartMadi = 0;
    long long     displayTotalMadi = 0;
    bool          hasResults       = false;
    void setResults(double bpm, long long sm, long long tm) {
        std::lock_guard l(resMtx);
        displayBpm = bpm; displayStartMadi = sm; displayTotalMadi = tm;
        hasResults = true;
    }
    void clearResults() { std::lock_guard l(resMtx); hasResults = false; }

    AppLog log;
    std::atomic<bool> busy{false};

    // dialog
    std::atomic<bool> dialogOpen{false};
    std::atomic<bool> dialogConfirmed{false};
    std::atomic<bool> dialogCancelled{false};
    DialogStep dialogStep{DialogStep::None};
    char       dialogBuf[256]{}; // Vergrößerter Buffer für extra Sicherheit
    std::string dialogQuestion;

    double resultOffset = 0.0;
    double resultBpm    = 120.0;
    double resultTime   = 0.0;

    std::thread worker;
};
static AppState g_app;

// ──────────────────────────────────────────────────────────────
//  Process helpers
// ──────────────────────────────────────────────────────────────
struct ProcResult { bool success{}; std::string output; };

static ProcResult RunProcess(const std::string& exe,
                             const std::string& args,
                             bool captureStdout) {
    ProcResult res;
    std::string cmd = "\"" + exe + "\" " + args;
    SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};
    HANDLE rOut{},wOut{},rErr{},wErr{};
    CreatePipe(&rOut,&wOut,&sa,0); SetHandleInformation(rOut,HANDLE_FLAG_INHERIT,0);
    CreatePipe(&rErr,&wErr,&sa,0); SetHandleInformation(rErr,HANDLE_FLAG_INHERIT,0);
    STARTUPINFOA si{}; si.cb=sizeof(si); si.dwFlags=STARTF_USESTDHANDLES;
    si.hStdOutput=wOut; si.hStdError=wErr; si.hStdInput=GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::vector<char> buf(cmd.begin(),cmd.end()); buf.push_back('\0');
    if (!CreateProcessA(nullptr,buf.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi)) {
        CloseHandle(rOut);CloseHandle(wOut);CloseHandle(rErr);CloseHandle(wErr);
        res.output="CreateProcess failed ("+exe+")"; return res;
    }
    CloseHandle(wOut); CloseHandle(wErr);
    HANDLE hRead=captureStdout?rOut:rErr, hSkip=captureStdout?rErr:rOut;
    char tmp[4096]; DWORD n;
    while (ReadFile(hRead,tmp,sizeof(tmp)-1,&n,nullptr)&&n){tmp[n]='\0';res.output+=tmp;}
    while (ReadFile(hSkip,tmp,sizeof(tmp)-1,&n,nullptr)&&n){}
    WaitForSingleObject(pi.hProcess,INFINITE);
    DWORD code; GetExitCodeProcess(pi.hProcess,&code); res.success=(code==0);
    CloseHandle(pi.hProcess);CloseHandle(pi.hThread);CloseHandle(rOut);CloseHandle(rErr);
    return res;
}

static std::string FindTool(const std::string& name)
{
    auto exists = [](const std::string& p) {
        return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES;
    };

    char exeBuf[MAX_PATH]{};
    GetModuleFileNameA(nullptr, exeBuf, MAX_PATH);
    fs::path exeDir = fs::path(exeBuf).parent_path();
    std::string candidate = (exeDir / name).string();
    if (exists(candidate)) return candidate;

    char pathEnv[32768]{};
    GetEnvironmentVariableA("PATH", pathEnv, sizeof(pathEnv));
    std::istringstream ss(pathEnv);
    std::string dir;
    while (std::getline(ss, dir, ';')) {
        if (dir.empty()) continue;
        candidate = (fs::path(dir) / name).string();
        if (exists(candidate)) return candidate;
    }

    static const char* common[] = {
        "C:\\ffmpeg\\bin",
        "C:\\ffmpeg",
        "C:\\Program Files\\ffmpeg\\bin",
        "C:\\Program Files\\ffmpeg",
        nullptr
    };
    for (int i = 0; common[i]; ++i) {
        candidate = (fs::path(common[i]) / name).string();
        if (exists(candidate)) return candidate;
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

static ProcResult RunFfmpeg (const std::string& a){return RunProcess(FfmpegPath(), a,false);}
static ProcResult RunFfprobe(const std::string& a){return RunProcess(FfprobePath(),a,true );}

// ──────────────────────────────────────────────────────────────
//  Audio helpers
// ──────────────────────────────────────────────────────────────
static double GetDuration(const std::string& file) {
    auto r=RunFfprobe("-v error -show_entries format=duration -of default=noprint_wrappers=1:nokey=1 \""+file+"\"");
    try{return std::stod(r.output);}catch(...){return -1.0;}
}

static double GetBpm(const std::string& file) {
    auto r=RunFfmpeg("-i \""+file+"\" -af bpm -f null -");
    std::vector<double> v;
    std::regex re(R"(BPM:\s*([\d\.]+))");
    for (auto it=std::sregex_iterator(r.output.begin(),r.output.end(),re);
         it!=std::sregex_iterator{};++it) {
        try{double x=std::stod((*it)[1].str()); if(x>40&&x<300) v.push_back(x);}catch(...){}
    }
    if (v.empty()) return 0.0;
    std::sort(v.begin(),v.end()); size_t n=v.size();
    return (n%2==0)?(v[n/2-1]+v[n/2])/2.0:v[n/2];
}

static std::string FmtBpm(double bpm) {
    if (std::abs(std::fmod(bpm,1.0))<1e-9) return std::to_string((long long)bpm);
    std::ostringstream o; o<<std::fixed<<std::setprecision(3)<<bpm;
    std::string s=o.str(); s.erase(s.find_last_not_of('0')+1);
    if (s.back()=='.') s.pop_back(); return s;
}

static std::string FmtSize(const std::string& path) {
    try{double mb=(double)fs::file_size(path)/(1024.0*1024.0);
        std::ostringstream o;o<<std::fixed<<std::setprecision(2)<<mb;
        return "  ("+o.str()+" MB)";}catch(...){return "";}
}

static bool ParseEbur128(const std::string& out, double& lufs) {
    std::smatch m; std::regex re(R"(Integrated loudness:\s+I:\s+(-?[\d\.]+))");
    if (!std::regex_search(out,m,re)) return false;
    try{lufs=std::stod(m[1].str());return true;}catch(...){return false;}
}

static void ApplyMadiParity(long long start, long long& total, double bpm, double& trimSec) {
    if ((start%2==0)==(total%2==0)) --total;
    trimSec=(double)total*4.0*60.0/bpm;
}

// ──────────────────────────────────────────────────────────────
//  Logging helpers
// ──────────────────────────────────────────────────────────────
static void LogFileDetails(const std::string& file) {
    try {
        fs::path p(file);
        double sizeMb=(double)fs::file_size(p)/(1024.0*1024.0);
        auto r=RunFfprobe("-v error -select_streams a:0 "
            "-show_entries stream=channels,sample_rate,bit_rate:format=duration "
            "-of default=noprint_wrappers=1:nokey=1 \""+file+"\"");
        std::string dur{"N/A"},sr{"N/A"},br{"N/A"},ch{"N/A"};
        std::istringstream ss(r.output); std::string line;
        while(std::getline(ss,line)){
            line.erase(line.find_last_not_of("\r\n")+1);
            try{double v=std::stod(line);
                if (v>8000&&sr=="N/A"&&line.find('.')==std::string::npos)
                    sr=std::to_string((long long)v)+" Hz";
                else if(v>10000&&br=="N/A")
                {std::ostringstream o;o<<std::fixed<<std::setprecision(0)<<v/1000.0;br=o.str()+" kbps";}
                else if(v>0&&v<10&&ch=="N/A")
                    ch=((int)v==1?"Mono":(int)v==2?"Stereo":std::to_string((int)v)+" Ch");
                else if(dur=="N/A")
                {int h=(int)(v/3600),m=(int)((v-h*3600)/60),s=(int)(v-h*3600-m*60);
                 char buf[16];snprintf(buf,sizeof(buf),"%02d:%02d:%02d",h,m,s);dur=buf;}
            }catch(...){}
        }
        std::ostringstream os; os<<std::fixed<<std::setprecision(2)<<sizeMb;
        g_app.log.add("Input File Information",                 V_ACCENT_GOLD);
        g_app.log.add("  File     : "+p.filename().string(),    V_TEXT_MAIN);
        g_app.log.add("  Duration : "+dur,                      V_TEXT_MUTED);
        g_app.log.add("  Size     : "+os.str()+" MB",           V_TEXT_MUTED);
        g_app.log.add("  Bitrate  : "+br,                       V_TEXT_MUTED);
        g_app.log.add("  Sample   : "+sr,                       V_TEXT_MUTED);
        g_app.log.add("  Channels : "+ch,                       V_TEXT_MUTED);
        g_app.log.add("");
    } catch(...){ g_app.log.add("Could not read file details.",V_TEXT_MUTED); }
}

static void LogConversionResult(const ProcResult& res, const std::string& outFile) {
    if (res.success) {
        g_app.log.add("Conversion successful."+FmtSize(outFile), V_SUCCESS);
        g_app.setStatus("Done.");
    } else {
        g_app.log.add("Error: Conversion failed.", V_ERROR);
        g_app.log.add(res.output,                  V_TEXT_MUTED);
        g_app.setStatus("Error.");
    }
}

// ──────────────────────────────────────────────────────────────
//  Dialog helper (Sicher gegen Abstürze!)
// ──────────────────────────────────────────────────────────────
static bool PromptValue(const std::string& question,
                        const std::string& def,
                        DialogStep step, double& out) {
    g_app.dialogQuestion = question;
    
    // VERHINDERT ABSTÜRZE: Sicheres snprintf
    snprintf(g_app.dialogBuf, sizeof(g_app.dialogBuf), "%s", def.c_str());
    
    g_app.dialogStep      = step;
    g_app.dialogConfirmed = false;
    g_app.dialogCancelled = false;
    g_app.dialogOpen.store(true,std::memory_order_release);
    
    // Warte blockierend, bis der UI Thread die Eingabe bestätigt oder abbricht
    while (g_app.dialogOpen.load(std::memory_order_acquire)) {
        Sleep(30);
    }
    
    if (g_app.dialogCancelled) return false;
    
    out = (step==DialogStep::WaitingOffset) ? g_app.resultOffset
        : (step==DialogStep::WaitingBpm)   ? g_app.resultBpm
                                           : g_app.resultTime;
    return true;
}

// ──────────────────────────────────────────────────────────────
//  Core conversion
// ──────────────────────────────────────────────────────────────
struct ConvParams {
    std::string seekArg, filterArg, trimArg, newFileName;
};

static bool BuildConvParams(const std::string& inputFile,
                             double offset, double bpm, double time,
                             const std::string& origName, const std::string& ext,
                             ConvParams& out) {
    std::vector<std::string> filters;

    if (g_app.applyLufs) {
        auto ar=RunFfmpeg("-i \""+inputFile+"\" -af ebur128 -f null -");
        double lufs=0.0;
        if (ParseEbur128(ar.output,lufs)) {
            double gain=-8.0-lufs;
            std::ostringstream o; o<<std::fixed<<std::setprecision(2)<<gain;
            filters.push_back("volume="+o.str()+"dB");
            g_app.log.add("EBU R128 Measurement",             V_ACCENT_GOLD);
            g_app.log.add("  I:\t\t"+std::to_string((int)std::round(lufs))+" LUFS",V_TEXT_MAIN);
            g_app.log.add("  LUFS:\t\t-8",                    V_TEXT_MUTED);
        }
    }

    if (offset>0) {
        int ms=(int)(offset*1000);
        filters.insert(filters.begin(),
            "adelay="+std::to_string(ms)+"|"+std::to_string(ms));
    }
    if (offset<0) {
        char buf[64]; snprintf(buf,sizeof(buf),"-ss %.3f",std::abs(offset));
        out.seekArg=buf;
    }

    double trimSec=-1.0;
    if (g_app.calcMadi) {
        double songLen=GetDuration(inputFile);
        if (songLen<=0){g_app.log.add("Error: Could not read duration.",V_ERROR);return false;}
        double timeFull=time+offset;
        long long sm=(long long)std::ceil((bpm/60.0)*timeFull/4.0-1.0);
        long long tm=(long long)std::ceil((bpm*songLen)/(4.0*60.0));
        ApplyMadiParity(sm,tm,bpm,trimSec);

        g_app.setResults(bpm, sm, tm);

        g_app.log.add("");
        g_app.log.add("Time Signature",                        V_ACCENT_GOLD);
        g_app.log.add("  BPM:\t\t"+FmtBpm(bpm),               V_TEXT_MAIN);
        g_app.log.add("  StartMadi:\t"+std::to_string(sm),     V_SUCCESS);
        g_app.log.add("  TotalMadi:\t"+std::to_string(tm),     V_SUCCESS);
        g_app.log.add("");
        out.newFileName="S_"+origName+" "+FmtBpm(bpm)+" "
            +std::to_string(sm)+" "+std::to_string(tm)+ext;
    } else {
        out.newFileName="edited_"+origName+ext;
    }

    if (!filters.empty()) {
        std::string j; for(size_t i=0;i<filters.size();++i){if(i)j+=",";j+=filters[i];}
        out.filterArg="-af \""+j+"\"";
    }
    if (trimSec>0) {
        char buf[64]; snprintf(buf,sizeof(buf),"-t %.6f",trimSec);
        out.trimArg=buf;
    }
    return true;
}

static void DoConvertToWav(const std::string& f) {
    fs::path p(f);
    std::string out=(p.parent_path()/p.stem()).string()+".wav";
    LogConversionResult(RunFfmpeg("-i \""+f+"\" -c:a pcm_s16le -ar 44100 "
        "-rf64 never -map_metadata -1 -fflags +bitexact -y \""+out+"\""),out);
}

static void DoConvertWavToOgg(const std::string& f,double offset,double bpm,double time) {
    fs::path p(f); ConvParams cp;
    if (!BuildConvParams(f,offset,bpm,time,p.stem().string(),".ogg",cp)) return;
    std::string out=p.parent_path().string()+"\\"+cp.newFileName;
    LogConversionResult(RunFfmpeg(cp.seekArg+" -i \""+f+"\" "+cp.filterArg+" "+cp.trimArg
        +" -c:a libvorbis -b:a 96k -ar 44100 -map_metadata -1 -y \""+out+"\""),out);
}

static void DoConvertWavToWav(const std::string& f,double offset,double bpm,double time) {
    fs::path p(f); ConvParams cp;
    if (!BuildConvParams(f,offset,bpm,time,p.stem().string(),".wav",cp)) return;
    std::string out=p.parent_path().string()+"\\"+cp.newFileName;
    LogConversionResult(RunFfmpeg(cp.seekArg+" -i \""+f+"\" "+cp.filterArg+" "+cp.trimArg
        +" -c:a pcm_s16le -ar 44100 -rf64 never -map_metadata -1 -fflags +bitexact -y \""+out+"\""),out);
}

// ──────────────────────────────────────────────────────────────
//  Clipboard default offset (Sicher gegen Abstürze!)
// ──────────────────────────────────────────────────────────────
static std::string ClipboardDefaultOffset() {
    std::string def = "0.0";
    if (!OpenClipboard(nullptr)) return def;
    
    HANDLE h = GetClipboardData(CF_TEXT);
    if (h) { 
        char* t = (char*)GlobalLock(h);
        if (t) { 
            try {
                // Lese absolut sicher maximal 2048 Zeichen aus
                size_t len = 0;
                while (len < 2048 && t[len] != '\0') {
                    len++;
                }
                std::string clip(t, len);
                GlobalUnlock(h);
                
                // CRASH-FIX: C++ std::regex unterstützt keine Lookbehinds wie (?<!\d)
                // Dies hat den harten Absturz verursacht. Der neue Regex ist simpel und sicher:
                std::regex re(R"(-?\d+[.,]\d+)"); 
                std::smatch m;
                if (std::regex_search(clip, m, re)) {
                    def = m[0].str();
                    std::replace(def.begin(), def.end(), ',', '.');
                }
            } catch (...) {
                // Falls wider Erwarten irgendetwas schiefgeht, crasht das Programm nicht
            }
        }
    }
    CloseClipboard(); 
    return def;
}

// ──────────────────────────────────────────────────────────────
//  Processing orchestrator
// ──────────────────────────────────────────────────────────────
static void ProcessFile(std::string inputFile) {
    // Thread startet: Bereite UI vor
    g_app.log.clear();
    g_app.clearResults();
    g_app.setStatus("Processing...");

    LogFileDetails(inputFile);

    fs::path p(inputFile);
    std::string ext=p.extension().string();
    for (auto& c:ext) c=(char)tolower((unsigned char)c);

    if (ext==".flac"||ext==".ogg"||ext==".mp3") {
        DoConvertToWav(inputFile); 
        g_app.busy = false; 
        return;
    }
    if (ext!=".wav") {
        g_app.log.add("Error: Unsupported file type '"+ext+"'.",V_ERROR);
        g_app.setStatus("Invalid file."); 
        g_app.busy = false; 
        return;
    }

    double offset=0.0;
    if (!PromptValue("Enter Manual Offset (e.g. 1.234)",
                     ClipboardDefaultOffset(),DialogStep::WaitingOffset,offset))
    { g_app.setStatus("Cancelled."); g_app.busy=false; return; }

    double bpm=120.0, time=0.0;
    if (g_app.calcMadi) {
        g_app.setStatus("Detecting BPM...");
        double det=GetBpm(inputFile); if(det>0) bpm=det;

        if (!PromptValue("Enter BPM",FmtBpm(bpm),DialogStep::WaitingBpm,bpm))
        { g_app.setStatus("Cancelled."); g_app.busy=false; return; }

        if (!PromptValue("Enter Time (First Space)","0.0",DialogStep::WaitingTime,time))
        { g_app.setStatus("Cancelled."); g_app.busy=false; return; }
    }

    g_app.setStatus("Processing...");
    if (g_app.wavMode) DoConvertWavToWav(inputFile,offset,bpm,time);
    else               DoConvertWavToOgg(inputFile,offset,bpm,time);
    
    g_app.busy = false;
}

static std::string OpenFileDialog() {
    char buf[MAX_PATH]{}; OPENFILENAMEA ofn{};
    ofn.lStructSize=sizeof(ofn);
    ofn.lpstrFilter="Audio Files\0*.flac;*.wav;*.ogg;*.mp3\0"
                    "WAV Files\0*.wav\0OGG Files\0*.ogg\0"
                    "FLAC Files\0*.flac\0MP3 Files\0*.mp3\0\0";
    ofn.lpstrFile=buf; ofn.nMaxFile=MAX_PATH;
    ofn.Flags=OFN_PATHMUSTEXIST|OFN_FILEMUSTEXIST;
    ofn.lpstrTitle="Select Audio File";
    return GetOpenFileNameA(&ofn)?buf:"";
}

static void LaunchWorker(const std::string& file) {
    // ATOMARER SCHUTZ gegen Deadlocks beim schnellen Doppel-Droppen!
    if (g_app.busy.exchange(true)) return; 
    
    auto exists = [](const std::string& p) {
        return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES;
    };
    
    if (!exists(FfmpegPath()) || !exists(FfprobePath())) {
        g_app.log.clear();
        g_app.log.add("Error: ffmpeg.exe / ffprobe.exe not found!", V_ERROR);
        g_app.setStatus("Missing ffmpeg.");
        g_app.busy = false; // Reset bei Error
        return;
    }
    
    // Sicherstellen dass alter Thread aufgeräumt ist
    if (g_app.worker.joinable()) g_app.worker.join();
    g_app.worker = std::thread(ProcessFile, file);
}

// ──────────────────────────────────────────────────────────────
//  Custom drawing helpers
// ──────────────────────────────────────────────────────────────

static bool DrawToggle(const char* id, bool* v, float w=36.f, float h=20.f) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 pos     = ImGui::GetCursorScreenPos();
    float  r       = h * 0.5f;

    ImGui::InvisibleButton(id, {w, h});
    bool clicked = ImGui::IsItemClicked();
    if (clicked) *v = !*v;

    ImU32 trackCol = *v ? C_ACCENT_GOLD : C_BG_LIGHT;
    dl->AddRectFilled({pos.x, pos.y}, {pos.x+w, pos.y+h}, trackCol, r);

    float cx = *v ? pos.x+w-r : pos.x+r;
    dl->AddCircleFilled({cx, pos.y+r}, r-3.f, C_BG_DARK);

    return clicked;
}

static void DrawDashedRect(ImDrawList* dl, ImVec2 a, ImVec2 b,
                            ImU32 col, float radius=6.f,
                            float dash=8.f, float gap=5.f) {
    auto seg=[&](ImVec2 p1,ImVec2 p2){
        float dx=p2.x-p1.x, dy=p2.y-p1.y;
        float len=std::sqrt(dx*dx+dy*dy);
        dx/=len; dy/=len; float t=0;
        while (t<len) {
            float t2=std::min(t+dash,len);
            dl->AddLine({p1.x+dx*t,p1.y+dy*t},{p1.x+dx*t2,p1.y+dy*t2},col,1.5f);
            t+=dash+gap;
        }
    };
    float r=radius;
    seg({a.x+r,a.y},{b.x-r,a.y}); 
    seg({a.x+r,b.y},{b.x-r,b.y}); 
    seg({a.x,a.y+r},{a.x,b.y-r}); 
    seg({b.x,a.y+r},{b.x,b.y-r}); 
    auto arc=[&](ImVec2 c,float sa,float ea){
        int steps=6; float step=(ea-sa)/steps;
        for(int i=0;i<steps;i+=2){
            float a1=sa+i*step, a2=sa+(i+1)*step;
            dl->AddLine({c.x+r*std::cos(a1),c.y+r*std::sin(a1)},
                        {c.x+r*std::cos(a2),c.y+r*std::sin(a2)},col,1.5f);
        }
    };
    constexpr float PI=3.14159265f;
    arc({a.x+r,a.y+r}, PI,     PI*1.5f);
    arc({b.x-r,a.y+r}, PI*1.5f,PI*2.f);
    arc({b.x-r,b.y-r}, 0,      PI*0.5f);
    arc({a.x+r,b.y-r}, PI*0.5f,PI);
}

// Perfekt in einer horizontalen Linie darstellbar!
static void DrawMetricCard(const char* label, const std::string& value, ImU32 valCol, float width) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float h = 58.f; 
    
    // Zwingt ImGui, den Platz sauber im Raster zu reservieren
    ImGui::Dummy({width, h}); 
    
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, {pos.x+width, pos.y+h}, C_BG_LIGHT, 6.f);
    dl->AddRect(pos, {pos.x+width, pos.y+h}, C_BG_DARK, 6.f, 0, 1.f);
    
    ImFont* font = ImGui::GetFont();
    float fs = ImGui::GetFontSize();
    dl->AddText(font, fs * 0.8f, {pos.x+12.f, pos.y+8.f}, C_TEXT_MUTED, label);
    dl->AddText(font, fs * 1.35f, {pos.x+12.f, pos.y+26.f}, valCol, value.c_str());
}

static void SectionLabel(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, (ImVec4)ImColor(C_TEXT_MUTED));
    ImGui::SetWindowFontScale(0.8f);
    ImGui::TextUnformatted(text);
    ImGui::SetWindowFontScale(1.f);
    ImGui::PopStyleColor();
    ImGui::Spacing();
}

// ──────────────────────────────────────────────────────────────
//  UI rendering
// ──────────────────────────────────────────────────────────────
static void RenderUI() {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos({0,0});
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, (ImVec4)ImColor(C_BG_MAIN));
    ImGui::Begin("##root",nullptr,
        ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|
        ImGuiWindowFlags_NoMove   |ImGuiWindowFlags_NoBringToFrontOnFocus|
        ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor();

    float winW = io.DisplaySize.x - 32.f; 
    bool busy = g_app.busy.load();

    // ── Header ────────────────────────────────────────────
    ImGui::PushStyleColor(ImGuiCol_Text,(ImVec4)ImColor(C_ACCENT_GOLD));
    ImGui::SetWindowFontScale(1.25f);
    ImGui::Text("Silencer");
    ImGui::SetWindowFontScale(1.f);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text,(ImVec4)ImColor(C_TEXT_MUTED));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 3.f);
    ImGui::Text("by Sanya");
    ImGui::PopStyleColor();
    
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 2.f);
    ImGui::PushStyleColor(ImGuiCol_Separator,(ImVec4)ImColor(C_BG_LIGHT));
    ImGui::Separator();
    ImGui::PopStyleColor();
    ImGui::Spacing();

    // ── Settings section ──────────────────────────────────
    SectionLabel("SETTINGS");

    struct ToggleDef { const char* id; bool* val; const char* label; const char* desc; };
    ToggleDef toggles[] = {
        {"##lufs", &g_app.applyLufs, "LUFS Normalization",    "Target: -8 LUFS (EBU R128)"},
        {"##madi", &g_app.calcMadi,  "Calculate Madi Values", "Start / Total Madi + parity"},
        {"##wav",  &g_app.wavMode,   "WAV Output Mode",       "Output as pcm_s16le instead of OGG"},
    };

    if (busy) ImGui::BeginDisabled();
    for (auto& t : toggles) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored((ImVec4)ImColor(C_TEXT_MAIN), "%s", t.label);
        ImGui::SameLine(180.f);
        ImGui::TextColored((ImVec4)ImColor(C_TEXT_MUTED), "%s", t.desc);
        ImGui::SameLine(winW - 36.f);
        DrawToggle(t.id, t.val);
    }
    if (busy) ImGui::EndDisabled();

    ImGui::Spacing();

    // ── Drop zone ─────────────────────────────────────────
    SectionLabel("FILE");

    ImDrawList* dl       = ImGui::GetWindowDrawList();
    ImVec2      dzPos    = ImGui::GetCursorScreenPos();
    float       dzW      = winW;
    float       dzH      = 60.f; // Kompakter
    bool        dzHover  = false;

    ImGui::InvisibleButton("##dropzone", {dzW, dzH});
    dzHover = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked() && !busy) {
        std::string f = OpenFileDialog();
        if (!f.empty()) LaunchWorker(f);
    }

    ImU32 dzBg  = dzHover ? IM_COL32(212,175,55,15) : C_BG_DARK;
    ImU32 dzBdr = dzHover ? C_ACCENT_GOLD : C_BG_LIGHT;
    dl->AddRectFilled(dzPos, {dzPos.x+dzW, dzPos.y+dzH}, dzBg, 6.f);
    DrawDashedRect(dl, dzPos, {dzPos.x+dzW, dzPos.y+dzH}, dzBdr, 6.f);

    float cx = dzPos.x + dzW*0.5f;
    float cy = dzPos.y + dzH*0.5f;
    
    const char* main_txt = busy ? "Processing..." : "Drop audio file here (or click to browse)";
    ImVec2 ms = ImGui::CalcTextSize(main_txt);
    dl->AddText({cx-ms.x*0.5f, cy-ms.y*0.5f}, dzHover?C_TEXT_MAIN:C_TEXT_MUTED, main_txt);

    ImGui::Spacing();

    // ── Result cards ──────────────────────────────────────
    bool hasRes; double dBpm; long long dSm, dTm;
    { std::lock_guard l(g_app.resMtx);
      hasRes=g_app.hasResults; dBpm=g_app.displayBpm;
      dSm=g_app.displayStartMadi; dTm=g_app.displayTotalMadi; }

    ImGui::Spacing();
    SectionLabel("RESULT");
    // Breite so berechnen, dass 3 Karten perfekt nebeneinander passen. 
    // Vermeidung von negativen Werten (Crashschutz).
    float cw = std::max(10.f, (winW - 16.f) / 3.f); 
    
    DrawMetricCard("BPM",        hasRes ? FmtBpm(dBpm) : "--",        C_ACCENT_GOLD, cw);
    ImGui::SameLine(0.f, 8.f);
    DrawMetricCard("START MADI", hasRes ? std::to_string(dSm) : "--", C_ACCENT_GOLD, cw);
    ImGui::SameLine(0.f, 8.f);
    DrawMetricCard("TOTAL MADI", hasRes ? std::to_string(dTm) : "--", C_SUCCESS,     cw);
    
    ImGui::Spacing();
    ImGui::Spacing();

    // ── Log panel ─────────────────────────────────────────
    SectionLabel("LOG");

    // Sicherer, großer Platz für die Logs
    float logH = std::max(30.f, ImGui::GetContentRegionAvail().y - 28.f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg,(ImVec4)ImColor(C_BG_DARK));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{12.f,10.f});
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding,6.f);
    if (ImGui::BeginChild("##log",{-1.f,logH},true,
                          ImGuiWindowFlags_HorizontalScrollbar)) {
        g_app.log.draw();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    ImGui::Spacing();

    // ── Status bar ────────────────────────────────────────
    std::string status = g_app.getStatus();
    ImVec4 sc = busy             ? V_ACCENT_GOLD
              : status=="Done."  ? V_SUCCESS
              : status=="Error." ? V_ERROR
                                 : V_TEXT_MUTED;

    ImVec2 dotPos = ImGui::GetCursorScreenPos();
    dotPos.x += 2.f; dotPos.y += 6.f;
    ImU32 dotCol;
    if      (busy)             { float t=(float)ImGui::GetTime();
                                 float a=0.4f+0.6f*(std::sin(t*4.f)*0.5f+0.5f);
                                 dotCol=IM_COL32(212,175,55,(int)(a*255)); }
    else if (status=="Done.")  dotCol=C_SUCCESS;
    else if (status=="Error.") dotCol=C_ERROR;
    else                       dotCol=C_TEXT_MUTED;
    dl->AddCircleFilled(dotPos,4.f,dotCol);

    ImGui::SetCursorPosX(ImGui::GetCursorPosX()+14.f);
    if (busy) {
        int dots=(int)(ImGui::GetTime()*2)%4;
        status+= std::string(dots,'.');
    }
    ImGui::TextColored(sc,"%s",status.c_str());

    ImGui::End();

    // ── Input dialog (Overlay) ───────────────────────────────────
    if (g_app.dialogOpen.load(std::memory_order_acquire))
    {
        ImDrawList* bg = ImGui::GetBackgroundDrawList();
        bg->AddRectFilled({0,0}, io.DisplaySize, IM_COL32(0,0,0,160));

        ImGui::SetNextWindowSize({380.f, 138.f}, ImGuiCond_Always);
        ImGui::SetNextWindowPos(
            {io.DisplaySize.x * .5f, io.DisplaySize.y * .5f},
            ImGuiCond_Always, {.5f, .5f});
        ImGui::SetNextWindowFocus(); 

        ImGui::PushStyleColor(ImGuiCol_WindowBg, (ImVec4)ImColor(C_BG_DARK));
        ImGui::PushStyleColor(ImGuiCol_Border,   (ImVec4)ImColor(C_BG_LIGHT));
        ImGui::PushStyleVar (ImGuiStyleVar_WindowRounding, 8.f);
        ImGui::PushStyleVar (ImGuiStyleVar_WindowPadding,  {20.f, 16.f});

        ImGui::Begin("##inputdlg", nullptr,
            ImGuiWindowFlags_NoResize   | ImGuiWindowFlags_NoMove    |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoScrollbar);

        ImGui::PushStyleColor(ImGuiCol_Text, (ImVec4)ImColor(C_ACCENT_GOLD));
        ImGui::SetWindowFontScale(1.05f);
        ImGui::TextUnformatted(g_app.dialogQuestion.c_str());
        ImGui::SetWindowFontScale(1.f);
        ImGui::PopStyleColor();
        ImGui::Spacing();

        static bool needsFocus = false;
        static bool lastOpen   = false;
        bool        curOpen2   = true;
        if (!lastOpen && curOpen2) needsFocus = true;
        lastOpen = curOpen2;

        ImGui::PushStyleColor(ImGuiCol_FrameBg, (ImVec4)ImColor(C_BG_LIGHT));
        ImGui::PushStyleColor(ImGuiCol_Text,    (ImVec4)ImColor(C_TEXT_MAIN));
        ImGui::SetNextItemWidth(-1.f);
        if (needsFocus) { ImGui::SetKeyboardFocusHere(); needsFocus = false; }
        bool enter = ImGui::InputText("##dlginput", g_app.dialogBuf,
                                      sizeof(g_app.dialogBuf),
                                      ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::PopStyleColor(2);
        ImGui::Spacing();

        auto commit = [&]() {
            double v = 0.0;
            try { v = std::stod(g_app.dialogBuf); } catch (...) {}
            switch (g_app.dialogStep) {
                case DialogStep::WaitingOffset: g_app.resultOffset = v; break;
                case DialogStep::WaitingBpm:    g_app.resultBpm    = v; break;
                case DialogStep::WaitingTime:   g_app.resultTime   = v; break;
                default: break;
            }
            lastOpen = false;
            g_app.dialogConfirmed.store(true);
            g_app.dialogOpen.store(false, std::memory_order_release);
        };

        float btnW = (ImGui::GetContentRegionAvail().x - 8.f) * .5f;

        ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(80,140,80,255));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  IM_COL32(100,160,100,255));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,   IM_COL32(120,180,120,255));
        if (ImGui::Button("OK", {btnW, 30.f}) || enter) commit();
        ImGui::PopStyleColor(3);

        ImGui::SameLine(0.f, 8.f);

        ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(140,60,60,255));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  IM_COL32(160,80,80,255));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,   IM_COL32(180,100,100,255));
        if (ImGui::Button("Cancel", {btnW, 30.f})) {
            lastOpen = false;
            g_app.dialogCancelled.store(true);
            g_app.dialogOpen.store(false, std::memory_order_release);
        }
        ImGui::PopStyleColor(3);

        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
    }
}

// ──────────────────────────────────────────────────────────────
//  DirectX 11 device
// ──────────────────────────────────────────────────────────────
static ID3D11Device*           g_pDevice   = nullptr;
static ID3D11DeviceContext*    g_pContext  = nullptr;
static IDXGISwapChain*         g_pSwap     = nullptr;
static ID3D11RenderTargetView* g_pRTV      = nullptr;

static void CreateRTV() {
    ID3D11Texture2D* bb=nullptr;
    g_pSwap->GetBuffer(0,IID_PPV_ARGS(&bb));
    g_pDevice->CreateRenderTargetView(bb,nullptr,&g_pRTV);
    bb->Release();
}

static bool InitD3D(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount=2; sd.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate={60,1};
    sd.Flags=DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow=hwnd; sd.SampleDesc.Count=1;
    sd.Windowed=TRUE; sd.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL fl;
    const D3D_FEATURE_LEVEL fla[]={D3D_FEATURE_LEVEL_11_0,D3D_FEATURE_LEVEL_10_0};
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,
        nullptr,0,fla,2,D3D11_SDK_VERSION,&sd,&g_pSwap,&g_pDevice,&fl,&g_pContext)))
        return false;
    CreateRTV(); return true;
}

static void CleanupD3D() {
    if (g_pRTV)    {g_pRTV->Release();   g_pRTV   =nullptr;}
    if (g_pSwap)   {g_pSwap->Release();  g_pSwap  =nullptr;}
    if (g_pContext){g_pContext->Release();g_pContext=nullptr;}
    if (g_pDevice) {g_pDevice->Release();g_pDevice =nullptr;}
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND,UINT,WPARAM,LPARAM);

static LRESULT WINAPI WndProc(HWND hWnd,UINT msg,WPARAM wParam,LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd,msg,wParam,lParam)) return 1;
    switch(msg){
    case WM_DROPFILES:{
        HDROP hd=(HDROP)wParam; char buf[MAX_PATH]{};
        // Verhindert Starten während schon was läuft:
        if (DragQueryFileA(hd,0,buf,MAX_PATH) && !g_app.busy.load()) {
            LaunchWorker(buf);
        }
        DragFinish(hd); return 0;
    }
    case WM_SYSCOMMAND: if((wParam&0xfff0)==SC_KEYMENU) return 0; break;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hWnd,msg,wParam,lParam);
}

int WINAPI WinMain(HINSTANCE hInst,HINSTANCE,LPSTR,int) {
    WNDCLASSEXW wc{sizeof(wc),CS_CLASSDC,WndProc,0,0,hInst,
                   LoadIconW(hInst,MAKEINTRESOURCEW(101)),
                   nullptr,nullptr,nullptr,L"SilencerImGui",
                   LoadIconW(hInst,MAKEINTRESOURCEW(101))};
    RegisterClassExW(&wc);
    
    // Festes Fenster 700x550, NICHT in der Größe veränderbar
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rect = { 0, 0, 700, 650 };
    AdjustWindowRect(&rect, style, FALSE);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"Silencer - v1.5", style, 
        CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, hInst, nullptr);
        
    DragAcceptFiles(hwnd,TRUE);
    if (!InitD3D(hwnd)){CleanupD3D();return 1;}
    ShowWindow(hwnd,SW_SHOWDEFAULT); UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io=ImGui::GetIO();
    io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename=nullptr;

    // --- SYSTEM SCHRIFTART LADEN (Segoe UI) ---
    ImFontConfig fontCfg;
    fontCfg.OversampleH = 2;
    fontCfg.OversampleV = 2;
    if (fs::exists("C:\\Windows\\Fonts\\segoeui.ttf")) {
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 17.f, &fontCfg);
    } else {
        io.Fonts->AddFontDefault();
    }

    ImGuiStyle& s=ImGui::GetStyle();
    s.WindowRounding=0.f; s.ChildRounding=6.f;
    s.FrameRounding=6.f;  s.PopupRounding=8.f;
    s.GrabRounding=4.f;   s.FramePadding={8.f,5.f};
    s.ItemSpacing={8.f,6.f}; s.WindowPadding={16.f,12.f};
    s.ScrollbarSize=8.f;

    ImVec4* c=s.Colors;
    c[ImGuiCol_WindowBg]             = {18.f/255.f, 10.f/255.f, 40.f/255.f, 1.f};
    c[ImGuiCol_ChildBg]              = {10.f/255.f, 5.f/255.f,  22.f/255.f, 1.f};
    c[ImGuiCol_PopupBg]              = {10.f/255.f, 5.f/255.f,  22.f/255.f, 1.f};
    c[ImGuiCol_FrameBg]              = {31.f/255.f, 21.f/255.f, 61.f/255.f, 1.f};
    c[ImGuiCol_FrameBgHovered]       = {46.f/255.f, 31.f/255.f, 89.f/255.f, 1.f};
    c[ImGuiCol_FrameBgActive]        = {60.f/255.f, 40.f/255.f, 110.f/255.f,1.f};
    c[ImGuiCol_Border]               = {31.f/255.f, 21.f/255.f, 61.f/255.f, 1.f};
    c[ImGuiCol_TitleBgActive]        = {18.f/255.f, 10.f/255.f, 40.f/255.f, 1.f};
    c[ImGuiCol_CheckMark]            = V_ACCENT_GOLD;
    c[ImGuiCol_SliderGrab]           = V_ACCENT_GOLD;
    c[ImGuiCol_Button]               = {31.f/255.f, 21.f/255.f, 61.f/255.f, 1.f};
    c[ImGuiCol_ButtonHovered]        = {46.f/255.f, 31.f/255.f, 89.f/255.f, 1.f};
    c[ImGuiCol_ButtonActive]         = V_ACCENT_GOLD;
    c[ImGuiCol_Header]               = {31.f/255.f, 21.f/255.f, 61.f/255.f, 1.f};
    c[ImGuiCol_HeaderHovered]        = {46.f/255.f, 31.f/255.f, 89.f/255.f, 1.f};
    c[ImGuiCol_Separator]            = {31.f/255.f, 21.f/255.f, 61.f/255.f, 1.f};
    c[ImGuiCol_ScrollbarBg]          = {10.f/255.f, 5.f/255.f,  22.f/255.f, 1.f};
    c[ImGuiCol_ScrollbarGrab]        = {31.f/255.f, 21.f/255.f, 61.f/255.f, 1.f};
    c[ImGuiCol_ScrollbarGrabHovered] = {46.f/255.f, 31.f/255.f, 89.f/255.f, 1.f};
    c[ImGuiCol_Text]                 = V_TEXT_MAIN;
    c[ImGuiCol_TextDisabled]         = V_TEXT_MUTED;
    c[ImGuiCol_ModalWindowDimBg]     = {0.f,0.f,0.f,0.6f};
    c[ImGuiCol_NavHighlight]         = V_ACCENT_GOLD;

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pDevice,g_pContext);

    constexpr float clear[4]={18.f/255.f, 10.f/255.f, 40.f/255.f, 1.f};
    MSG msg{};
    while(true){
        while(PeekMessage(&msg,nullptr,0,0,PM_REMOVE)){
            TranslateMessage(&msg); DispatchMessage(&msg);
            if(msg.message==WM_QUIT) goto done;
        }
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        RenderUI();
        ImGui::Render();
        g_pContext->OMSetRenderTargets(1,&g_pRTV,nullptr);
        g_pContext->ClearRenderTargetView(g_pRTV,clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwap->Present(1,0);
    }
done:
    if (g_app.worker.joinable()){
        g_app.dialogCancelled.store(true);
        g_app.dialogOpen.store(false,std::memory_order_release);
        g_app.worker.join();
    }
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName,hInst);
    return 0;
}