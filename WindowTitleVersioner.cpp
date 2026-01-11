// WindowTitleVersioner.cpp : Defines the entry point for the application.
//

#include "framework.h"
#include "WindowTitleVersioner.h"
#include <cstdio>
#include <string>
#include <Psapi.h>
#include <vector>
#include <sstream>
#include <windows.h>
#include <string>
#include <vector>
#include <tlhelp32.h>
#include <psapi.h>
#include <iostream>
#include <sstream>
#include <filesystem>
#include <unordered_set>
#include <shellapi.h>
#include <dwmapi.h>
#include <fstream>
#include <unordered_map>

#pragma comment(lib, "Version.lib")
#pragma comment(lib, "Shell32.lib")

#define WM_TRAYICON (WM_USER + 1)
#define TRAY_ICON_ID 1001

#ifdef _DEBUG
#pragma comment(linker, "/entry:wWinMainCRTStartup /subsystem:console")
#endif // _DEBUG

#define MAX_LOADSTRING 100

// Global Variables:
HINSTANCE hInst;                     // current instance
WCHAR szTitle[MAX_LOADSTRING];       // The title bar text
WCHAR szWindowClass[MAX_LOADSTRING]; // the main window class name

// Forward declarations of functions included in this code module:
LRESULT CALLBACK WndProc  (HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK About    (HWND, UINT, WPARAM, LPARAM);
void    CALLBACK TimerProc(HWND hWnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime);
BOOL    CALLBACK EnumWindowsProc(HWND hwnd, LPARAM);
std::wstring GetExecutablePath(DWORD pid);
std::wstring GetFileVersion(const std::wstring& filePath);
void ProcessWindow(HWND hwnd);
void AddTrayIcon(HWND hwnd);
void RemoveTrayIcon(HWND hwnd);
void ShowTrayMenu(HWND hwnd);

const std::wstring g_ApplicationName = L"WindowTitleVersioner";
const std::wstring g_SettingsIniName = L"WindowTitleVersioner.ini";
/* */ std::wstring g_ApplicationDirPath;
/* */ std::wstring g_ApplicationPath;
/* */ std::wstring g_SettingsIniPath;

struct ApplicationInfo {
    HWND hwnd;
    UINT processId;
    std::wstring processName;
    std::wstring title;
    std::wstring titlePattern;
    std::wstring version;
    std::wstring newTitle;
};

namespace {
    std::unordered_map<std::wstring, std::wstring> g_ProcessInfos;
    std::unordered_map<HWND, ApplicationInfo> g_ProcessedWindows;
}

/**
 *  \brief Splits the string into vector of strings.
 */
std::vector<std::wstring> Split(const std::wstring& s, const std::wstring& seps) {
    std::vector<std::wstring> ret;
    for (size_t p = 0, q; p != std::wstring::npos; p = q) {
        p = s.find_first_not_of(seps, p);
        if (p == std::wstring::npos)
            break;
        q = s.find_first_of(seps, p);
        ret.push_back(s.substr(p, q - p));
    }
    return ret;
}

std::wstring ToLower(const std::wstring& s) {
    std::wstring result = s;
    std::transform(result.begin(), result.end(), result.begin(), towlower);
    return result;
}

std::wstring ToUpper(const std::wstring& s) {
    std::wstring result = s;
    std::transform(result.begin(), result.end(), result.begin(), towupper);
    return result;
}

bool RunAtStartup(const std::wstring& appName, const std::wstring& appPath, const bool enable) {
    printf("[- %-24s | %4d] appName: %ls, appPath: %ls\n", __FUNCTION__, __LINE__, appName.c_str(), appPath.c_str());
    HKEY hKey;
    LONG result =
        RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_WRITE, &hKey);
    if (result != ERROR_SUCCESS) {
        return false;
    }
    if (enable) {
        result = RegSetValueExW(
            hKey,
            appName.c_str(),
            0,
            REG_SZ,
            reinterpret_cast<const BYTE*>(appPath.c_str()),
            static_cast<DWORD>((appPath.size() + 1) * sizeof(wchar_t)));
    } else {
        result = RegDeleteValueW(hKey, appName.c_str());
    }
    RegCloseKey(hKey);
    return result == ERROR_SUCCESS;
}

