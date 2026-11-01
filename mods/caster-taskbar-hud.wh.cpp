// ==WindhawkMod==
// @id              caster-taskbar-hud
// @name            Caster & ADCE Taskbar HUD
// @description     Displays real-time Caster voice recognition commands, active grammar rules, and ADCE context zones directly in the Windows 11 taskbar next to the system tray
// @version         1.1.2
// @author          Amir Farhadi
// @github          https://github.com/amirf147
// @homepage        https://github.com/amirf147/caster-taskbar-hud
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -DWIN32_LEAN_AND_MEAN -lole32 -loleaut32 -lruntimeobject -lwindowsapp -luser32 -lshell32 -lgdi32 -ladvapi32 -lcomctl32
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Caster & ADCE Taskbar HUD

A high-performance Windows 11 taskbar HUD designed for voice control and accessibility workflows ([Caster](https://github.com/dictation-toolbox/Caster), Dragonfly, and [Active Desktop Context Engine](https://github.com/amirf147/active-desktop-context-engine)).

It embeds a dynamic, Fluent-styled command strip or discrete pills directly into the taskbar adjacent to the System Tray:
1. **Status Dot Indicator**: Real-time microphone and speech state (emerald green awake, crimson red sleeping, amber streaming).
2. **Rotating Carousel (Default)**: During active speech, renders recognized voice commands. While idle, smoothly cycles through the ADCE zone (purple themed), active CCR rules (blue themed), and last command.
3. **Multi-Box Discrete Pills**: Optional side-by-side mode rendering 3 distinct pills for ADCE, rules, and command streaming.
4. **In-Situ Context Menu**: Right-click the HUD directly on the taskbar to switch display modes and toggle component visibility with registry persistence.

## Companion Caster Plugin
Install the companion Caster bridge from [caster-plugins](https://github.com/amirf147/caster-plugins):
```powershell
py -3.10 -m castervoice.bin.plugin_cli install taskbar_hud
```

## Features
- **Zero-Latency IPC**: Operates an asynchronous Windows Named Pipe server (`\\.\pipe\CasterTaskbarHud`) to ingest telemetry packets in `< 0.5ms`.
- **Native XAML Integration**: Injected natively into Windows 11's `SystemTrayFrameGrid` with automatic light/dark theme adaptation, DPI scaling, and multi-monitor support.
- **In-Situ Configuration**: Right-click menu directly on the taskbar allows changing modes without opening external settings.
- **Safe Teardown & Re-injection**: Cleanly removes all injected controls, subclasses, and timers when unloaded, with automatic bounded retry upon Explorer restarts.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- displayMode: rotate
  $name: Display Mode
  $description: Display layout style (rotating carousel, unified single strip, or multi-box discrete pills)
  $options:
    - rotate: Rotating Carousel (Cycles ADCE, Rules, Command when idle)
    - unified: Unified Single Strip (Static Compact Text)
    - multi_box: Multi-Box Discrete Pills (Side-by-Side)
- showAdceZone: true
  $name: Show ADCE Zone
  $description: Include ADCE semantic zone in carousel or side-by-side view
- showActiveRules: true
  $name: Show Active Rules
  $description: Include active contextual grammar rules in carousel or side-by-side view
- showCommandStream: true
  $name: Show Command Stream
  $description: Include recognized voice commands and live streaming hypotheses
- carouselIntervalSec: 4
  $name: Carousel Rotation Interval (seconds)
  $description: Time in seconds before rotating to the next indicator when idle
- commandClearTimeoutSec: 6
  $name: Command Hold Timeout (seconds)
  $description: Time in seconds to keep recognized command displayed before resuming idle carousel
- customPipeName: CasterTaskbarHud
  $name: Named Pipe Name
  $description: Windows Named Pipe name (\\.\pipe\<name>)
- maxCommandLength: 35
  $name: Max Command Text Length
  $description: Truncate long command strings to this number of characters
*/
// ==/WindhawkModSettings==

#include <windhawk_utils.h>

#include <windows.h>
#include <commctrl.h>
#include <unknwn.h>

#ifdef GetCurrentTime
#undef GetCurrentTime
#endif

#include <winrt/base.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Xaml.Media.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <sstream>
#include <vector>

using namespace winrt::Windows::Data::Json;
using namespace winrt::Windows::UI;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Input;
using namespace winrt::Windows::UI::Xaml::Media;

namespace {

/**********************************************/
// Settings and State
/**********************************************/

constexpr UINT_PTR kHudRetryTimerId = 0x43415354; // "CAST"
constexpr int kMaxStartupRetries = 10;

struct ModSettings {
    std::wstring displayMode = L"rotate"; // "rotate", "unified", "multi_box"
    bool showAdceZone = true;
    bool showActiveRules = true;
    bool showCommandStream = true;
    int carouselIntervalSec = 4;
    int commandClearTimeoutSec = 6;
    std::wstring customPipeName = L"CasterTaskbarHud";
    int maxCommandLength = 35;
};

static ModSettings g_settings;
static std::mutex g_settingsMutex;

struct HudTelemetry {
    std::wstring adceZone = L"--";
    std::wstring rules = L"Global";
    std::wstring command = L"Ready";
    std::wstring lastCommand = L"Ready";
    std::wstring status = L"idle"; // "idle", "streaming", "recognized", "sleeping", "error"
    std::wstring micState = L"on"; // "on", "sleeping", "off"
    uint64_t timestampMs = 0;
};

static HudTelemetry g_telemetry;
static std::mutex g_telemetryMutex;

struct TaskbarUiInstance {
    HWND hWnd = nullptr;
    DWORD ownerThreadId = 0;
    bool windowSubclassed = false;
    bool injected = false;
    int retryCount = 0;

    Panel injectionParent{nullptr};
    ColumnDefinition hudColumnDefinition{nullptr};
    StackPanel container{nullptr};

    // Unified / Carousel Pill
    Border unifiedBorder{nullptr};
    StackPanel unifiedInner{nullptr};
    Border unifiedStatusDot{nullptr};
    TextBlock unifiedText{nullptr};

    // Discrete Pills (Multi-Box)
    Border adceBorder{nullptr};
    TextBlock adceText{nullptr};

    Border rulesBorder{nullptr};
    TextBlock rulesText{nullptr};

    Border commandBorder{nullptr};
    StackPanel commandInner{nullptr};
    Border commandStatusDot{nullptr};
    TextBlock commandText{nullptr};

    // Rotational Carousel State
    int carouselIndex = 0; // 0: ADCE Zone, 1: Active Rules, 2: Last Command / Status
    uint64_t lastCarouselSwitchMs = 0;

    DispatcherTimer clearTimer{nullptr};
    winrt::event_token clearTimerToken{};
    winrt::event_token rightTappedToken{};
};

static std::vector<std::unique_ptr<TaskbarUiInstance>> g_uiInstances;
static std::mutex g_uiInstancesMutex;

static std::atomic<bool> g_unloading{false};
static std::atomic<bool> g_taskbarDllHooked{false};
static HANDLE g_stopEvent = nullptr;
static HANDLE g_pipeThread = nullptr;

/**********************************************/
// Symbol Hook Definitions
/**********************************************/

using CTaskBand_GetTaskbarHost_t = void(WINAPI*)(void*, void**);
static CTaskBand_GetTaskbarHost_t CTaskBand_GetTaskbarHost_Original = nullptr;
static CTaskBand_GetTaskbarHost_t CSecondaryTaskBand_GetTaskbarHost_Original = nullptr;

using TaskbarHost_FrameHeight_t = int(WINAPI*)(void*);
static TaskbarHost_FrameHeight_t TaskbarHost_FrameHeight_Original = nullptr;

using Std_Ref_Decref_t = void(WINAPI*)(void*);
static Std_Ref_Decref_t Std_Ref_Decref_Original = nullptr;

static void* CTaskBand_ITaskListWndSite_vftable = nullptr;
static void* CSecondaryTaskBand_ITaskListWndSite_vftable = nullptr;

using TrayUI_StartTaskbar_t = void(WINAPI*)(void*);
static TrayUI_StartTaskbar_t TrayUI_StartTaskbar_Original = nullptr;

/**********************************************/
// Forward Declarations
/**********************************************/

static void PostUiUpdate();
static void TriggerInject(HWND hWnd);
static void TriggerInjectAll();
static void RemoveAllHudInstances();
static void UpdateHudUi(TaskbarUiInstance& state);
static TaskbarUiInstance* FindUiState(HWND hWnd);
static void ShowTaskbarContextMenu(HWND hWnd);
static bool InjectHudIntoTaskbar(HWND hWnd);

/**********************************************/
// Registry Persistence Helpers
/**********************************************/

static const wchar_t* kRegistrySubKey = L"Software\\Caster\\TaskbarHud";

static void SaveRegistryString(PCWSTR valueName, PCWSTR value) {
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegistrySubKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, valueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(value), static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t)));
        RegCloseKey(hKey);
    }
}

