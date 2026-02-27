
#include <windows.h>
#include <psapi.h>
#include <sddl.h>
#include <algorithm>
#include <vector>
#include <string>

static const unsigned char SIG[] = {
    0xFF, 0x15, 0,0,0,0,    // call [rip+?]
    0x48, 0x83, 0xF8, 0x01, // cmp  rax,1
    0x75, 0x02,             // jne  +2
    0xFF, 0x03              // inc  [rbx]
};
static const char MASK[] = "xx????xxxxxx";   // 只校验12字节 patch偏移12

static const char GAME_ARGS[] = "--start_from_launcher=1 --is_multi_start"; // 启动参数

static inline bool sig_match(const BYTE* p) {
    // 快速匹配
    if (p[0] != 0xFF || p[1] != 0x15) return false;
    for (int i = 6; MASK[i]; i++)
        if (MASK[i] == 'x' && p[i] != SIG[i]) return false;
    return true;
}

// 取当前用户sid
static std::string GetCurrentUserSid() {
    HANDLE hTok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hTok))
        return {};

    DWORD needed = 0;
    GetTokenInformation(hTok, TokenUser, nullptr, 0, &needed);
    std::vector<BYTE> buf(needed);
    if (!GetTokenInformation(hTok, TokenUser, buf.data(), needed, &needed)) {
        CloseHandle(hTok);
        return {};
    }
    CloseHandle(hTok);

    PSID sid = reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid;
    LPSTR sidStr = nullptr;
    if (!ConvertSidToStringSidA(sid, &sidStr))
        return {};

    std::string result(sidStr);
    LocalFree(sidStr);
    return result;
}