bool IsRunAtStartupEnabled(const std::wstring& appName, const std::wstring& appPath) {
    printf("[- %-24s | %4d] appName: %ls, appPath: %ls\n", __FUNCTION__, __LINE__, appName.c_str(), appPath.c_str());
    HKEY hKey;
    LONG result =
        RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ, &hKey);
    if (result != ERROR_SUCCESS) {
        return false;
    }
    wchar_t value[MAX_PATH];
    DWORD valueSize = sizeof(value);
    result = RegQueryValueExW(hKey, appName.c_str(), nullptr, nullptr, reinterpret_cast<BYTE*>(value), &valueSize);
    RegCloseKey(hKey);
    if (result != ERROR_SUCCESS) {
        return false;
    }
    return appPath == value;
}

bool IsInvisibleWin10BackgroundAppWindow(HWND hWnd) {
    BOOL isCloaked = FALSE;
    DwmGetWindowAttribute(hWnd, DWMWA_CLOAKED, &isCloaked, sizeof(BOOL));
    return isCloaked == TRUE;
}

/**
 * Get owner window handle for the given hWnd.
 *
 * \param hWnd Window handle
 * \return The owner window handle for the given hWnd.
 */
HWND GetOwnerWindowHwnd(HWND hWnd) {
    HWND hOwner = hWnd;
    do {
        hOwner = GetWindow(hOwner, GW_OWNER);
    } while (GetWindow(hOwner, GW_OWNER));
    hOwner = hOwner ? hOwner : hWnd;
    return hOwner;
}

/**
 * Check if the given window handle's window is a AltTab window.
 *
 * \param hWnd Window handle
 * \return True if the given hWnd is a AltTab window otherwise false.
 */
bool IsAltTabWindow(HWND hWnd) {
    if (!IsWindowVisible(hWnd))
        return false;

    HWND hOwner = GetOwnerWindowHwnd(hWnd);

    // If the last active popup is not the window itself, then it is not a AltTab window
    // if (GetLastActivePopup(hOwner) != hWnd)
    //    return false;

    // Return false if it is a popup window
    if (GetWindowLong(hWnd, GWL_STYLE) & WS_POPUP)
        return false;

    // Even the owner window is hidden we are getting the window styles, so make
    // sure that the owner window is visible before checking the window styles
    DWORD ownerES = GetWindowLong(hOwner, GWL_EXSTYLE);
    if (ownerES && IsWindowVisible(hOwner) && !((ownerES & WS_EX_TOOLWINDOW) && !(ownerES & WS_EX_APPWINDOW))
        && !IsInvisibleWin10BackgroundAppWindow(hOwner)) {
        return true;
    }

    DWORD windowES = GetWindowLong(hWnd, GWL_EXSTYLE);
    if (windowES && !((windowES & WS_EX_TOOLWINDOW) && !(windowES & WS_EX_APPWINDOW))
        && !IsInvisibleWin10BackgroundAppWindow(hWnd)) {
        return true;
    }

    if (windowES == 0 && ownerES == 0) {
        return true;
    }

    return false;
}

bool IsProcessBeingDebugged(DWORD processId) {
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, processId);
    if (!hProcess)
        return false;

    BOOL debuggerPresent = FALSE;
    CheckRemoteDebuggerPresent(hProcess, &debuggerPresent);

    CloseHandle(hProcess);
    return debuggerPresent == TRUE;
}

// ----------------------------------------------------------------------------

template <typename T>
void WriteSetting(const std::wstring& iniFile, LPCTSTR section, LPCTSTR keyName, const T& value) {
    WritePrivateProfileStringW(section, keyName, std::to_wstring(value).c_str(), iniFile.c_str());
}