static void SaveRegistryDword(PCWSTR valueName, DWORD value) {
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegistrySubKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, valueName, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
        RegCloseKey(hKey);
    }
}

static bool ReadRegistryString(PCWSTR valueName, std::wstring& out) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegistrySubKey, 0, KEY_QUERY_VALUE, &hKey) == ERROR_SUCCESS) {
        wchar_t buf[256] = {};
        DWORD cb = sizeof(buf);
        DWORD type = 0;
        if (RegQueryValueExW(hKey, valueName, nullptr, &type, reinterpret_cast<BYTE*>(buf), &cb) == ERROR_SUCCESS && type == REG_SZ) {
            out = buf;
            RegCloseKey(hKey);
            return true;
        }
        RegCloseKey(hKey);
    }
    return false;
}

static bool ReadRegistryDword(PCWSTR valueName, int& out) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegistrySubKey, 0, KEY_QUERY_VALUE, &hKey) == ERROR_SUCCESS) {
        DWORD val = 0;
        DWORD cb = sizeof(val);
        DWORD type = 0;
        if (RegQueryValueExW(hKey, valueName, nullptr, &type, reinterpret_cast<BYTE*>(&val), &cb) == ERROR_SUCCESS && type == REG_DWORD) {
            out = static_cast<int>(val);
            RegCloseKey(hKey);
            return true;
        }
        RegCloseKey(hKey);
    }
    return false;
}

/**********************************************/
// Window Thread Marshaling
/**********************************************/

using WindowThreadProc = bool (*)(void* param);

static bool RunFromWindowThread(HWND hWnd, WindowThreadProc proc, void* param) {
    static const UINT kMsg = RegisterWindowMessageW(L"Windhawk_RunFromWindowThread_" WH_MOD_ID);
    struct Payload {
        WindowThreadProc proc;
        void* param;
        std::atomic<bool> ran{false};
        std::atomic<bool> result{false};
        Payload(WindowThreadProc p, void* a) : proc(p), param(a) {}
    };
    using PayloadRef = std::shared_ptr<Payload>;
    static std::mutex pendingPayloadsMutex;
    static std::vector<std::pair<UINT_PTR, PayloadRef>> pendingPayloads;
    static std::atomic<UINT_PTR> nextPayloadId{1};

    DWORD tid = GetWindowThreadProcessId(hWnd, nullptr);
    if (!tid) return false;
    if (tid == GetCurrentThreadId()) {
        return proc(param);
    }

    auto payload = std::make_shared<Payload>(proc, param);
    UINT_PTR id = nextPayloadId.fetch_add(1, std::memory_order_relaxed);
    if (!id) id = nextPayloadId.fetch_add(1, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lk(pendingPayloadsMutex);
        pendingPayloads.emplace_back(id, payload);
    }

    HHOOK hook = SetWindowsHookExW(
        WH_CALLWNDPROC,
        [](int nCode, WPARAM wParam, LPARAM lParam) -> LRESULT {
            if (nCode == HC_ACTION && lParam) {
                const auto* cwp = reinterpret_cast<const CWPSTRUCT*>(lParam);
                if (cwp->message == kMsg && cwp->wParam) {
                    UINT_PTR id = static_cast<UINT_PTR>(cwp->wParam);
                    PayloadRef target;
                    {
                        std::lock_guard<std::mutex> lk(pendingPayloadsMutex);
                        for (auto it = pendingPayloads.begin(); it != pendingPayloads.end(); ++it) {
                            if (it->first == id) {
                                target = it->second;
                                pendingPayloads.erase(it);
                                break;
                            }
                        }
                    }
                    if (target && !target->ran.exchange(true)) {
                        target->result.store(target->proc(target->param));
                    }
                }
            }
            return CallNextHookEx(nullptr, nCode, wParam, lParam);
        },
        nullptr,
        tid
    );

    if (!hook) {
        std::lock_guard<std::mutex> lk(pendingPayloadsMutex);
        for (auto it = pendingPayloads.begin(); it != pendingPayloads.end(); ++it) {
            if (it->first == id) {
                pendingPayloads.erase(it);
                break;
            }
        }
        return false;
    }

    SendMessageW(hWnd, kMsg, static_cast<WPARAM>(id), 0);
    UnhookWindowsHookEx(hook);
    return payload->result.load();
}

/**********************************************/
// Taskbar Window Enumeration & Subclass
/**********************************************/

