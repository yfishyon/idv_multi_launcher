#include <windows.h>
#include <psapi.h>
#include <sddl.h>
#include <commctrl.h>
#include <algorithm>
#include <vector>
#include <string>

#include "resource.h"

#pragma comment(lib, "comctl32.lib")

static const unsigned char SIG[] = {
    0xFF, 0x15, 0,0,0,0,    // call [rip+?]
    0x48, 0x83, 0xF8, 0x01, // cmp  rax,1
    0x75, 0x02,             // jne  +2
    0xFF, 0x03              // inc  [rbx]
};
static const char MASK[] = "xx????xxxxxx";   // 只校验12字节 patch偏移12

static const wchar_t GAME_ARGS[] = L"--start_from_launcher=1 --is_multi_start";   // 启动参数

// 全局变量
static HINSTANCE g_hInst;
static HWND g_hWnd;
static HWND g_hCountEdit;
static HWND g_hCountSlider;
static HWND g_hPathEdit;
static HWND g_hStatusText;
static int g_InstanceCount = 1;
static wchar_t g_ExePath[MAX_PATH] = L"";
static wchar_t g_StatusMsg[256] = L"已就绪";
static bool g_AutoDetecting = false;

// 函数声明
static INT_PTR CALLBACK DlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static void LoadConfig();
static void SaveConfig();
static void UpdateStatus(HWND hwnd, const wchar_t* msg);
static void UpdateCountDisplay(HWND hwnd);

static inline bool sig_match(const BYTE* p) {
    // 快速匹配
    if (p[0] != 0xFF || p[1] != 0x15) return false;
    for (int i = 6; MASK[i]; i++)
        if (MASK[i] == 'x' && p[i] != SIG[i]) return false;
    return true;
}

// 取当前用户sid
static std::wstring GetCurrentUserSid() {
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
    LPWSTR sidStr = nullptr;
    if (!ConvertSidToStringSidW(sid, &sidStr))
        return {};

    std::wstring result(sidStr);
    LocalFree(sidStr);
    return result;
}