template <>
void WriteSetting(const std::wstring& iniFile, LPCTSTR section, LPCTSTR keyName, const std::wstring& value) {
    WritePrivateProfileStringW(section, keyName, value.c_str(), iniFile.c_str());
}

template <typename T, typename DefaultType>
void ReadSetting(const std::wstring& iniFile, LPCTSTR section, LPCTSTR keyName, DefaultType defaultValue, T& value) {
    value = GetPrivateProfileIntW(section, keyName, defaultValue, iniFile.c_str());
}

template <>
void ReadSetting(
    const std::wstring& iniFile,
    LPCTSTR section,
    LPCTSTR keyName,
    LPCTSTR defaultValue,
    std::wstring& value) {
    const int bufferSize = 4096; // Initial buffer size
    wchar_t buffer[bufferSize];  // Buffer to store the retrieved string
    GetPrivateProfileStringW(section, keyName, defaultValue, buffer, bufferSize, iniFile.c_str());
    value = buffer;
}

void CreateDefaultSettingsIniFile() {
    printf("[- %-24s | %4d] g_SettingsIniPath: %ls\n", __FUNCTION__, __LINE__, g_SettingsIniPath.c_str());
    // Create default ini file if it does not exist
    if (!std::filesystem::exists(g_SettingsIniPath)) {
        std::ofstream fs(g_SettingsIniPath);
        if (!fs.is_open()) {
            throw std::exception("Failed to create WindowTitleVersioner.ini file");
        }
        fs << "; -----------------------------------------------------------------------------" << std::endl;
        fs << "; Configuration/settings file for WindowTitleVersioner." << std::endl;
        fs << "; Notes:" << std::endl;
        fs << ";   1. Add your process names to the ProcessNames key." << std::endl;
        fs << ";      Ex: ProcessNames=notepad.exe|xplorer2_64.exe|fork.exe" << std::endl;
        fs << ";   2. Each process name should be separated by a pipe (|) character." << std::endl;
        fs << ";   3. The INI file will NOT be reloaded automatically when changes are made." << std::endl;
        fs << ";      Please use 'Reload Settings' menu from the application tray icon." << std::endl;
        fs << ";   4. Please delete this file to create a new settings file when WindowTitleVersioner opens."
           << std::endl;
        fs << ";   5. Default title pattern for each process is \"{Title} - v{Version}   [{AppDirPath}]\"."
           << std::endl;
        fs << ";   6. Change the pattern if you want to customize title for any process as shown below." << std::endl;
        fs << ";      Ex: notepad.exe={Title} | v{Version} | [{AppDirPath}]" << std::endl;
        fs << "; -----------------------------------------------------------------------------" << std::endl;
        fs.close();
        WriteSetting<std::wstring>(
            g_SettingsIniPath, L"Apps", L"ProcessNames", L"notepad.exe|geo.exe|xplorer2_64.exe|fork.exe");
    }
}

void LoadSettingsFromIniFile() {
    // Clear previous process names
    g_ProcessInfos.clear();

    const std::wstring defaultTitlePattern = L"{Title} - v{Version}   [{AppDirPath}]";
    std::wstring processNamesStr;
    ReadSetting<std::wstring>(g_SettingsIniPath, L"Apps", L"ProcessNames", L"", processNamesStr);
    printf("[- %-24s | %4d] processNamesStr: %ls\n", __FUNCTION__, __LINE__, processNamesStr.c_str());
    size_t start = 0;
    size_t end = processNamesStr.find(L'|');
    while (end != std::wstring::npos) {
        std::wstring processName = processNamesStr.substr(start, end - start);
        g_ProcessInfos[ToLower(processName)] = defaultTitlePattern;
        start = end + 1;
        end = processNamesStr.find(L'|', start);
    }
    // Add the last process name
    std::wstring lastProcessName = processNamesStr.substr(start);
    if (!lastProcessName.empty()) {
        g_ProcessInfos[ToLower(lastProcessName)] = defaultTitlePattern;
    }

    // Print loaded process names
    printf("[- %-24s | %4d] Loaded process names to monitor:\n", __FUNCTION__, __LINE__);
    for (const auto& processName : g_ProcessInfos) {
        printf("[- %-24s | %4d]  - %ls\n", __FUNCTION__, __LINE__, processName.first.c_str());
    }

    // Now read the custom title patterns for each process
    for (const auto& processName : g_ProcessInfos) {
        std::wstring titlePattern;
        ReadSetting<std::wstring>(
            g_SettingsIniPath, L"TitlePatterns", processName.first.c_str(), defaultTitlePattern.c_str(), titlePattern);
        g_ProcessInfos[processName.first] = titlePattern;
        printf(
            "[- %-24s | %4d] Title pattern for %ls: %ls\n",
            __FUNCTION__,
            __LINE__,
            processName.first.c_str(),
            titlePattern.c_str());
    }
}