static std::vector<HWND> FindCurrentProcessTaskbarWnds() {
    std::vector<HWND> result;
    DWORD currentPid = GetCurrentProcessId();

    HWND hPrimary = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (hPrimary) {
        DWORD pid = 0;
        GetWindowThreadProcessId(hPrimary, &pid);
        if (pid == currentPid) result.push_back(hPrimary);
    }

    HWND hSecondary = nullptr;
    while ((hSecondary = FindWindowExW(nullptr, hSecondary, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr) {
        DWORD pid = 0;
        GetWindowThreadProcessId(hSecondary, &pid);
        if (pid == currentPid) result.push_back(hSecondary);
    }

    return result;
}

static TaskbarUiInstance* FindUiState(HWND hWnd) {
    std::lock_guard<std::mutex> lk(g_uiInstancesMutex);
    for (const auto& instance : g_uiInstances) {
        if (instance->hWnd == hWnd) return instance.get();
    }
    return nullptr;
}

static LRESULT CALLBACK TaskbarWindowSubclassProc(
    HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
    DWORD_PTR dwRefData) {
    if (uMsg == WM_NCDESTROY) {
        KillTimer(hWnd, kHudRetryTimerId);
        WindhawkUtils::RemoveWindowSubclassFromAnyThread(hWnd, TaskbarWindowSubclassProc);
        std::lock_guard<std::mutex> lk(g_uiInstancesMutex);
        for (auto it = g_uiInstances.begin(); it != g_uiInstances.end(); ++it) {
            if ((*it)->hWnd == hWnd) {
                g_uiInstances.erase(it);
                break;
            }
        }
        return DefSubclassProc(hWnd, uMsg, wParam, lParam);
    }

    if (uMsg == WM_TIMER && wParam == kHudRetryTimerId) {
        KillTimer(hWnd, kHudRetryTimerId);
        if (!g_unloading.load(std::memory_order_acquire)) {
            TaskbarUiInstance* state = FindUiState(hWnd);
            if (!state || !state->injected) {
                if (InjectHudIntoTaskbar(hWnd)) {
                    Wh_Log(L"Caster HUD retry injection succeeded for %p", hWnd);
                } else if (state && state->retryCount < kMaxStartupRetries) {
                    state->retryCount++;
                    SetTimer(hWnd, kHudRetryTimerId, 500, nullptr);
                }
            }
        }
        return 0;
    }

    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

/**********************************************/
// XAML Discovery & Tree Traversal
/**********************************************/

static HRESULT TryGetTaskbarElementAbi(HWND hTaskbarWnd, void** result) {
    *result = nullptr;
    void* taskbarHostSharedPtr[2]{};

    auto cleanup = [&]() {
        if (taskbarHostSharedPtr[1] && Std_Ref_Decref_Original) {
            Std_Ref_Decref_Original(taskbarHostSharedPtr[1]);
        }
    };

    wchar_t cls[64] = {};
    bool isSecondary = GetClassNameW(hTaskbarWnd, cls, ARRAYSIZE(cls)) &&
                       _wcsicmp(cls, L"Shell_SecondaryTrayWnd") == 0;
    HWND hTaskSwWnd = isSecondary ? FindWindowExW(hTaskbarWnd, nullptr, L"WorkerW", nullptr)
                                  : (HWND)GetProp(hTaskbarWnd, L"TaskbandHWND");
    if (!hTaskSwWnd) return E_HANDLE;

    void* taskBand = (void*)GetWindowLongPtrW(hTaskSwWnd, 0);
    if (!taskBand) return E_POINTER;

    void* taskBandVftable = isSecondary ? CSecondaryTaskBand_ITaskListWndSite_vftable
                                        : CTaskBand_ITaskListWndSite_vftable;
    auto getTaskbarHost = isSecondary ? CSecondaryTaskBand_GetTaskbarHost_Original
                                      : CTaskBand_GetTaskbarHost_Original;
    if (!taskBandVftable || !getTaskbarHost) return E_NOINTERFACE;

    void* taskBandForTaskListWndSite = taskBand;
    for (int i = 0; *(void**)taskBandForTaskListWndSite != taskBandVftable; i++) {
        if (i == 20) return E_NOINTERFACE;
        taskBandForTaskListWndSite = (void**)taskBandForTaskListWndSite + 1;
        if (!taskBandForTaskListWndSite) return E_POINTER;
    }

    getTaskbarHost(taskBandForTaskListWndSite, taskbarHostSharedPtr);
    if (!taskbarHostSharedPtr[0]) {
        cleanup();
        return E_POINTER;
    }

    size_t taskbarElementIUnknownOffset = 0;
#if defined(_M_X64)
    {
        const BYTE* b = (const BYTE*)TaskbarHost_FrameHeight_Original;
        if (b[0] == 0x48 && b[1] == 0x83 && b[2] == 0xEC && b[4] == 0x48 &&
            b[5] == 0x83 && b[6] == 0xC1 && b[7] <= 0x7F) {
            taskbarElementIUnknownOffset = b[7];
        } else {
            cleanup();
            return E_NOINTERFACE;
        }
    }
#elif defined(_M_ARM64)
    {
        const DWORD* p = (const DWORD*)TaskbarHost_FrameHeight_Original;
        if (p[0] == 0xD503237F && (p[1] & 0xFFC07FFF) == 0xA9807BFD &&
            p[2] == 0x910003FD && (p[3] & 0xFFF00FE0) == 0xF8400C00) {
            taskbarElementIUnknownOffset = (p[3] >> 12) & 0xFF;
        } else {
            cleanup();
            return E_NOINTERFACE;
        }
    }
#else
#error "Unsupported architecture"
#endif

    auto* taskbarElementIUnknown =
        *(IUnknown**)((BYTE*)taskbarHostSharedPtr[0] + taskbarElementIUnknownOffset);
    if (!taskbarElementIUnknown) {
        cleanup();
        return E_POINTER;
    }

    HRESULT hr = taskbarElementIUnknown->QueryInterface(winrt::guid_of<FrameworkElement>(), result);
    cleanup();
    if (FAILED(hr) && *result) {
        static_cast<IUnknown*>(*result)->Release();
        *result = nullptr;
    }
    return hr;
}

static XamlRoot GetTaskbarXamlRoot(HWND hTaskbarWnd) {
    if (!CTaskBand_ITaskListWndSite_vftable || !CTaskBand_GetTaskbarHost_Original ||
        !TaskbarHost_FrameHeight_Original) {
        return nullptr;
    }
    void* taskbarElementAbi = nullptr;
    if (FAILED(TryGetTaskbarElementAbi(hTaskbarWnd, &taskbarElementAbi)) || !taskbarElementAbi) {
        return nullptr;
    }

    FrameworkElement taskbarElement{nullptr};
    winrt::attach_abi(taskbarElement, taskbarElementAbi);
    return taskbarElement ? taskbarElement.XamlRoot() : nullptr;
}

static FrameworkElement FindChildByName(FrameworkElement const& root, std::wstring_view name, int depth = 32) {
    if (!root || depth == 0) return nullptr;
    int n = VisualTreeHelper::GetChildrenCount(root);
    for (int i = 0; i < n; ++i) {
        auto child = VisualTreeHelper::GetChild(root, i).try_as<FrameworkElement>();
        if (!child) continue;
        if (child.Name() == name) return child;
        if (auto found = FindChildByName(child, name, depth - 1)) return found;
    }
    return nullptr;
}

/**********************************************/
// UI Building & Teardown
/**********************************************/

static void RemoveHudFromState(TaskbarUiInstance& state) {
    if (state.hWnd) {
        KillTimer(state.hWnd, kHudRetryTimerId);
    }
    if (state.windowSubclassed && state.hWnd) {
        WindhawkUtils::RemoveWindowSubclassFromAnyThread(state.hWnd, TaskbarWindowSubclassProc);
        state.windowSubclassed = false;
    }
    state.retryCount = 0;

    if (state.clearTimer) {
        try {
            state.clearTimer.Stop();
            if (state.clearTimerToken.value) {
                state.clearTimer.Tick(state.clearTimerToken);
                state.clearTimerToken = {};
            }
        } catch (...) {}
        state.clearTimer = nullptr;
    }

    if (state.container && state.rightTappedToken.value) {
        try {
            state.container.RightTapped(state.rightTappedToken);
            state.rightTappedToken = {};
        } catch (...) {}
    }

    if (state.injectionParent && state.container) {
        try {
            auto children = state.injectionParent.Children();
            uint32_t idx = 0;
            if (children.IndexOf(state.container, idx)) {
                children.RemoveAt(idx);
            }
            if (auto grid = state.injectionParent.try_as<Grid>()) {
                if (state.hudColumnDefinition) {
                    uint32_t colIdx = 0;
                    if (grid.ColumnDefinitions().IndexOf(state.hudColumnDefinition, colIdx)) {
                        grid.ColumnDefinitions().RemoveAt(colIdx);
                        for (uint32_t i = 0; i < grid.Children().Size(); ++i) {
                            if (auto child = grid.Children().GetAt(i).try_as<FrameworkElement>()) {
                                int current = Grid::GetColumn(child);
                                if (current > 0) Grid::SetColumn(child, current - 1);
                            }
                        }
                    }
                }
            }
        } catch (...) {}
    }

    state.container = nullptr;
    state.unifiedBorder = nullptr;
    state.unifiedInner = nullptr;
    state.unifiedStatusDot = nullptr;
    state.unifiedText = nullptr;
    state.adceBorder = nullptr;
    state.adceText = nullptr;
    state.rulesBorder = nullptr;
    state.rulesText = nullptr;
    state.commandBorder = nullptr;
    state.commandInner = nullptr;
    state.commandStatusDot = nullptr;
    state.commandText = nullptr;
    state.hudColumnDefinition = nullptr;
    state.injectionParent = nullptr;
    state.injected = false;
}

static bool InjectHudIntoTaskbar(HWND hWnd) {
    if (!hWnd || g_unloading.load(std::memory_order_acquire)) return false;
    DWORD ownerTid = GetWindowThreadProcessId(hWnd, nullptr);
    if (!ownerTid || ownerTid != GetCurrentThreadId()) return false;

    TaskbarUiInstance* state = FindUiState(hWnd);
    if (!state) {
        auto newState = std::make_unique<TaskbarUiInstance>();
        newState->hWnd = hWnd;
        newState->ownerThreadId = ownerTid;
        {
            std::lock_guard<std::mutex> lk(g_uiInstancesMutex);
            g_uiInstances.push_back(std::move(newState));
            state = g_uiInstances.back().get();
        }
        if (!state) return false;
        if (WindhawkUtils::SetWindowSubclassFromAnyThread(hWnd, TaskbarWindowSubclassProc, 0)) {
            state->windowSubclassed = true;
        }
    } else if (state->injected) {
        RemoveHudFromState(*state);
        if (!state->windowSubclassed) {
            if (WindhawkUtils::SetWindowSubclassFromAnyThread(hWnd, TaskbarWindowSubclassProc, 0)) {
                state->windowSubclassed = true;
            }
        }
    }

    auto xamlRoot = GetTaskbarXamlRoot(hWnd);
    if (!xamlRoot) return false;
    auto root = xamlRoot.Content().try_as<FrameworkElement>();
    if (!root) return false;

    auto trayFrame = FindChildByName(root, L"SystemTrayFrameGrid");
    if (!trayFrame) return false;
    auto trayPanel = trayFrame.try_as<Panel>();
    auto trayGrid = trayFrame.try_as<Grid>();
    if (!trayPanel || (!trayGrid && !trayFrame.try_as<StackPanel>())) return false;

    state->injectionParent = trayPanel;

    // --- Main Container ---
    StackPanel container;
    container.Name(L"CasterTaskbarHudContainer");
    container.Orientation(Orientation::Horizontal);
    container.Spacing(4.0);
    container.VerticalAlignment(VerticalAlignment::Center);
    container.Margin(Thickness{3.0, 0.0, 3.0, 0.0});

    // --- 0. Unified Single Strip / Carousel Pill ---
    Border unifiedBorder;
    unifiedBorder.Name(L"CasterUnifiedBox");
    unifiedBorder.CornerRadius(CornerRadius{4.0, 4.0, 4.0, 4.0});
    unifiedBorder.Background(SolidColorBrush(Color{35, 255, 255, 255}));
    unifiedBorder.BorderBrush(SolidColorBrush(Color{65, 255, 255, 255}));
    unifiedBorder.BorderThickness(Thickness{1.0, 1.0, 1.0, 1.0});
    unifiedBorder.Padding(Thickness{7.0, 2.0, 8.0, 2.0});
    unifiedBorder.VerticalAlignment(VerticalAlignment::Center);

    StackPanel unifiedInner;
    unifiedInner.Orientation(Orientation::Horizontal);
    unifiedInner.VerticalAlignment(VerticalAlignment::Center);

    Border unifiedStatusDot;
    unifiedStatusDot.Name(L"CasterUnifiedStatusDot");
    unifiedStatusDot.Width(7.0);
    unifiedStatusDot.Height(7.0);
    unifiedStatusDot.CornerRadius(CornerRadius{3.5, 3.5, 3.5, 3.5});
    unifiedStatusDot.Background(SolidColorBrush(Color{255, 16, 185, 129})); // Emerald green
    unifiedStatusDot.VerticalAlignment(VerticalAlignment::Center);
    unifiedStatusDot.Margin(Thickness{0.0, 0.0, 6.0, 0.0});

    TextBlock unifiedText;
    unifiedText.Name(L"CasterUnifiedText");
    unifiedText.Text(L"Ready");
    unifiedText.FontSize(11.5);
    unifiedText.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
    unifiedText.Foreground(SolidColorBrush(Color{255, 255, 255, 255}));
    unifiedText.TextTrimming(TextTrimming::CharacterEllipsis);
    unifiedText.MaxWidth(190.0);
    unifiedText.VerticalAlignment(VerticalAlignment::Center);

    unifiedInner.Children().Append(unifiedStatusDot);
    unifiedInner.Children().Append(unifiedText);
    unifiedBorder.Child(unifiedInner);

    // --- 1. ADCE Zone Box (Discrete Multi-Box) ---
    Border adceBorder;
    adceBorder.Name(L"CasterAdceBox");
    adceBorder.CornerRadius(CornerRadius{4.0, 4.0, 4.0, 4.0});
    adceBorder.Background(SolidColorBrush(Color{45, 168, 85, 247})); // Purple accent matching Caster HUD
    adceBorder.BorderBrush(SolidColorBrush(Color{120, 168, 85, 247}));
    adceBorder.BorderThickness(Thickness{1.0, 1.0, 1.0, 1.0});
    adceBorder.Padding(Thickness{6.0, 2.0, 6.0, 2.0});
    adceBorder.VerticalAlignment(VerticalAlignment::Center);

    TextBlock adceText;
    adceText.Name(L"CasterAdceText");
    adceText.Text(L"ADCE: --");
    adceText.FontSize(11.0);
    adceText.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
    adceText.Foreground(SolidColorBrush(Color{255, 243, 232, 255}));
    adceText.VerticalAlignment(VerticalAlignment::Center);
    adceBorder.Child(adceText);

    // --- 2. Active Rules Box (Discrete Multi-Box) ---
    Border rulesBorder;
    rulesBorder.Name(L"CasterRulesBox");
    rulesBorder.CornerRadius(CornerRadius{4.0, 4.0, 4.0, 4.0});
    rulesBorder.Background(SolidColorBrush(Color{40, 59, 130, 246})); // Blue accent matching Caster HUD
    rulesBorder.BorderBrush(SolidColorBrush(Color{100, 96, 165, 250}));
    rulesBorder.BorderThickness(Thickness{1.0, 1.0, 1.0, 1.0});
    rulesBorder.Padding(Thickness{6.0, 2.0, 6.0, 2.0});
    rulesBorder.VerticalAlignment(VerticalAlignment::Center);

    TextBlock rulesText;
    rulesText.Name(L"CasterRulesText");
    rulesText.Text(L"Rules: Global");
    rulesText.FontSize(11.0);
    rulesText.Foreground(SolidColorBrush(Color{255, 224, 242, 254}));
    rulesText.VerticalAlignment(VerticalAlignment::Center);
    rulesBorder.Child(rulesText);

    // --- 3. Command Streaming Box (Discrete Multi-Box) ---
    Border commandBorder;
    commandBorder.Name(L"CasterCommandBox");
    commandBorder.CornerRadius(CornerRadius{4.0, 4.0, 4.0, 4.0});
    commandBorder.Background(SolidColorBrush(Color{45, 255, 255, 255}));
    commandBorder.BorderBrush(SolidColorBrush(Color{75, 255, 255, 255}));
    commandBorder.BorderThickness(Thickness{1.0, 1.0, 1.0, 1.0});
    commandBorder.Padding(Thickness{7.0, 2.0, 8.0, 2.0});
    commandBorder.VerticalAlignment(VerticalAlignment::Center);

    StackPanel commandInner;
    commandInner.Orientation(Orientation::Horizontal);
    commandInner.VerticalAlignment(VerticalAlignment::Center);

    Border commandStatusDot;
    commandStatusDot.Name(L"CasterCommandStatusDot");
    commandStatusDot.Width(7.0);
    commandStatusDot.Height(7.0);
    commandStatusDot.CornerRadius(CornerRadius{3.5, 3.5, 3.5, 3.5});
    commandStatusDot.Background(SolidColorBrush(Color{255, 16, 185, 129}));
    commandStatusDot.VerticalAlignment(VerticalAlignment::Center);
    commandStatusDot.Margin(Thickness{0.0, 0.0, 6.0, 0.0});

    TextBlock commandText;
    commandText.Name(L"CasterCommandText");
    commandText.Text(L"Ready");
    commandText.FontSize(11.5);
    commandText.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
    commandText.Foreground(SolidColorBrush(Color{255, 255, 255, 255}));
    commandText.TextTrimming(TextTrimming::CharacterEllipsis);
    commandText.MaxWidth(200.0);
    commandText.VerticalAlignment(VerticalAlignment::Center);

    commandInner.Children().Append(commandStatusDot);
    commandInner.Children().Append(commandText);
    commandBorder.Child(commandInner);

    container.Children().Append(unifiedBorder);
    container.Children().Append(adceBorder);
    container.Children().Append(rulesBorder);
    container.Children().Append(commandBorder);

    // --- Right-Click Context Menu Hook ---
    try {
        state->rightTappedToken = container.RightTapped([hWnd = state->hWnd](auto const&, RightTappedRoutedEventArgs const& e) {
            e.Handled(true);
            ShowTaskbarContextMenu(hWnd);
        });
    } catch (...) {}

    // --- Insert into Taskbar Tray ---
    if (trayGrid) {
        ColumnDefinition colDef;
        colDef.Width(GridLength{1.0, GridUnitType::Auto});
        state->hudColumnDefinition = colDef;
        trayGrid.ColumnDefinitions().InsertAt(0, colDef);
        for (uint32_t i = 0; i < trayGrid.Children().Size(); ++i) {
            if (auto child = trayGrid.Children().GetAt(i).try_as<FrameworkElement>()) {
                Grid::SetColumn(child, Grid::GetColumn(child) + 1);
            }
        }
        Grid::SetColumn(container, 0);
        trayGrid.Children().Append(container);
    } else {
        trayPanel.Children().InsertAt(0, container);
    }

    state->container = container;
    state->unifiedBorder = unifiedBorder;
    state->unifiedInner = unifiedInner;
    state->unifiedStatusDot = unifiedStatusDot;
    state->unifiedText = unifiedText;
    state->adceBorder = adceBorder;
    state->adceText = adceText;
    state->rulesBorder = rulesBorder;
    state->rulesText = rulesText;
    state->commandBorder = commandBorder;
    state->commandInner = commandInner;
    state->commandStatusDot = commandStatusDot;
    state->commandText = commandText;
    state->carouselIndex = 0;
    state->lastCarouselSwitchMs = GetTickCount64();
    state->injected = true;
    state->retryCount = 0;
    KillTimer(hWnd, kHudRetryTimerId);

    // Auto-clear and Carousel DispatcherTimer (ticks every 500ms)
    DispatcherTimer clearTimer;
    clearTimer.Interval(std::chrono::milliseconds(500));
    auto token = clearTimer.Tick([hWnd = state->hWnd](auto const&, auto const&) {
        if (g_unloading.load(std::memory_order_acquire)) return;
        auto* s = FindUiState(hWnd);
        if (!s || !s->injected) return;

        ModSettings settings;
        {
            std::lock_guard<std::mutex> lk(g_settingsMutex);
            settings = g_settings;
        }

        uint64_t nowMs = GetTickCount64();
        uint64_t lastMs = 0;
        std::wstring currentStatus;
        {
            std::lock_guard<std::mutex> lk(g_telemetryMutex);
            lastMs = g_telemetry.timestampMs;
            currentStatus = g_telemetry.status;
        }

        bool needUpdate = false;

        // 1. Check if speech hold timeout expired (only if awake and active)
        if (currentStatus != L"idle" && currentStatus != L"sleeping") {
            int timeoutSec = settings.commandClearTimeoutSec > 0 ? settings.commandClearTimeoutSec : 6;
            if (lastMs > 0 && (nowMs - lastMs) >= static_cast<uint64_t>(timeoutSec * 1000)) {
                {
                    std::lock_guard<std::mutex> lk(g_telemetryMutex);
                    g_telemetry.status = L"idle";
                    g_telemetry.command = L"Ready";
                }
                currentStatus = L"idle";
                s->carouselIndex = 0;
                s->lastCarouselSwitchMs = nowMs;
                needUpdate = true;
            }
        }

        std::wstring currentMicState;
        {
            std::lock_guard<std::mutex> lk(g_telemetryMutex);
            currentMicState = g_telemetry.micState;
        }

        // 2. Advance Carousel only if idle, awake, in rotate mode, and multiple features are active
        if (currentStatus == L"idle" && currentMicState != L"sleeping" && settings.displayMode == L"rotate") {
            bool hasZone = false;
            bool hasRules = false;
            {
                std::lock_guard<std::mutex> lk(g_telemetryMutex);
                hasZone = !g_telemetry.adceZone.empty() && g_telemetry.adceZone != L"--" && g_telemetry.adceZone != L"Unknown";
                hasRules = !g_telemetry.rules.empty() && g_telemetry.rules != L"Global";
            }

            int availableSteps = 1; // Step 2 (Ready / Last Command) is always present
            if (settings.showAdceZone && hasZone) availableSteps++;
            if (settings.showActiveRules && hasRules) availableSteps++;

            if (availableSteps > 1) {
                int intervalSec = settings.carouselIntervalSec > 0 ? settings.carouselIntervalSec : 4;
                if (s->lastCarouselSwitchMs == 0) {
                    s->lastCarouselSwitchMs = nowMs;
                } else if ((nowMs - s->lastCarouselSwitchMs) >= static_cast<uint64_t>(intervalSec * 1000)) {
                    for (int step = 1; step <= 3; ++step) {
                        int nextIdx = (s->carouselIndex + step) % 3;
                        if (nextIdx == 0 && (!settings.showAdceZone || !hasZone)) continue;
                        if (nextIdx == 1 && (!settings.showActiveRules || !hasRules)) continue;
                        s->carouselIndex = nextIdx;
                        break;
                    }
                    s->lastCarouselSwitchMs = nowMs;
                    needUpdate = true;
                }
            } else {
                // If only Ready/Command is active, lock on Step 2 (Green Ready Box)
                if (s->carouselIndex != 2) {
                    s->carouselIndex = 2;
                    needUpdate = true;
                }
            }
        }

        if (needUpdate) {
            UpdateHudUi(*s);
        }
    });

    state->clearTimer = clearTimer;
    state->clearTimerToken = token;
    clearTimer.Start();

    UpdateHudUi(*state);
    Wh_Log(L"Caster & ADCE Taskbar HUD successfully injected into %p", hWnd);
    return true;
}

static void UpdateHudUi(TaskbarUiInstance& state) {
    if (!state.injected || !state.container) return;

    ModSettings settings;
    {
        std::lock_guard<std::mutex> lk(g_settingsMutex);
        settings = g_settings;
    }

    HudTelemetry telemetry;
    {
        std::lock_guard<std::mutex> lk(g_telemetryMutex);
        telemetry = g_telemetry;
    }

    // Determine status dot color (green awake, red sleeping, amber streaming, coral red error)
    Color dotColor = Color{255, 16, 185, 129}; // Vibrant Emerald Green
    if (telemetry.micState == L"sleeping" || telemetry.status == L"sleeping") {
        dotColor = Color{255, 239, 68, 68}; // Crimson Red
    } else if (telemetry.status == L"streaming") {
        dotColor = Color{255, 245, 158, 11}; // Amber
    } else if (telemetry.status == L"error") {
        dotColor = Color{255, 220, 38, 38}; // Coral Red
    }

    if (state.unifiedStatusDot) {
        state.unifiedStatusDot.Background(SolidColorBrush(dotColor));
    }
    if (state.commandStatusDot) {
        state.commandStatusDot.Background(SolidColorBrush(dotColor));
    }

    bool isDiscrete = (settings.displayMode == L"multi_box");

    if (!isDiscrete) {
        // Hide discrete boxes
        state.adceBorder.Visibility(Visibility::Collapsed);
        state.rulesBorder.Visibility(Visibility::Collapsed);
        state.commandBorder.Visibility(Visibility::Collapsed);

        // Show unified / carousel box
        state.unifiedBorder.Visibility(Visibility::Visible);

        std::wstring text;

        if (telemetry.micState == L"sleeping" || telemetry.status == L"sleeping") {
            text = L"Sleeping";
            state.unifiedBorder.Background(SolidColorBrush(Color{45, 239, 68, 68}));
            state.unifiedBorder.BorderBrush(SolidColorBrush(Color{180, 239, 68, 68}));
            state.unifiedText.Foreground(SolidColorBrush(Color{255, 254, 202, 202}));
        } else if (telemetry.status == L"streaming") {
            text = L"... " + (telemetry.command.empty() ? L"listening" : telemetry.command);
            state.unifiedBorder.Background(SolidColorBrush(Color{50, 245, 158, 11}));
            state.unifiedBorder.BorderBrush(SolidColorBrush(Color{180, 245, 158, 11}));
            state.unifiedText.Foreground(SolidColorBrush(Color{255, 253, 230, 138}));
        } else if (telemetry.status == L"recognized") {
            text = telemetry.command;
            state.unifiedBorder.Background(SolidColorBrush(Color{50, 34, 197, 94}));
            state.unifiedBorder.BorderBrush(SolidColorBrush(Color{180, 74, 222, 128}));
            state.unifiedText.Foreground(SolidColorBrush(Color{255, 255, 255, 255}));
        } else if (telemetry.status == L"error") {
            text = telemetry.command.empty() ? L"[?]" : telemetry.command;
            state.unifiedBorder.Background(SolidColorBrush(Color{45, 239, 68, 68}));
            state.unifiedBorder.BorderBrush(SolidColorBrush(Color{160, 239, 68, 68}));
            state.unifiedText.Foreground(SolidColorBrush(Color{255, 255, 255, 255}));
        } else {
            // Idle state: Rotate or Unified
            if (settings.displayMode == L"rotate") {
                // Carousel Mode
                int idx = state.carouselIndex;
                bool hasZone = !telemetry.adceZone.empty() && telemetry.adceZone != L"--" && telemetry.adceZone != L"Unknown";
                bool hasRules = !telemetry.rules.empty() && telemetry.rules != L"Global";

                if (idx == 0 && (!settings.showAdceZone || !hasZone)) {
                    idx = (settings.showActiveRules && hasRules) ? 1 : 2;
                }
                if (idx == 1 && (!settings.showActiveRules || !hasRules)) {
                    idx = (settings.showAdceZone && hasZone) ? 0 : 2;
                }

                if (idx == 0 && settings.showAdceZone) {
                    // State 0: ADCE Zone (Purple themed matching Caster HUD adce_bar.py)
                    if (hasZone) {
                        if (telemetry.adceZone.front() == L'{' || telemetry.adceZone.front() == L'[') {
                            text = L"ADCE: " + telemetry.adceZone;
                        } else {
                            text = L"ADCE: {" + telemetry.adceZone + L"}";
                        }
                    } else {
                        text = L"ADCE: [Offline]";
                    }
                    state.unifiedBorder.Background(SolidColorBrush(Color{45, 168, 85, 247}));
                    state.unifiedBorder.BorderBrush(SolidColorBrush(Color{140, 192, 132, 252}));
                    state.unifiedText.Foreground(SolidColorBrush(Color{255, 243, 232, 255}));
                } else if (idx == 1 && settings.showActiveRules) {
                    // State 1: Active Rules (Blue themed matching Caster HUD active_rules_bar.py)
                    if (hasRules) {
                        text = L"Rules: " + telemetry.rules;
                    } else {
                        text = L"Rules: Global";
                    }
                    state.unifiedBorder.Background(SolidColorBrush(Color{40, 59, 130, 246}));
                    state.unifiedBorder.BorderBrush(SolidColorBrush(Color{120, 96, 165, 250}));
                    state.unifiedText.Foreground(SolidColorBrush(Color{255, 224, 242, 254}));
                } else {
                    // State 2: Last Command / Status
                    if (!telemetry.lastCommand.empty() && telemetry.lastCommand != L"Ready") {
                        text = L"Last: " + telemetry.lastCommand;
                    } else {
                        text = L"Caster Ready";
                    }
                    state.unifiedBorder.Background(SolidColorBrush(Color{35, 34, 197, 94}));
                    state.unifiedBorder.BorderBrush(SolidColorBrush(Color{140, 74, 222, 128}));
                    state.unifiedText.Foreground(SolidColorBrush(Color{255, 255, 255, 255}));
                }
            } else {
                // Static Unified mode
                bool hasZone = settings.showAdceZone && !telemetry.adceZone.empty() && telemetry.adceZone != L"--" && telemetry.adceZone != L"Unknown";
                bool hasRules = settings.showActiveRules && !telemetry.rules.empty() && telemetry.rules != L"Global";

                if (hasZone && hasRules) {
                    text = telemetry.adceZone + L" | " + telemetry.rules;
                } else if (hasZone) {
                    text = telemetry.adceZone;
                } else if (hasRules) {
                    text = L"Ready (" + telemetry.rules + L")";
                } else {
                    text = (telemetry.command.empty() || telemetry.command == L"Ready") ? L"Ready" : telemetry.command;
                }

                state.unifiedBorder.Background(SolidColorBrush(Color{35, 255, 255, 255}));
                state.unifiedBorder.BorderBrush(SolidColorBrush(Color{65, 255, 255, 255}));
                state.unifiedText.Foreground(SolidColorBrush(Color{210, 255, 255, 255}));
            }
        }

        if (settings.maxCommandLength > 0 && static_cast<int>(text.length()) > settings.maxCommandLength) {
            text = text.substr(0, settings.maxCommandLength - 3) + L"...";
        }
        state.unifiedText.Text(text);

    } else {
        // Multi-Box (Side-by-Side) Mode
        state.unifiedBorder.Visibility(Visibility::Collapsed);

        state.adceBorder.Visibility(settings.showAdceZone ? Visibility::Visible : Visibility::Collapsed);
        state.rulesBorder.Visibility(settings.showActiveRules ? Visibility::Visible : Visibility::Collapsed);
        state.commandBorder.Visibility(settings.showCommandStream ? Visibility::Visible : Visibility::Collapsed);

        // ADCE Box (Purple theme)
        std::wstring adceFormatted;
        if (telemetry.adceZone.empty() || telemetry.adceZone == L"--" || telemetry.adceZone == L"Unknown") {
            adceFormatted = L"ADCE: --";
        } else if (telemetry.adceZone.front() == L'{' || telemetry.adceZone.front() == L'[') {
            adceFormatted = L"ADCE: " + telemetry.adceZone;
        } else {
            adceFormatted = L"ADCE: {" + telemetry.adceZone + L"}";
        }
        state.adceText.Text(adceFormatted);

        // Rules Box (Blue theme)
        std::wstring rulesFormatted = telemetry.rules.empty() ? L"Rules: Global" : (L"Rules: " + telemetry.rules);
        state.rulesText.Text(rulesFormatted);

        // Command Box
        std::wstring cmd = telemetry.command;
        if (settings.maxCommandLength > 0 && static_cast<int>(cmd.length()) > settings.maxCommandLength) {
            cmd = cmd.substr(0, settings.maxCommandLength - 3) + L"...";
        }
        state.commandText.Text(cmd);

        if (telemetry.micState == L"sleeping" || telemetry.status == L"sleeping") {
            state.commandBorder.Background(SolidColorBrush(Color{40, 239, 68, 68}));
            state.commandBorder.BorderBrush(SolidColorBrush(Color{120, 248, 113, 113}));
            state.commandText.Foreground(SolidColorBrush(Color{220, 252, 165, 165}));
        } else if (telemetry.status == L"streaming") {
            state.commandBorder.Background(SolidColorBrush(Color{50, 245, 158, 11}));
            state.commandBorder.BorderBrush(SolidColorBrush(Color{180, 245, 158, 11}));
            state.commandText.Foreground(SolidColorBrush(Color{255, 253, 230, 138}));
        } else if (telemetry.status == L"recognized") {
            state.commandBorder.Background(SolidColorBrush(Color{50, 34, 197, 94}));
            state.commandBorder.BorderBrush(SolidColorBrush(Color{180, 74, 222, 128}));
            state.commandText.Foreground(SolidColorBrush(Color{255, 255, 255, 255}));
        } else {
            state.commandBorder.Background(SolidColorBrush(Color{35, 255, 255, 255}));
            state.commandBorder.BorderBrush(SolidColorBrush(Color{65, 255, 255, 255}));
            state.commandText.Foreground(SolidColorBrush(Color{210, 255, 255, 255}));
        }
    }
}

static void ShowTaskbarContextMenu(HWND hWnd) {
    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;

    ModSettings s;
    {
        std::lock_guard<std::mutex> lk(g_settingsMutex);
        s = g_settings;
    }

    enum {
        ID_MODE_ROTATE = 1001,
        ID_MODE_UNIFIED,
        ID_MODE_MULTIBOX,
        ID_TOGGLE_ADCE = 1010,
        ID_TOGGLE_RULES,
        ID_TOGGLE_COMMAND,
        ID_CYCLE_NEXT = 1020,
    };

    UINT rotateFlag = (s.displayMode == L"rotate") ? (MF_STRING | MF_CHECKED) : MF_STRING;
    UINT unifiedFlag = (s.displayMode == L"unified") ? (MF_STRING | MF_CHECKED) : MF_STRING;
    UINT multiFlag = (s.displayMode == L"multi_box") ? (MF_STRING | MF_CHECKED) : MF_STRING;

    AppendMenuW(hMenu, rotateFlag, ID_MODE_ROTATE, L"Mode: Rotating Carousel (Compact)");
    AppendMenuW(hMenu, unifiedFlag, ID_MODE_UNIFIED, L"Mode: Unified Single Strip (Static)");
    AppendMenuW(hMenu, multiFlag, ID_MODE_MULTIBOX, L"Mode: Side-by-Side (3 Discrete Pills)");

    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    UINT adceFlag = s.showAdceZone ? (MF_STRING | MF_CHECKED) : MF_STRING;
    UINT rulesFlag = s.showActiveRules ? (MF_STRING | MF_CHECKED) : MF_STRING;
    UINT cmdFlag = s.showCommandStream ? (MF_STRING | MF_CHECKED) : MF_STRING;

    AppendMenuW(hMenu, adceFlag, ID_TOGGLE_ADCE, L"Show ADCE Semantic Zone");
    AppendMenuW(hMenu, rulesFlag, ID_TOGGLE_RULES, L"Show Active CCR Rules");
    AppendMenuW(hMenu, cmdFlag, ID_TOGGLE_COMMAND, L"Show Command Stream");

    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, ID_CYCLE_NEXT, L"Cycle Next Item Now");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hWnd);

    int cmd = TrackPopupMenuEx(hMenu, TPM_RIGHTBUTTON | TPM_RETURNCMD, pt.x, pt.y, hWnd, nullptr);
    DestroyMenu(hMenu);

    if (cmd == 0) return;

    bool settingsChanged = false;
    {
        std::lock_guard<std::mutex> lk(g_settingsMutex);
        switch (cmd) {
            case ID_MODE_ROTATE:
                g_settings.displayMode = L"rotate";
                SaveRegistryString(L"displayMode", L"rotate");
                settingsChanged = true;
                break;
            case ID_MODE_UNIFIED:
                g_settings.displayMode = L"unified";
                SaveRegistryString(L"displayMode", L"unified");
                settingsChanged = true;
                break;
            case ID_MODE_MULTIBOX:
                g_settings.displayMode = L"multi_box";
                SaveRegistryString(L"displayMode", L"multi_box");
                settingsChanged = true;
                break;
            case ID_TOGGLE_ADCE:
                g_settings.showAdceZone = !g_settings.showAdceZone;
                SaveRegistryDword(L"showAdceZone", g_settings.showAdceZone ? 1 : 0);
                settingsChanged = true;
                break;
            case ID_TOGGLE_RULES:
                g_settings.showActiveRules = !g_settings.showActiveRules;
                SaveRegistryDword(L"showActiveRules", g_settings.showActiveRules ? 1 : 0);
                settingsChanged = true;
                break;
            case ID_TOGGLE_COMMAND:
                g_settings.showCommandStream = !g_settings.showCommandStream;
                SaveRegistryDword(L"showCommandStream", g_settings.showCommandStream ? 1 : 0);
                settingsChanged = true;
                break;
            case ID_CYCLE_NEXT: {
                TaskbarUiInstance* state = FindUiState(hWnd);
                if (state) {
                    state->carouselIndex = (state->carouselIndex + 1) % 3;
                    state->lastCarouselSwitchMs = GetTickCount64();
                    UpdateHudUi(*state);
                }
                break;
            }
        }
    }

    if (settingsChanged) {
        PostUiUpdate();
    }
}

static void PostUiUpdate() {
    if (g_unloading.load(std::memory_order_acquire)) return;
    for (HWND hWnd : FindCurrentProcessTaskbarWnds()) {
        RunFromWindowThread(hWnd, [](void* param) -> bool {
            HWND h = static_cast<HWND>(param);
            auto* s = FindUiState(h);
            if (!g_unloading.load(std::memory_order_acquire) && s && s->injected) {
                UpdateHudUi(*s);
            }
            return true;
        }, hWnd);
    }
}

static void TriggerInject(HWND hWnd) {
    if (g_unloading.load(std::memory_order_acquire)) return;
    RunFromWindowThread(hWnd, [](void* param) -> bool {
        HWND h = static_cast<HWND>(param);
        if (!InjectHudIntoTaskbar(h)) {
            TaskbarUiInstance* s = FindUiState(h);
            if (s && s->retryCount < kMaxStartupRetries) {
                s->retryCount++;
                SetTimer(h, kHudRetryTimerId, 500, nullptr);
            }
        } else {
            KillTimer(h, kHudRetryTimerId);
            TaskbarUiInstance* s = FindUiState(h);
            if (s) s->retryCount = 0;
        }
        return true;
    }, hWnd);
}

static void TriggerInjectAll() {
    if (g_unloading.load(std::memory_order_acquire)) return;
    for (HWND hWnd : FindCurrentProcessTaskbarWnds()) {
        TriggerInject(hWnd);
    }
}

static void RemoveAllHudInstances() {
    for (HWND hWnd : FindCurrentProcessTaskbarWnds()) {
        RunFromWindowThread(hWnd, [](void* param) -> bool {
            HWND h = static_cast<HWND>(param);
            auto* s = FindUiState(h);
            if (s) RemoveHudFromState(*s);
            return true;
        }, hWnd);
    }

    std::lock_guard<std::mutex> lk(g_uiInstancesMutex);
    for (auto& instance : g_uiInstances) {
        if (instance->windowSubclassed && instance->hWnd) {
            WindhawkUtils::RemoveWindowSubclassFromAnyThread(instance->hWnd, TaskbarWindowSubclassProc);
            instance->windowSubclassed = false;
        }
    }
}

/**********************************************/
// Named Pipe IPC Server
/**********************************************/

static void ProcessIncomingPayload(const char* buffer, DWORD length) {
    if (!buffer || length == 0) return;
    std::string raw(buffer, length);

    int wlen = MultiByteToWideChar(CP_UTF8, 0, raw.c_str(), -1, nullptr, 0);
    if (wlen <= 1) return;
    std::wstring wideStr(wlen - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, raw.c_str(), -1, wideStr.data(), wlen);

    std::wistringstream stream(wideStr);
    std::wstring line;
    bool anyUpdate = false;

    while (std::getline(stream, line)) {
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' ' || line.back() == L'\t')) {
            line.pop_back();
        }
        size_t start = line.find_first_not_of(L" \t");
        if (start == std::wstring::npos) continue;
        if (start > 0) line = line.substr(start);
        if (line.empty()) continue;

        try {
            JsonObject jsonObject{nullptr};
            if (JsonObject::TryParse(line, jsonObject) && jsonObject) {
                bool rulesChanged = false;
                bool zoneChanged = false;

                {
                    std::lock_guard<std::mutex> lk(g_telemetryMutex);

                    if (jsonObject.HasKey(L"adce_zone")) {
                        std::wstring newZone = jsonObject.GetNamedString(L"adce_zone", L"--").c_str();
                        if (newZone != g_telemetry.adceZone && !newZone.empty() && newZone != L"--" && newZone != L"Unknown") {
                            zoneChanged = true;
                        }
                        g_telemetry.adceZone = newZone;
                    }

                    if (jsonObject.HasKey(L"rules")) {
                        std::wstring newRules;
                        auto val = jsonObject.Lookup(L"rules");
                        if (val.ValueType() == JsonValueType::String) {
                            newRules = val.GetString().c_str();
                        } else if (val.ValueType() == JsonValueType::Array) {
                            std::wstring combined;
                            for (auto item : val.GetArray()) {
                                if (item.ValueType() == JsonValueType::String) {
                                    if (!combined.empty()) combined += L", ";
                                    combined += item.GetString().c_str();
                                }
                            }
                            newRules = combined.empty() ? L"Global" : combined;
                        }

                        if (newRules != g_telemetry.rules && !newRules.empty() && newRules != L"Global") {
                            rulesChanged = true;
                        }
                        g_telemetry.rules = newRules;
                    }

                    if (jsonObject.HasKey(L"mic_state")) {
                        g_telemetry.micState = jsonObject.GetNamedString(L"mic_state", L"on").c_str();
                    }

                    std::wstring currentMic = g_telemetry.micState;

                    if (currentMic == L"sleeping" || currentMic == L"off") {
                        // Microphone is asleep/off: unconditionally display crimson red Sleeping
                        g_telemetry.status = L"sleeping";
                        g_telemetry.command = L"Sleeping";
                    } else {
                        // Microphone is awake ("on"): allow active streaming, recognition, and idle states
                        if (jsonObject.HasKey(L"status")) {
                            std::wstring st = jsonObject.GetNamedString(L"status", L"idle").c_str();
                            if (st == L"sleeping") {
                                st = L"idle";
                            }
                            g_telemetry.status = st;
                            if (st == L"recognized" || st == L"streaming") {
                                g_telemetry.timestampMs = GetTickCount64();
                            }
                        } else if (g_telemetry.status == L"sleeping") {
                            g_telemetry.status = L"idle";
                        }

                        if (jsonObject.HasKey(L"command")) {
                            std::wstring cmd = jsonObject.GetNamedString(L"command", L"Ready").c_str();
                            if (cmd == L"Sleeping") {
                                cmd = L"Ready";
                            }
                            g_telemetry.command = cmd;
                            if (cmd != L"Ready" && cmd != L"Sleeping" && cmd != L"Off") {
                                g_telemetry.lastCommand = cmd;
                            }
                            g_telemetry.timestampMs = GetTickCount64();
                        } else if (g_telemetry.command == L"Sleeping") {
                            g_telemetry.command = L"Ready";
                        }
                    }
                } // end g_telemetryMutex lock

                if (rulesChanged) {
                    std::lock_guard<std::mutex> lkUi(g_uiInstancesMutex);
                    for (auto& inst : g_uiInstances) {
                        inst->carouselIndex = 1;
                        inst->lastCarouselSwitchMs = GetTickCount64();
                    }
                } else if (zoneChanged) {
                    std::lock_guard<std::mutex> lkUi(g_uiInstancesMutex);
                    for (auto& inst : g_uiInstances) {
                        inst->carouselIndex = 0;
                        inst->lastCarouselSwitchMs = GetTickCount64();
                    }
                }

                anyUpdate = true;
            }
        } catch (...) {
            continue;
        }
    }

    if (anyUpdate) {
        PostUiUpdate();
    }
}