// 从注册表取安装路径
static std::string GetInstallPathFromRegistry() {
    std::string sid = GetCurrentUserSid();
    if (sid.empty()) return {};

    std::string subKey = sid + "\\Software\\FeverGames\\FeverGamesInstaller\\game\\73";

    HKEY hKey;
    if (RegOpenKeyExA(HKEY_USERS, subKey.c_str(), 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS)
        return {};

    char path[MAX_PATH] = {};
    DWORD sz = sizeof(path);
    RegQueryValueExA(hKey, "InstallPath", nullptr, nullptr, (LPBYTE)path, &sz);
    RegCloseKey(hKey);

    std::string s(path);
    while (!s.empty() && (s.back() == '\\' || s.back() == '/')) s.pop_back();
    return s;
}

// 尝试从idv-login的config.json取exe路径
static std::string GetExePathFromConfig() {
    char winDir[MAX_PATH];
    GetWindowsDirectoryA(winDir, sizeof(winDir));
    char drive[3] = { winDir[0], ':', '\0' };

    std::string cfgPath = std::string(drive) + "\\ProgramData\\idv-login\\config.json";

    HANDLE hFile = CreateFileA(cfgPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, 0, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return {};

    DWORD fileSz = GetFileSize(hFile, nullptr);
    if (fileSz == 0 || fileSz > 1 * 1024 * 1024) { CloseHandle(hFile); return {}; }

    std::string json(fileSz, '\0');
    DWORD got = 0;
    ReadFile(hFile, &json[0], fileSz, &got, nullptr);
    CloseHandle(hFile);
    if (got != fileSz) return {};

    const char* key = "\"aecfrt3rmaaaaajl-g-h55\"";
    size_t kp = json.find(key);
    if (kp == std::string::npos) return {};

    const char* pathKey = "\"path\"";
    size_t pp = json.find(pathKey, kp);
    if (pp == std::string::npos) return {};

    size_t colon = json.find(':', pp + 6);
    if (colon == std::string::npos) return {};
    size_t q1 = json.find('"', colon + 1);
    if (q1 == std::string::npos) return {};
    size_t q2 = json.find('"', q1 + 1);
    if (q2 == std::string::npos) return {};

    std::string val;
    val.reserve(q2 - q1);
    for (size_t i = q1 + 1; i < q2; i++) {
        if (json[i] == '\\' && i + 1 < q2) { val += json[++i]; }
        else val += json[i];
    }
    return val;
}

static std::string GetExePath() {
    // 1. 优先尝试注册表获取安装目录
    std::string dir = GetInstallPathFromRegistry();
    if (!dir.empty()) {
        std::string p = dir + "\\dwrg.exe";
        if (GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
    }

    // 2. 尝试从 config.json 获取路径
    std::string path = GetExePathFromConfig();
    if (!path.empty() && GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) return path;

    // 3. 最后尝试从当前运行目录查找 dwrg.exe
    char buf[MAX_PATH];
    if (GetModuleFileNameA(NULL, buf, MAX_PATH)) {
        std::string s = buf;
        size_t last = s.find_last_of("\\/");
        if (last != std::string::npos) {
            std::string local = s.substr(0, last + 1) + "dwrg.exe";
            if (GetFileAttributesA(local.c_str()) != INVALID_FILE_ATTRIBUTES) return local;
        }
    }

    return {};
}

static BYTE* WaitForRemoteModule(HANDLE hProc, const char* name, DWORD timeoutMs) {
    DWORD deadline = GetTickCount() + timeoutMs;
    DWORD interval = 50;   // 渐增间隔
    do {
        HMODULE mods[512];
        DWORD needed = 0;
        if (EnumProcessModules(hProc, mods, sizeof(mods), &needed)) {
            DWORD n = needed / sizeof(HMODULE);
            char buf[64];
            for (DWORD i = 0; i < n; i++) {
                if (GetModuleBaseNameA(hProc, mods[i], buf, sizeof(buf)) &&
                    _stricmp(buf, name) == 0)
                    return (BYTE*)mods[i];
            }
        }
        Sleep(interval);
        if (interval < 200) interval += 30;
    } while (GetTickCount() < deadline);
    return nullptr;
}

static void ScanAndPatch(HANDLE hProc, BYTE* base) {

    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS  nt;
    if (!ReadProcessMemory(hProc, base, &dos, sizeof(dos), nullptr)) return;
    if (!ReadProcessMemory(hProc, base + dos.e_lfanew, &nt, sizeof(nt), nullptr)) return;

    DWORD secOff = dos.e_lfanew + sizeof(IMAGE_NT_HEADERS);
    int   numSec = nt.FileHeader.NumberOfSections;
    std::vector<IMAGE_SECTION_HEADER> secs(numSec);
    if (!ReadProcessMemory(hProc, base + secOff, secs.data(),
                           numSec * sizeof(IMAGE_SECTION_HEADER), nullptr)) return;

    for (auto& s : secs) {
        if (memcmp(s.Name, ".text", 5) != 0) continue;

        BYTE*       start   = base + s.VirtualAddress;
        DWORD       total   = s.Misc.VirtualSize;
        const DWORD CHUNK   = 0x40000;
        const DWORD OVERLAP = (DWORD)sizeof(SIG) - 1;// 跨块边界保护

        std::vector<BYTE> buf;
        buf.reserve(CHUNK + OVERLAP);

        for (DWORD off = 0; off < total; ) {
            DWORD readSz = std::min(CHUNK + OVERLAP, total - off);
            buf.resize(readSz);
            SIZE_T got = 0;
            if (!ReadProcessMemory(hProc, start + off, buf.data(), readSz, &got) ||
                got < sizeof(SIG)) {
                off += CHUNK;
                continue;
            }
            DWORD scanEnd = (DWORD)got - (DWORD)sizeof(SIG) + 1;
            static const BYTE nop2[2] = { 0x90, 0x90 };
            for (DWORD j = 0; j < scanEnd; j++) {
                if (sig_match(buf.data() + j)) {
                    BYTE* patchAddr = start + off + j + 12;
                    DWORD old = 0;
                    VirtualProtectEx(hProc, patchAddr, 2, PAGE_EXECUTE_READWRITE, &old);
                    WriteProcessMemory(hProc, patchAddr, nop2, 2, nullptr);
                    VirtualProtectEx(hProc, patchAddr, 2, old, &old);
                    return;
                }
            }
            off += CHUNK;
        }
        break;
    }
}

static void LaunchAndPatch(const std::string& exePath) {
    std::string dir = exePath.substr(0, exePath.find_last_of("\\/"));
    std::string cmd = '"' + exePath + "\" " + GAME_ARGS;

    STARTUPINFOA        si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};

    if (!CreateProcessA(exePath.c_str(), cmd.data(),
                        nullptr, nullptr, FALSE, 0,
                        nullptr, dir.c_str(), &si, &pi))
        return;

    BYTE* base = WaitForRemoteModule(pi.hProcess, "neox_engine.dll", 5000); // 等待neox_engine.dll加载
    if (base)
        ScanAndPatch(pi.hProcess, base);

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR lpCmdLine, int) {
    int count = 1; // 默认启动1个实例
    if (lpCmdLine && *lpCmdLine) {
        int n = atoi(lpCmdLine);
        if (n > 0) count = n;
    }

    std::string exePath = GetExePath();
    if (exePath.empty()) {
        MessageBoxA(nullptr, "未找到游戏安装路径，请确认游戏已正确安装。",
                    "Launcher 错误", MB_ICONERROR | MB_OK);
        return 1;
    }

    for (int i = 0; i < count; i++) {
        if (i > 0) Sleep(600); // 多开时错开 避免资源竞争
        LaunchAndPatch(exePath);
    }
    return 0;
}