// ----------------------------------------------------------------------------
// Main
// ----------------------------------------------------------------------------
int APIENTRY
wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine, _In_ int nCmdShow) {
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    // TODO: Place code here.

    // Initialize global strings
    LoadStringW(hInstance, IDS_APP_TITLE, szTitle, MAX_LOADSTRING);
    LoadStringW(hInstance, IDC_WINDOWTITLEVERSIONER, szWindowClass, MAX_LOADSTRING);

    hInst = hInstance;
    wchar_t pathBuffer[MAX_PATH]{};
    GetModuleFileNameW(NULL, pathBuffer, MAX_PATH);
    g_ApplicationPath = pathBuffer;
    printf("[- %-24s | %4d] g_ApplicationPath: %ls\n", __FUNCTION__, __LINE__, g_ApplicationPath.c_str());

    std::filesystem::path appPath = g_ApplicationPath;
    g_ApplicationDirPath = appPath.parent_path().wstring();
    printf("[- %-24s | %4d] g_ApplicationDirPath: %ls\n", __FUNCTION__, __LINE__, g_ApplicationDirPath.c_str());

    g_SettingsIniPath = g_ApplicationDirPath + L"\\" + g_SettingsIniName;
    printf("[- %-24s | %4d] g_SettingsIniPath: %ls\n", __FUNCTION__, __LINE__, g_SettingsIniPath.c_str());

    // Register invisible window class
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = szWindowClass;
    RegisterClassW(&wc);

    // Create invisible window
    HWND hwnd = CreateWindowW(
        szWindowClass,
        szTitle,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        0,
        CW_USEDEFAULT,
        0,
        nullptr,
        nullptr,
        hInstance,
        nullptr);

    // Add tray icon
    AddTrayIcon(hwnd);

    // Load settings ini file or create default one
    if (!std::filesystem::exists(g_SettingsIniPath)) {
        CreateDefaultSettingsIniFile();
    }

    // Load settings from ini file
    LoadSettingsFromIniFile();

    // Add initial processes that we want to monitor
#if 0
   g_ProcessNames.insert(L"notepad.exe");
   g_ProcessNames.insert(L"geo.exe");
   g_ProcessNames.insert(L"xplorer2_64.exe");
   g_ProcessNames.insert(L"fork.exe");
#endif // 0

#ifdef _DEBUG
    EnumWindows(EnumWindowsProc, 0);
#else
    // Timer repeats every 1 second
    SetTimer(nullptr, 1, 1000, TimerProc);
#endif // _DEBUG

    HACCEL hAccelTable = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_WINDOWTITLEVERSIONER));

    MSG msg;

    // Main message loop:
    while (GetMessage(&msg, nullptr, 0, 0)) {
        if (!TranslateAccelerator(msg.hwnd, hAccelTable, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    return (int)msg.wParam;
}

void CALLBACK TimerProc(HWND hWnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime) {
    EnumWindows(EnumWindowsProc, 0);

    // Clean up processed PIDs that are no longer running
    std::vector<HWND> windowsToRemove;
    for (const auto& entry : g_ProcessedWindows) {
        UINT pid = entry.second.processId;
        HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
        if (!hProcess) {
            windowsToRemove.push_back(entry.first);
        } else {
            CloseHandle(hProcess);
        }
    }

    for (HWND hwnd : windowsToRemove) {
        g_ProcessedWindows.erase(hwnd);
    }
}

std::wstring GetExecutablePath(DWORD pid) {
    wchar_t path[MAX_PATH]{};
    HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (hProc) {
        GetModuleFileNameExW(hProc, NULL, path, MAX_PATH);
        CloseHandle(hProc);
    }
    return path;
}

std::wstring GetFileVersion(const std::wstring& filePath) {
    DWORD handle = 0;
    DWORD size = GetFileVersionInfoSizeW(filePath.c_str(), &handle);

    if (size == 0)
        return L"";

    std::vector<BYTE> buffer(size);

    if (!GetFileVersionInfoW(filePath.c_str(), 0, size, buffer.data()))
        return L"";

    VS_FIXEDFILEINFO* pInfo = nullptr;
    UINT len = 0;

    if (!VerQueryValueW(buffer.data(), L"\\", (LPVOID*)&pInfo, &len))
        return L"";

    std::wstringstream ss;
    ss << HIWORD(pInfo->dwFileVersionMS) << L"." << LOWORD(pInfo->dwFileVersionMS) << L"."
       << HIWORD(pInfo->dwFileVersionLS) << L"." << LOWORD(pInfo->dwFileVersionLS);

    return ss.str();
}

void ProcessWindow(HWND hwnd) {
#if 0
   // For debugging purposes print all non AltTab windows
   {
      if (!IsWindowVisible(hwnd))
         return;

      wchar_t title[512]{};
      GetWindowTextW(hwnd, title, 512);
      const bool isAltTabWindow = IsAltTabWindow(hwnd);
      if (wcslen(title) > 0) {
         DWORD pid = 0;
         GetWindowThreadProcessId(hwnd, &pid);
         const std::wstring processPath = GetExecutablePath(pid);
         std::filesystem::path filePath = processPath;
         const std::wstring processName = ToLower(filePath.filename().wstring());
         printf("processName: %-30ls, hwnd: %#010x, isAltTabWin: %d, title: %ls\n", processName.c_str(), hwnd, isAltTabWindow, title);
      }
      if (!isAltTabWindow)
         return;
   }
#else
    const bool isAltTabWindow = IsAltTabWindow(hwnd);
    if (!isAltTabWindow)
        return;
#endif

    wchar_t szTitle[512]{};
    GetWindowTextW(hwnd, szTitle, 512);

    if (wcslen(szTitle) == 0)
        return;

    //// Skip windows already processed
    // if (wcsstr(title, L" - v"))
    //    return;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);

    const std::wstring processPath = GetExecutablePath(pid);
    if (processPath.empty())
        return;

    std::filesystem::path filePath = processPath;

    const std::wstring directoryPath = filePath.parent_path().wstring();
    const std::wstring processName = ToLower(filePath.filename().wstring());

    // Do NOT change the title for all windows except the listed ones
    if (g_ProcessInfos.find(processName) == g_ProcessInfos.end())
        return;

    const std::wstring version = GetFileVersion(processPath);
    if (version.empty())
        return;

    std::wstring newTitle = szTitle;
    const std::wstring title = szTitle;

    printf("[- %-24s | %4d] --------------------------------------------\n", __FUNCTION__, __LINE__);

#if 0
   printf("[- %-24s | %4d] processName: %ls\n", __FUNCTION__, __LINE__, processName.c_str());

   // Skip windows already processed
   if (wcsstr(szTitle, L" - v"))
      return;

   // Skip windows already processed
   if (!wcsstr(szTitle, L" - v"))
      newTitle += L" - v" + version + L"   [" + directoryPath + L"]";

   if (!wcsstr(szTitle, L"[Debugging...]") && IsProcessBeingDebugged(pid)) {
      newTitle += L"   [Debugging...]";
   }
#else
    bool isProcessed = false;
    ApplicationInfo appInfo{};
    // First check if the current title already matches the desired pattern
    auto it = g_ProcessedWindows.find(hwnd);
    if (it != g_ProcessedWindows.end()) {
        isProcessed = true;
        appInfo = it->second;
        // return;
    }

    // Follow the title pattern from settings
    const std::wstring titlePattern = g_ProcessInfos[processName];
    printf(
        "[- %-24s | %4d] processName: %ls, titlePattern: %ls\n",
        __FUNCTION__,
        __LINE__,
        processName.c_str(),
        titlePattern.c_str());
    printf("[- %-24s | %4d] title: %ls\n", __FUNCTION__, __LINE__, title.c_str());
    newTitle = titlePattern;
    size_t pos = newTitle.find(L"{Title}");
    if (pos != std::wstring::npos) {
        newTitle.replace(pos, 7, szTitle);
    }

    bool bVersion = false, bVersionFound = false;
    pos = newTitle.find(L"{Version}");
    if (pos != std::wstring::npos) {
        bVersion = true;
        bVersionFound = (newTitle.find(version) != std::wstring::npos);
        newTitle.replace(pos, 9, version);
    }

    bool bAppDirPath = false, bAppDirPathFound = false;
    pos = newTitle.find(L"{AppDirPath}");
    if (pos != std::wstring::npos) {
        bAppDirPath = true;
        bAppDirPathFound = (newTitle.find(directoryPath) != std::wstring::npos);
        newTitle.replace(pos, 12, directoryPath);
    }

    bool bAppPath = false, bAppPathFound = false;
    pos = newTitle.find(L"{AppPath}");
    if (pos != std::wstring::npos) {
        bAppPath = true;
        bAppPathFound = (newTitle.find(processPath) != std::wstring::npos);
        newTitle.replace(pos, 9, processPath);
    }

    if (!wcsstr(szTitle, L"[Debugging...]") && IsProcessBeingDebugged(pid)) {
        newTitle += L"   [Debugging...]";
    }

    auto ProcessPatternComponent = [&](std::wstring& component) {
        // Process each component as needed
        size_t pos = std::wstring::npos;

        pos = component.find(L"{Version}");
        if (pos != std::wstring::npos) {
            // Handle version component
            component.replace(pos, 9, version);
            return;
        }

        pos = component.find(L"{AppDirPath}");
        if (pos != std::wstring::npos) {
            // Handle app dir path component
            component.replace(pos, 12, directoryPath);
            return;
        }

        pos = component.find(L"{AppPath}");
        if (pos != std::wstring::npos) {
            // Handle app path component
            component.replace(pos, 9, processPath);
            return;
        }

        pos = component.find(L"{AppDirPath}");
        if (pos != std::wstring::npos) {
            // Handle app dir path component
            component.replace(pos, 12, directoryPath);
            return;
        }
    };

    // Split components and check if all are present
    bool bAllComponentsFound = true;
    std::vector<std::wstring> components = Split(titlePattern, L" -|/\\");
    for (const auto& comp : components)
        printf("[- %-24s | %4d]  - comp: %ls\n", __FUNCTION__, __LINE__, comp.c_str());
    for (auto comp : components) {
        // Ignore {Title} component
        if (comp == L"{Title}")
            continue;

        // Process each component as needed
        ProcessPatternComponent(comp);
        if (title.find(comp) == std::wstring::npos) {
            bAllComponentsFound = false;
            break;
        }
    }
    printf("[- %-24s | %4d] bAllComponentsFound: %d\n", __FUNCTION__, __LINE__, bAllComponentsFound ? 1 : 0);
    if (bAllComponentsFound) {
        // All components are already present, so skip changing the title
        return;
    }

    printf("[- %-24s | %4d] bVersion: %d, bVersionFound: %d\n", __FUNCTION__, __LINE__, bVersion, bVersionFound);
    printf(
        "[- %-24s | %4d] bAppDirPath: %d, bAppDirPathFound: %d\n",
        __FUNCTION__,
        __LINE__,
        bAppDirPath,
        bAppDirPathFound);
    printf("[- %-24s | %4d] bAppPath: %d, bAppPathFound: %d\n", __FUNCTION__, __LINE__, bAppPath, bAppPathFound);

    // Check if version or app dir path were requested but not found in the title
    // if ((bVersion && !bVersionFound) || (bAppDirPath && !bAppDirPathFound) || (bAppPath && !bAppPathFound)) {
    //   // Something was requested but not found, so we proceed to change the title
    //} else {
    //   // Nothing new to add, so skip changing the title
    //   return;
    //}

    // Store processed info
    appInfo.hwnd = hwnd;
    appInfo.processId = pid;
    appInfo.processName = processName;
    appInfo.titlePattern = titlePattern;
    appInfo.version = version;
    appInfo.newTitle = newTitle;
    if (!isProcessed)
        appInfo.title = szTitle;
    g_ProcessedWindows[hwnd] = appInfo;

#endif // 0

    SetWindowTextW(hwnd, newTitle.c_str());
    printf(
        "[- %15s | %4d] processName: %-30ls, title: %ls, newTitle: %ls\n",
        __FUNCTION__,
        __LINE__,
        processName.c_str(),
        szTitle,
        newTitle.c_str());
}

BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM) {
    ProcessWindow(hwnd);
    return TRUE;
}

// ----------------------------------------------------------------------------

void AddTrayIcon(HWND hwnd) {
    NOTIFYICONDATA nid{};
    nid.cbSize = sizeof(NOTIFYICONDATA);
    nid.hWnd = hwnd;
    nid.uID = TRAY_ICON_ID;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = LoadIcon(hInst, MAKEINTRESOURCE(IDI_WINDOWTITLEVERSIONER));
    wcscpy_s(nid.szTip, g_ApplicationName.c_str());
    Shell_NotifyIconW(NIM_ADD, &nid);
}

void RemoveTrayIcon(HWND hwnd) {
    NOTIFYICONDATA nid{};
    nid.cbSize = sizeof(NOTIFYICONDATA);
    nid.hWnd = hwnd;
    nid.uID = TRAY_ICON_ID;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

void ShowTrayMenu(HWND hwnd) {
    POINT pt;
    GetCursorPos(&pt);
    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING, IDM_ABOUT, L"About");
    const bool runAtStartupEnabled = IsRunAtStartupEnabled(g_ApplicationName, g_ApplicationPath);
    AppendMenuW(hMenu, MF_STRING | (runAtStartupEnabled ? MF_CHECKED : 0), IDM_RUN_AT_STARTUP, L"Run at Startup");
    AppendMenuW(hMenu, MF_STRING, IDM_RELOAD_SETTINGS, L"Reload Settings");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, IDM_EXIT, L"Exit");
    SetForegroundWindow(hwnd);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(hMenu);
}

// ----------------------------------------------------------------------------

INT_PTR CALLBACK About(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_INITDIALOG:
        return TRUE;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
            EndDialog(hDlg, LOWORD(wParam));
            return TRUE;
        }
        break;
    }
    return FALSE;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_TRAYICON:
        if (lParam == WM_RBUTTONUP) {
            ShowTrayMenu(hwnd);
        }
        break;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDM_ABOUT:
            DialogBox(hInst, MAKEINTRESOURCE(IDD_ABOUTBOX), hwnd, About);
            break;

        case IDM_RUN_AT_STARTUP: {
            const bool runAtStartupEnabled = IsRunAtStartupEnabled(g_ApplicationName, g_ApplicationPath);
            const bool result = RunAtStartup(g_ApplicationName, g_ApplicationPath, !runAtStartupEnabled);
            if (!result) {
                MessageBoxW(hwnd, L"Failed to update run at startup setting.", L"Error", MB_OK | MB_ICONERROR);
            }
        } break;

        case IDM_RELOAD_SETTINGS:
            LoadSettingsFromIniFile();
            break;

        case IDM_EXIT:
            DestroyWindow(hwnd);
            break;
        }
        break;

    case WM_DESTROY:
        RemoveTrayIcon(hwnd);
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}