static DWORD WINAPI PipeServerThreadProc(LPVOID) {
    std::wstring pipeName;
    {
        std::lock_guard<std::mutex> lk(g_settingsMutex);
        pipeName = g_settings.customPipeName;
    }
    std::wstring pipePath = L"\\\\.\\pipe\\" + (pipeName.empty() ? L"CasterTaskbarHud" : pipeName);
    Wh_Log(L"Starting Named Pipe server on %s", pipePath.c_str());

    while (!g_unloading.load(std::memory_order_acquire)) {
        HANDLE hPipe = CreateNamedPipeW(
            pipePath.c_str(),
            PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            4096,
            4096,
            0,
            nullptr
        );

        if (hPipe == INVALID_HANDLE_VALUE) {
            Sleep(500);
            continue;
        }

        OVERLAPPED ovConnect{};
        ovConnect.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        BOOL connected = ConnectNamedPipe(hPipe, &ovConnect);
        if (!connected) {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                HANDLE events[2] = {g_stopEvent, ovConnect.hEvent};
                DWORD wait = WaitForMultipleObjects(2, events, FALSE, INFINITE);
                if (wait != WAIT_OBJECT_0 + 1) {
                    CloseHandle(ovConnect.hEvent);
                    CloseHandle(hPipe);
                    break;
                }
            } else if (err != ERROR_PIPE_CONNECTED) {
                CloseHandle(ovConnect.hEvent);
                CloseHandle(hPipe);
                Sleep(200);
                continue;
            }
        }
        CloseHandle(ovConnect.hEvent);

        char buffer[4096];
        while (!g_unloading.load(std::memory_order_acquire)) {
            OVERLAPPED ovRead{};
            ovRead.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            DWORD bytesRead = 0;
            BOOL readOk = ReadFile(hPipe, buffer, sizeof(buffer) - 1, &bytesRead, &ovRead);
            if (!readOk) {
                DWORD err = GetLastError();
                if (err == ERROR_IO_PENDING) {
                    HANDLE events[2] = {g_stopEvent, ovRead.hEvent};
                    DWORD wait = WaitForMultipleObjects(2, events, FALSE, INFINITE);
                    if (wait != WAIT_OBJECT_0 + 1) {
                        CloseHandle(ovRead.hEvent);
                        break;
                    }
                    GetOverlappedResult(hPipe, &ovRead, &bytesRead, FALSE);
                } else {
                    CloseHandle(ovRead.hEvent);
                    break; // Client disconnected
                }
            }
            CloseHandle(ovRead.hEvent);

            if (bytesRead > 0) {
                buffer[bytesRead] = '\0';
                ProcessIncomingPayload(buffer, bytesRead);
            }
        }

        DisconnectNamedPipe(hPipe);
        CloseHandle(hPipe);
    }

    Wh_Log(L"Named Pipe server stopped");
    return 0;
}