// 从注册表取安装路径
static std::wstring GetInstallPathFromRegistry() {
    std::wstring sid = GetCurrentUserSid();
    if (sid.empty()) return {};

    std::wstring subKey = sid + L"\\Software\\FeverGames\\FeverGamesInstaller\\game\\73";

    HKEY hKey;
    if (RegOpenKeyExW(HKEY_USERS, subKey.c_str(), 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS)
        return {};

    wchar_t path[MAX_PATH] = {};
    DWORD sz = sizeof(path);
    RegQueryValueExW(hKey, L"InstallPath", nullptr, nullptr, (LPBYTE)path, &sz);
    RegCloseKey(hKey);

    std::wstring s(path);
    while (!s.empty() && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    return s;
}

// 尝试从idv-login的config.json取exe路径
static std::wstring GetExePathFromConfig() {
    wchar_t winDir[MAX_PATH];
    GetWindowsDirectoryW(winDir, sizeof(winDir)/sizeof(wchar_t));
    wchar_t drive[3] = { winDir[0], L':', L'\0' };

    std::wstring cfgPath = std::wstring(drive) + L"\\ProgramData\\idv-login\\config.json";

    HANDLE hFile = CreateFileW(cfgPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
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
    
    int len = MultiByteToWideChar(CP_UTF8, 0, val.c_str(), -1, nullptr, 0);
    std::wstring wval(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, val.c_str(), -1, &wval[0], len);
    wval.pop_back();
    
    return wval;
}

static bool IsValidExePath(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

static std::wstring AutoDetectExePath() {
    // 1. 优先尝试注册表获取安装目录
    std::wstring dir = GetInstallPathFromRegistry();
    if (!dir.empty()) {
        std::wstring p = dir + L"\\dwrg.exe";
        if (IsValidExePath(p)) return p;
    }

    // 2. 尝试从 config.json 获取路径
    std::wstring path = GetExePathFromConfig();
    if (!path.empty() && IsValidExePath(path)) return path;

    // 3. 最后尝试从当前运行目录查找 dwrg.exe
    wchar_t buf[MAX_PATH];
    if (GetModuleFileNameW(NULL, buf, MAX_PATH)) {
        std::wstring s = buf;
        size_t last = s.find_last_of(L"\\/");
        if (last != std::wstring::npos) {
            std::wstring local = s.substr(0, last + 1) + L"dwrg.exe";
            if (IsValidExePath(local)) return local;
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
        const DWORD OVERLAP = (DWORD)sizeof(SIG) - 1;   // 跨块边界保护

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

static bool LaunchAndPatch(const std::wstring& exePath, wchar_t* status) {
    std::wstring dir = exePath.substr(0, exePath.find_last_of(L"\\/"));
    std::wstring cmd = L'"' + exePath + L"\" " + GAME_ARGS;

    STARTUPINFOW        si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};

    if (!CreateProcessW(exePath.c_str(), (LPWSTR)cmd.data(),
                        nullptr, nullptr, FALSE, 0,
                        nullptr, dir.c_str(), &si, &pi)) {
        if (status) swprintf(status, 64, L"启动失败: %d", GetLastError());
        return false;
    }

    if (status) wcscpy_s(status, 64, L"正在注入多开补丁...");
    
    BYTE* base = WaitForRemoteModule(pi.hProcess, "neox_engine.dll", 5000);
    if (base) {
        ScanAndPatch(pi.hProcess, base);
        if (status) wcscpy_s(status, 64, L"游戏已启动");
    } else {
        if (status) wcscpy_s(status, 64, L"游戏已启动 但补丁可能失败");
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

static void LoadConfig() {
    wchar_t path[MAX_PATH] = L"";
    DWORD sz = sizeof(path);
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\DWRGLauncher", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        RegQueryValueExW(hKey, L"ExePath", nullptr, nullptr, (LPBYTE)path, &sz);
        
        DWORD count = g_InstanceCount;
        sz = sizeof(DWORD);
        RegQueryValueExW(hKey, L"InstanceCount", nullptr, nullptr, (LPBYTE)&count, &sz);
        if (count > 0 && count <= 10) g_InstanceCount = count;
        
        RegCloseKey(hKey);
    }
    
    if (path[0]) {
        wcscpy_s(g_ExePath, path);
    }
}

static void SaveConfig() {
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\DWRGLauncher", 0, nullptr, 
                        REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, L"ExePath", 0, REG_SZ, (LPBYTE)g_ExePath, (DWORD)(wcslen(g_ExePath) + 1) * sizeof(wchar_t));
        RegSetValueExW(hKey, L"InstanceCount", 0, REG_DWORD, (LPBYTE)&g_InstanceCount, sizeof(g_InstanceCount));
        RegCloseKey(hKey);
    }
}

static void UpdateStatus(HWND hwnd, const wchar_t* msg) {
    wcscpy_s(g_StatusMsg, msg);
    SetDlgItemTextW(hwnd, IDC_STATUS_TEXT, msg);
}

static void UpdateCountDisplay(HWND hwnd) {
    wchar_t buf[32];
    swprintf(buf, 32, L"%d", g_InstanceCount);
    SetDlgItemTextW(hwnd, IDC_COUNT_EDIT, buf);
}

static void BrowseForExe(HWND hwnd) {
    OPENFILENAMEW ofn = {};
    wchar_t file[MAX_PATH] = L"";
    
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"第五人格游戏文件\0dwrg.exe\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"选择 dwrg.exe";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    
    if (GetOpenFileNameW(&ofn)) {
        wcscpy_s(g_ExePath, file);
        SetDlgItemTextW(hwnd, IDC_PATH_EDIT, file);
        SaveConfig();
        UpdateStatus(hwnd, L"路径已更新");
    }
}

static void AutoDetect(HWND hwnd) {
    if (g_AutoDetecting) return;
    g_AutoDetecting = true;
    
    UpdateStatus(hwnd, L"正在自动检测路径...");
    std::wstring path = AutoDetectExePath();
    
    if (!path.empty()) {
        wcscpy_s(g_ExePath, path.c_str());
        SetDlgItemTextW(hwnd, IDC_PATH_EDIT, path.c_str());
        SaveConfig();
        
        wchar_t msg[256];
        swprintf(msg, 256, L"自动检测到: %s", path.c_str());
        UpdateStatus(hwnd, msg);
    } else {
        UpdateStatus(hwnd, L"自动检测路径失败，请手动选择");
        BrowseForExe(hwnd);
    }
    
    g_AutoDetecting = false;
}

static void StartGames(HWND hwnd) {
    GetDlgItemTextW(hwnd, IDC_PATH_EDIT, g_ExePath, MAX_PATH);
    
    if (wcslen(g_ExePath) == 0 || !IsValidExePath(g_ExePath)) {
        UpdateStatus(hwnd, L"请选择有效的游戏路径");
        MessageBoxW(hwnd, L"请先选择正确的游戏路径", L"错误", MB_ICONERROR | MB_OK);
        return;
    }
    
    wchar_t countStr[16];
    GetDlgItemTextW(hwnd, IDC_COUNT_EDIT, countStr, sizeof(countStr)/sizeof(wchar_t));
    g_InstanceCount = _wtoi(countStr);
    if (g_InstanceCount < 1) g_InstanceCount = 1;
    if (g_InstanceCount > 10) g_InstanceCount = 10;
    
    SaveConfig();
    
    EnableWindow(GetDlgItem(hwnd, IDC_START_BTN), FALSE);
    EnableWindow(GetDlgItem(hwnd, IDC_BROWSE_BTN), FALSE);
    EnableWindow(GetDlgItem(hwnd, IDC_AUTO_DETECT_BTN), FALSE);
    EnableWindow(g_hCountSlider, FALSE);
    EnableWindow(g_hCountEdit, FALSE);
    
    wchar_t statusMsg[256];
    swprintf(statusMsg, 256, L"正在启动 %d 个游戏实例...", g_InstanceCount);
    UpdateStatus(hwnd, statusMsg);
    
    int successCount = 0;
    for (int i = 0; i < g_InstanceCount; i++) {
        if (i > 0) Sleep(600);
        
        wchar_t stepStatus[64];
        if (LaunchAndPatch(std::wstring(g_ExePath), stepStatus)) {
            successCount++;
        }
        
        swprintf(statusMsg, 256, L"已启动 %d/%d 个游戏实例", i + 1, g_InstanceCount);
        UpdateStatus(hwnd, statusMsg);
        
        MSG msg;
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
    
    if (successCount == 0) {
        UpdateStatus(hwnd, L"启动失败，请检查路径");
        EnableWindow(GetDlgItem(hwnd, IDC_START_BTN), TRUE);
        EnableWindow(GetDlgItem(hwnd, IDC_BROWSE_BTN), TRUE);
        EnableWindow(GetDlgItem(hwnd, IDC_AUTO_DETECT_BTN), TRUE);
        EnableWindow(g_hCountSlider, TRUE);
        EnableWindow(g_hCountEdit, TRUE);
    } else {
        // 自杀
        UpdateStatus(hwnd, L"启动完成，程序即将10秒后退出");
        Sleep(1000);
        EndDialog(hwnd, 0);
    }
}

static INT_PTR CALLBACK DlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INITDIALOG: {
            g_hWnd = hwnd;
            
            g_hCountEdit = GetDlgItem(hwnd, IDC_COUNT_EDIT);
            g_hCountSlider = GetDlgItem(hwnd, IDC_COUNT_SLIDER);
            g_hPathEdit = GetDlgItem(hwnd, IDC_PATH_EDIT);
            g_hStatusText = GetDlgItem(hwnd, IDC_STATUS_TEXT);
            
            SendMessage(g_hCountSlider, TBM_SETRANGE, TRUE, MAKELPARAM(1, 10));
            SendMessage(g_hCountSlider, TBM_SETPAGESIZE, 0, 1);
            SendMessage(g_hCountSlider, TBM_SETTICFREQ, 1, 0);
            
            LoadConfig();
            
            SendMessage(g_hCountSlider, TBM_SETPOS, TRUE, g_InstanceCount);
            UpdateCountDisplay(hwnd);
            SetDlgItemTextW(hwnd, IDC_PATH_EDIT, g_ExePath);
            
            SetDlgItemTextW(hwnd, IDC_STATUS_TEXT, L"已就绪");
            
            HICON hIcon = LoadIcon(g_hInst, MAKEINTRESOURCE(1));
            if (hIcon) {
                SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
                SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
            }
            
            // 自动检测路径
            if (wcslen(g_ExePath) == 0) {
                AutoDetect(hwnd);
            }
            
            return TRUE;
        }
        
        case WM_HSCROLL: {
            if ((HWND)lParam == g_hCountSlider) {
                g_InstanceCount = SendMessage(g_hCountSlider, TBM_GETPOS, 0, 0);
                UpdateCountDisplay(hwnd);
            }
            break;
        }
        
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            int code = HIWORD(wParam);
            
            switch (id) {
                case IDC_BROWSE_BTN:
                    BrowseForExe(hwnd);
                    break;
                    
                case IDC_AUTO_DETECT_BTN:
                    AutoDetect(hwnd);
                    break;
                    
                case IDC_START_BTN:
                    StartGames(hwnd);
                    break;
                    
                case IDC_COUNT_EDIT:
                    if (code == EN_CHANGE) {
                        BOOL success;
                        int val = GetDlgItemInt(hwnd, IDC_COUNT_EDIT, &success, FALSE);
                        if (success && val >= 1 && val <= 10) {
                            g_InstanceCount = val;
                            SendMessage(g_hCountSlider, TBM_SETPOS, TRUE, val);
                        }
                    }
                    break;
                    
                case IDOK:
                    StartGames(hwnd);
                    break;
                    
                case IDCANCEL:
                    EndDialog(hwnd, 0);
                    break;
            }
            break;
        }
        
        case WM_CLOSE:
            EndDialog(hwnd, 0);
            return TRUE;
    }
    
    return FALSE;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    g_hInst = hInstance;
    
    // 初始化
    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_UPDOWN_CLASS | ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&icex);
    
    DialogBoxParam(hInstance, MAKEINTRESOURCE(101), NULL, DlgProc, 0);
    
    return 0;
}