/**********************************************/
// Settings Loading
/**********************************************/

static void LoadSettings() {
    std::lock_guard<std::mutex> lk(g_settingsMutex);
    PCWSTR modeSetting = Wh_GetStringSetting(L"displayMode");
    if (modeSetting && *modeSetting) {
        g_settings.displayMode = modeSetting;
        Wh_FreeStringSetting(modeSetting);
    } else {
        g_settings.displayMode = L"rotate";
    }

    g_settings.showAdceZone = Wh_GetIntSetting(L"showAdceZone") != 0;
    g_settings.showActiveRules = Wh_GetIntSetting(L"showActiveRules") != 0;
    g_settings.showCommandStream = Wh_GetIntSetting(L"showCommandStream") != 0;
    g_settings.carouselIntervalSec = Wh_GetIntSetting(L"carouselIntervalSec");
    if (g_settings.carouselIntervalSec <= 0) g_settings.carouselIntervalSec = 4;
    g_settings.commandClearTimeoutSec = Wh_GetIntSetting(L"commandClearTimeoutSec");
    if (g_settings.commandClearTimeoutSec < 0) g_settings.commandClearTimeoutSec = 6;
    g_settings.maxCommandLength = Wh_GetIntSetting(L"maxCommandLength");
    if (g_settings.maxCommandLength <= 0) g_settings.maxCommandLength = 35;

    PCWSTR pipeSetting = Wh_GetStringSetting(L"customPipeName");
    if (pipeSetting && *pipeSetting) {
        g_settings.customPipeName = pipeSetting;
        Wh_FreeStringSetting(pipeSetting);
    } else {
        g_settings.customPipeName = L"CasterTaskbarHud";
    }

    // Registry overrides take precedence for interactive menu changes
    std::wstring regMode;
    if (ReadRegistryString(L"displayMode", regMode) && !regMode.empty()) {
        g_settings.displayMode = regMode;
    }
    int regVal = 0;
    if (ReadRegistryDword(L"showAdceZone", regVal)) g_settings.showAdceZone = (regVal != 0);
    if (ReadRegistryDword(L"showActiveRules", regVal)) g_settings.showActiveRules = (regVal != 0);
    if (ReadRegistryDword(L"showCommandStream", regVal)) g_settings.showCommandStream = (regVal != 0);
    if (ReadRegistryDword(L"carouselIntervalSec", regVal) && regVal > 0) g_settings.carouselIntervalSec = regVal;
}

/**********************************************/
// Symbol Hooking
/**********************************************/

static void WINAPI TrayUI_StartTaskbar_Hook(void* pThis) {
    TrayUI_StartTaskbar_Original(pThis);
    if (g_unloading.load(std::memory_order_acquire)) return;
    RemoveAllHudInstances();
    TriggerInjectAll();
}

static bool HookTaskbarDllSymbols() {
    HMODULE h = LoadLibraryExW(L"taskbar.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!h) return false;

    WindhawkUtils::SYMBOL_HOOK taskbarDllHooks[] = {
        {{LR"(const CTaskBand::`vftable'{for `ITaskListWndSite'})"}, &CTaskBand_ITaskListWndSite_vftable},
        {{LR"(const CSecondaryTaskBand::`vftable'{for `ITaskListWndSite'})"}, &CSecondaryTaskBand_ITaskListWndSite_vftable},
        {{LR"(public: virtual class std::shared_ptr<class TaskbarHost> __cdecl CTaskBand::GetTaskbarHost(void)const )"}, &CTaskBand_GetTaskbarHost_Original},
        {{LR"(public: virtual class std::shared_ptr<class TaskbarHost> __cdecl CSecondaryTaskBand::GetTaskbarHost(void)const )"}, &CSecondaryTaskBand_GetTaskbarHost_Original},
        {{LR"(public: int __cdecl TaskbarHost::FrameHeight(void)const )"}, &TaskbarHost_FrameHeight_Original},
        {{LR"(public: void __cdecl std::_Ref_count_base::_Decref(void))"}, &Std_Ref_Decref_Original},
        {{LR"(public: virtual void __cdecl TrayUI::StartTaskbar(void))"}, &TrayUI_StartTaskbar_Original, TrayUI_StartTaskbar_Hook},
    };

    return WindhawkUtils::HookSymbols(h, taskbarDllHooks, ARRAYSIZE(taskbarDllHooks));
}

static bool IsShellProcess() {
    PCWSTR cmd = GetCommandLineW();
    if (cmd && (wcsstr(cmd, L"-Embedding") != nullptr ||
                wcsstr(cmd, L"/factory") != nullptr)) {
        return false;
    }

    HWND hPrimary = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (hPrimary) {
        DWORD pid = 0;
        GetWindowThreadProcessId(hPrimary, &pid);
        return pid == GetCurrentProcessId();
    }

    HWND hProgman = FindWindowW(L"Progman", nullptr);
    if (hProgman) {
        DWORD pid = 0;
        GetWindowThreadProcessId(hProgman, &pid);
        return pid == GetCurrentProcessId();
    }

    return true;
}

} // namespace

/**********************************************/
// Windhawk Module Lifecycle
/**********************************************/

BOOL Wh_ModInit() {
    Wh_Log(L"Initializing Caster & ADCE Taskbar HUD Mod v1.1.2");

    if (!IsShellProcess()) {
        Wh_Log(L"Current process is not the shell host; skipping initialization");
        return FALSE;
    }

    g_unloading.store(false, std::memory_order_release);
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    LoadSettings();

    if (!HookTaskbarDllSymbols()) {
        Wh_Log(L"Failed to hook taskbar.dll symbols");
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
        return FALSE;
    }

    g_taskbarDllHooked.store(true, std::memory_order_release);
    return TRUE;
}

void Wh_ModAfterInit() {
    if (!IsShellProcess()) {
        return;
    }
    if (!g_pipeThread) {
        g_pipeThread = CreateThread(nullptr, 0, PipeServerThreadProc, nullptr, 0, nullptr);
    }
    TriggerInjectAll();
}

void Wh_ModSettingsChanged() {
    LoadSettings();
    PostUiUpdate();
}

void Wh_ModBeforeUninit() {
    g_unloading.store(true, std::memory_order_release);
    if (g_stopEvent) {
        SetEvent(g_stopEvent);
    }
    RemoveAllHudInstances();
}

void Wh_ModUninit() {
    if (g_pipeThread) {
        WaitForSingleObject(g_pipeThread, 1500);
        CloseHandle(g_pipeThread);
        g_pipeThread = nullptr;
    }
    if (g_stopEvent) {
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
    }
    {
        std::lock_guard<std::mutex> lk(g_uiInstancesMutex);
        g_uiInstances.clear();
    }
    Wh_Log(L"Caster & ADCE Taskbar HUD Mod unloaded");
}
