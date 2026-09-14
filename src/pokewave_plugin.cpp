#include "plugin_api.h"
#include "teamspeak/public_errors.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>
#endif

namespace {
constexpr int kApiVersion = 26;
constexpr std::size_t kMaxMessageBytes = 256;
constexpr int kMenuOpen = 1;

struct Target { anyID id = 0; std::string name; };
struct Snapshot {
    bool running = false;
    std::uint64_t sent = 0;
    std::uint64_t total = 0;
    double rate = 0.0;
    unsigned int error = ERROR_ok;
    std::string errorText;
};

static TS3Functions g_ts3{};
static std::mutex g_connectionMutex;
static uint64 g_currentSchid = 0;
static std::string g_pluginId;

static void logLine(uint64 schid, LogLevel level, const std::string& text) {
    if (g_ts3.logMessage != nullptr) g_ts3.logMessage(text.c_str(), level, "PokeWave", schid);
}
static void commandReply(uint64 schid, LogLevel level, const std::string& text) {
    logLine(schid, level, text);
    if (g_ts3.printMessageToCurrentTab != nullptr) {
        const std::string line = "[PokeWave] " + text;
        g_ts3.printMessageToCurrentTab(line.c_str());
    }
}
static std::string getErrorText(unsigned int code) {
    if (g_ts3.getErrorMessage != nullptr) {
        char* text = nullptr;
        if (g_ts3.getErrorMessage(code, &text) == ERROR_ok && text != nullptr) {
            std::string result(text);
            if (g_ts3.freeMemory != nullptr) g_ts3.freeMemory(text);
            return result;
        }
    }
    std::ostringstream out; out << "TeamSpeak error 0x" << std::hex << code; return out.str();
}
static uint64 currentSchid() {
    std::lock_guard<std::mutex> lock(g_connectionMutex); return g_currentSchid;
}
static void setCurrentSchid(uint64 schid) {
    std::lock_guard<std::mutex> lock(g_connectionMutex); g_currentSchid = schid;
}
static bool validRate(double rate) { return std::isfinite(rate) && rate > 0.0; }
static bool validMessage(const std::string& message) {
    return !message.empty() && message.size() <= kMaxMessageBytes;
}
static std::string label(const Target& target) {
    std::ostringstream out; out << target.name << " [" << static_cast<unsigned int>(target.id) << "]"; return out.str();
}

static std::vector<Target> visibleClients(uint64 schid, std::string* error) {
    std::vector<Target> clients;
    if (schid == 0 || g_ts3.getClientList == nullptr) {
        if (error != nullptr) *error = "没有已连接的服务器";
        return clients;
    }
    anyID* ids = nullptr;
    const unsigned int result = g_ts3.getClientList(schid, &ids);
    if (result != ERROR_ok) {
        if (error != nullptr) *error = getErrorText(result);
        return clients;
    }
    if (ids != nullptr) {
        for (std::size_t i = 0; ids[i] != 0; ++i) {
            Target target; target.id = ids[i];
            char* name = nullptr;
            if (g_ts3.getClientVariableAsString != nullptr &&
                g_ts3.getClientVariableAsString(schid, target.id, CLIENT_NICKNAME, &name) == ERROR_ok &&
                name != nullptr) {
                target.name = name;
                if (g_ts3.freeMemory != nullptr) g_ts3.freeMemory(name);
            } else {
                target.name = "client " + std::to_string(static_cast<unsigned int>(target.id));
            }
            clients.push_back(std::move(target));
        }
        if (g_ts3.freeMemory != nullptr) g_ts3.freeMemory(ids);
    }
    std::sort(clients.begin(), clients.end(), [](const Target& a, const Target& b) {
        return a.name == b.name ? a.id < b.id : a.name < b.name;
    });
    return clients;
}

class PokeController {
public:
    ~PokeController() { stop(); }

    bool start(uint64 schid, std::vector<Target> targets, double rate, std::uint64_t total,
               std::string message, std::string* error) {
        if (schid == 0) { if (error != nullptr) *error = "没有已连接的服务器"; return false; }
        if (targets.empty()) { if (error != nullptr) *error = "至少选择一个目标"; return false; }
        if (!validRate(rate)) { if (error != nullptr) *error = "速率必须是大于 0 的有限数"; return false; }
        if (total == 0) { if (error != nullptr) *error = "总次数必须是大于 0 的整数"; return false; }
        if (!validMessage(message)) {
            if (error != nullptr) *error = "消息不能为空且最多 256 个 UTF-8 字节";
            return false;
        }
        if (g_ts3.requestClientPoke == nullptr) {
            if (error != nullptr) *error = "当前客户端没有提供 requestClientPoke";
            return false;
        }

        std::thread oldWorker;
        {
            std::lock_guard<std::mutex> lock(lifecycleMutex_);
            if (running_.load()) {
                if (error != nullptr) *error = "已有任务在运行，请先停止";
                return false;
            }
            stopRequested_.store(true);
            if (worker_.joinable()) oldWorker = std::move(worker_);
        }
        if (oldWorker.joinable()) oldWorker.join();

        sent_.store(0);
        total_.store(total);
        rate_.store(rate);
        lastError_.store(ERROR_ok);
        {
            std::lock_guard<std::mutex> lock(errorMutex_);
            lastErrorText_.clear();
        }
        {
            std::lock_guard<std::mutex> lock(lifecycleMutex_);
            stopRequested_.store(false);
            running_.store(true);
            worker_ = std::thread(&PokeController::run, this, schid, std::move(targets), rate, total, std::move(message));
        }
        return true;
    }

    bool running() const { return running_.load(); }

    void stop() {
        std::thread worker;
        {
            std::lock_guard<std::mutex> lock(lifecycleMutex_);
            stopRequested_.store(true);
            if (worker_.joinable()) worker = std::move(worker_);
        }
        cv_.notify_all();
        if (worker.joinable()) worker.join();
        running_.store(false);
    }

    Snapshot snapshot() const {
        Snapshot result;
        result.running = running_.load();
        result.sent = sent_.load();
        result.total = total_.load();
        result.rate = rate_.load();
        result.error = lastError_.load();
        std::lock_guard<std::mutex> lock(errorMutex_);
        result.errorText = lastErrorText_;
        return result;
    }

private:
    void setError(unsigned int code, const std::string& text) {
        lastError_.store(code);
        std::lock_guard<std::mutex> lock(errorMutex_); lastErrorText_ = text;
    }
    void run(uint64 schid, std::vector<Target> targets, double rate,
             std::uint64_t total, std::string message) {
        using Clock = std::chrono::steady_clock;
        const auto requestedInterval = std::chrono::duration<double>(1.0 / rate);
        auto interval = std::chrono::duration_cast<Clock::duration>(requestedInterval);
        if (interval.count() <= 0) interval = Clock::duration(1);
        auto next = Clock::now();
        logLine(schid, LogLevel_INFO, "PokeWave started: " + std::to_string(rate) + " poke/s, " +
                                      std::to_string(total) + " total, " + std::to_string(targets.size()) + " targets");
        for (std::uint64_t i = 0; i < total; ++i) {
            if (stopRequested_.load()) break;
            std::unique_lock<std::mutex> waitLock(waitMutex_);
            cv_.wait_until(waitLock, next, [this]() { return stopRequested_.load(); });
            waitLock.unlock();
            if (stopRequested_.load()) break;
            const Target& target = targets[static_cast<std::size_t>(i % targets.size())];
            const unsigned int result = g_ts3.requestClientPoke(schid, target.id, message.c_str(), nullptr);
            if (result != ERROR_ok) {
                const std::string text = "发送给 " + label(target) + " 失败：" + getErrorText(result);
                setError(result, text);
                logLine(schid, LogLevel_ERROR, "PokeWave stopped: " + text);
                break;
            }
            sent_.fetch_add(1);
            next += interval;
            const auto now = Clock::now();
            if (next < now) next = now + interval;
        }
        const bool userStopped = stopRequested_.load() && lastError_.load() == ERROR_ok;
        running_.store(false);
        if (userStopped) {
            logLine(schid, LogLevel_INFO, "PokeWave stopped: " + std::to_string(sent_.load()) + "/" + std::to_string(total));
        } else if (lastError_.load() == ERROR_ok) {
            logLine(schid, LogLevel_INFO, "PokeWave completed: " + std::to_string(sent_.load()) + "/" + std::to_string(total));
        }
    }

    mutable std::mutex lifecycleMutex_;
    std::thread worker_;
    std::mutex waitMutex_;
    std::condition_variable cv_;
    std::atomic_bool stopRequested_{true};
    std::atomic_bool running_{false};
    std::atomic<std::uint64_t> sent_{0};
    std::atomic<std::uint64_t> total_{0};
    std::atomic<double> rate_{0.0};
    std::atomic<unsigned int> lastError_{ERROR_ok};
    mutable std::mutex errorMutex_;
    std::string lastErrorText_;
};
static PokeController g_controller;

struct CommandConfig {
    double rate = 10.0;
    std::uint64_t total = 100;
    std::string message = "controlled test";
    std::vector<anyID> selected;
};
static std::mutex g_configMutex;
static CommandConfig g_config;

static std::vector<anyID> parseIds(std::string text) {
    std::replace(text.begin(), text.end(), ',', ' ');
    std::istringstream in(text);
    std::vector<anyID> ids;
    std::string token;
    while (in >> token) {
        try {
            std::size_t end = 0;
            const unsigned long value = std::stoul(token, &end, 10);
            if (end == token.size() && value > 0 && value <= std::numeric_limits<anyID>::max()) {
                const anyID id = static_cast<anyID>(value);
                if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
            }
        } catch (...) {}
    }
    return ids;
}
static std::string leftTrim(std::string value) {
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    return first == std::string::npos ? std::string() : value.substr(first);
}
static std::vector<Target> configuredTargets() {
    std::vector<Target> result;
    std::lock_guard<std::mutex> lock(g_configMutex);
    for (const anyID id : g_config.selected) {
        result.push_back(Target{id, "client " + std::to_string(static_cast<unsigned int>(id))});
    }
    return result;
}
static std::string statusText() {
    const Snapshot status = g_controller.snapshot();
    std::ostringstream out;
    if (status.running) {
        out << "running " << status.sent << "/" << status.total << " @ " << std::fixed << std::setprecision(2)
            << status.rate << " poke/s";
    } else if (status.error != ERROR_ok) {
        out << "stopped with error: " << status.errorText;
    } else {
        out << "idle, last sent " << status.sent << "/" << status.total;
    }
    return out.str();
}
static void help(uint64 schid) {
    commandReply(schid, LogLevel_INFO,
            "PokeWave: list | select <id,...> | add <id,...> | remove <id,...> | "
            "speed <positive> | count <positive uint64> | message <text> | start | stop | status");
}

#ifdef _WIN32
enum GuiId {
    kGuiList = 2001, kGuiRate = 2002, kGuiCount = 2003, kGuiMessage = 2004,
    kGuiRefresh = 2010, kGuiSelectAll = 2011, kGuiSelectNone = 2012,
    kGuiStart = 2013, kGuiStop = 2014, kGuiStatus = 2020, kGuiServer = 2021
};
static HWND g_guiWindow = nullptr;
static HWND g_guiList = nullptr;
static HWND g_guiRate = nullptr;
static HWND g_guiCount = nullptr;
static HWND g_guiMessage = nullptr;
static HWND g_guiStatus = nullptr;
static HWND g_guiServer = nullptr;
static uint64 g_guiSchid = 0;
static bool g_guiUpdating = false;

static std::wstring guiW(const std::string& value) {
    if (value.empty()) return {};
    const int n = static_cast<int>(value.size());
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), n, nullptr, 0);
    if (size == 0) size = MultiByteToWideChar(CP_UTF8, 0, value.data(), n, nullptr, 0);
    if (size == 0) return {};
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), n, result.data(), size);
    return result;
}
static std::string guiU(const std::wstring& value) {
    if (value.empty()) return {};
    const int n = static_cast<int>(value.size());
    int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), n, nullptr, 0, nullptr, nullptr);
    if (size == 0) size = WideCharToMultiByte(CP_UTF8, 0, value.data(), n, nullptr, 0, nullptr, nullptr);
    if (size == 0) return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), n, result.data(), size, nullptr, nullptr);
    return result;
}
static std::wstring guiText(HWND control) {
    if (control == nullptr) return {};
    const int n = GetWindowTextLengthW(control);
    std::wstring result(static_cast<std::size_t>(n) + 1, L'\0');
    GetWindowTextW(control, result.data(), n + 1);
    result.resize(static_cast<std::size_t>(n));
    return result;
}
static void guiFont(HWND control) {
    if (control != nullptr) SendMessageW(control, WM_SETFONT,
        reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
}
static HWND guiControl(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style,
                       int x, int y, int w, int h, int id) {
    HWND control = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
        x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr), nullptr);
    guiFont(control);
    return control;
}
static void guiSetText(HWND control, const std::wstring& value) {
    if (control != nullptr) SetWindowTextW(control, value.c_str());
}
static void guiUpdateServerLabel() {
    if (g_guiServer == nullptr) return;
    std::wstring text = L"当前服务器连接: ";
    text += g_guiSchid == 0 ? L"未连接" : std::to_wstring(g_guiSchid);
    guiSetText(g_guiServer, text);
}
static bool guiSelected(anyID id) {
    std::lock_guard<std::mutex> lock(g_configMutex);
    return std::find(g_config.selected.begin(), g_config.selected.end(), id) != g_config.selected.end();
}
static void guiReadSelection() {
    if (g_guiList == nullptr || g_guiUpdating) return;
    std::vector<anyID> selected;
    for (int i = 0, n = ListView_GetItemCount(g_guiList); i < n; ++i) {
        if (!ListView_GetCheckState(g_guiList, i)) continue;
        LVITEMW item{};
        item.mask = LVIF_PARAM;
        item.iItem = i;
        if (ListView_GetItem(g_guiList, &item) != FALSE)
            selected.push_back(static_cast<anyID>(item.lParam));
    }
    std::lock_guard<std::mutex> lock(g_configMutex);
    g_config.selected = std::move(selected);
}
static void guiStatus() {
    if (g_guiStatus != nullptr) guiSetText(g_guiStatus, guiW(statusText()));
}
static void guiSelectAll(bool selected) {
    if (g_guiList == nullptr) return;
    g_guiUpdating = true;
    for (int i = 0, n = ListView_GetItemCount(g_guiList); i < n; ++i)
        ListView_SetCheckState(g_guiList, i, selected ? TRUE : FALSE);
    g_guiUpdating = false;
    guiReadSelection();
}
static void guiRefresh() {
    if (g_guiList == nullptr) return;
    std::string error;
    const auto clients = visibleClients(g_guiSchid, &error);
    g_guiUpdating = true;
    ListView_DeleteAllItems(g_guiList);
    if (!error.empty()) {
        g_guiUpdating = false;
        guiUpdateServerLabel();
        guiSetText(g_guiStatus, guiW(error));
        return;
    }
    for (std::size_t i = 0; i < clients.size(); ++i) {
        const Target& target = clients[i];
        std::wstring name = guiW(target.name);
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = static_cast<int>(i);
        item.pszText = name.empty() ? const_cast<LPWSTR>(L"") : name.data();
        item.lParam = static_cast<LPARAM>(target.id);
        const int row = ListView_InsertItem(g_guiList, &item);
        if (row >= 0) {
            std::wstring id = std::to_wstring(static_cast<unsigned int>(target.id));
            ListView_SetItemText(g_guiList, row, 1, id.data());
            ListView_SetCheckState(g_guiList, row, guiSelected(target.id) ? TRUE : FALSE);
        }
    }
    g_guiUpdating = false;
    guiUpdateServerLabel();
    guiStatus();
}
static bool guiSave() {
    const std::string rateText = guiU(guiText(g_guiRate));
    const std::string countText = guiU(guiText(g_guiCount));
    const std::string message = guiU(guiText(g_guiMessage));
    double rate = 0.0;
    std::uint64_t count = 0;
    try {
        std::size_t end = 0;
        rate = std::stod(rateText, &end);
        if (end != rateText.size() || !validRate(rate)) throw std::invalid_argument("rate");
    } catch (...) {
        MessageBoxW(g_guiWindow, L"速率必须是大于 0 的有限数。", L"PokeWave", MB_OK | MB_ICONERROR);
        return false;
    }
    try {
        std::size_t end = 0;
        const unsigned long long value = std::stoull(countText, &end, 10);
        if (end != countText.size() || value == 0) throw std::invalid_argument("count");
        count = static_cast<std::uint64_t>(value);
    } catch (...) {
        MessageBoxW(g_guiWindow, L"次数必须是大于 0 的整数。", L"PokeWave", MB_OK | MB_ICONERROR);
        return false;
    }
    if (!validMessage(message)) {
        MessageBoxW(g_guiWindow, L"消息不能为空，且最多 256 个 UTF-8 字节。", L"PokeWave", MB_OK | MB_ICONERROR);
        return false;
    }
    guiReadSelection();
    std::lock_guard<std::mutex> lock(g_configMutex);
    g_config.rate = rate;
    g_config.total = count;
    g_config.message = message;
    return true;
}
static void guiStart() {
    if (!guiSave()) return;
    double rate;
    std::uint64_t count;
    std::string message;
    {
        std::lock_guard<std::mutex> lock(g_configMutex);
        rate = g_config.rate;
        count = g_config.total;
        message = g_config.message;
    }
    std::string error;
    if (!g_controller.start(g_guiSchid, configuredTargets(), rate, count, std::move(message), &error))
        MessageBoxW(g_guiWindow, guiW(error).c_str(), L"PokeWave", MB_OK | MB_ICONERROR);
    else
        commandReply(g_guiSchid, LogLevel_INFO, "PokeWave started from GUI.");
    guiStatus();
}
static LRESULT CALLBACK guiProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        guiControl(hwnd, L"STATIC", L"PokeWave \u63a7\u5236\u9762\u677f", SS_LEFT, 16, 12, 500, 26, 0);
        g_guiServer = guiControl(hwnd, L"STATIC", L"", SS_LEFT, 16, 42, 880, 22, kGuiServer);

        g_guiList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS,
            16, 75, 535, 425, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kGuiList)),
            GetModuleHandleW(nullptr), nullptr);
        guiFont(g_guiList);
        ListView_SetExtendedListViewStyle(g_guiList, LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES | LVS_EX_DOUBLEBUFFER);
        LVCOLUMNW nameColumn{};
        nameColumn.mask = LVCF_TEXT | LVCF_WIDTH;
        nameColumn.cx = 405;
        nameColumn.pszText = const_cast<LPWSTR>(L"\u76ee\u6807\u6635\u79f0");
        ListView_InsertColumn(g_guiList, 0, &nameColumn);
        LVCOLUMNW idColumn{};
        idColumn.mask = LVCF_TEXT | LVCF_WIDTH;
        idColumn.cx = 105;
        idColumn.pszText = const_cast<LPWSTR>(L"Client ID");
        ListView_InsertColumn(g_guiList, 1, &idColumn);

        guiControl(hwnd, L"BUTTON", L"\u5237\u65b0", BS_PUSHBUTTON, 16, 515, 90, 30, kGuiRefresh);
        guiControl(hwnd, L"BUTTON", L"\u5168\u9009", BS_PUSHBUTTON, 116, 515, 90, 30, kGuiSelectAll);
        guiControl(hwnd, L"BUTTON", L"\u6e05\u7a7a", BS_PUSHBUTTON, 216, 515, 90, 30, kGuiSelectNone);
        guiControl(hwnd, L"STATIC", L"\u52fe\u9009\u76ee\u6807\u540e\u5f00\u59cb\u53d1\u9001", SS_LEFT, 320, 520, 225, 20, 0);

        guiControl(hwnd, L"BUTTON", L"\u53d1\u9001\u8bbe\u7f6e", BS_GROUPBOX, 570, 75, 330, 270, 0);
        guiControl(hwnd, L"STATIC", L"\u53d1\u9001\u901f\u7387 (poke/s)", SS_LEFT, 590, 112, 140, 20, 0);
        g_guiRate = guiControl(hwnd, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, 735, 108, 140, 24, kGuiRate);
        guiControl(hwnd, L"STATIC", L"\u603b\u6b21\u6570", SS_LEFT, 590, 150, 140, 20, 0);
        g_guiCount = guiControl(hwnd, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | ES_NUMBER, 735, 146, 140, 24, kGuiCount);
        guiControl(hwnd, L"STATIC", L"\u6d88\u606f", SS_LEFT, 590, 188, 140, 20, 0);
        g_guiMessage = guiControl(hwnd, L"EDIT", L"", WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL,
                                  590, 212, 285, 70, kGuiMessage);
        guiControl(hwnd, L"BUTTON", L"\u5f00\u59cb", BS_DEFPUSHBUTTON, 590, 302, 135, 30, kGuiStart);
        guiControl(hwnd, L"BUTTON", L"\u505c\u6b62", BS_PUSHBUTTON, 740, 302, 135, 30, kGuiStop);

        guiControl(hwnd, L"BUTTON", L"\u72b6\u6001", BS_GROUPBOX, 570, 365, 330, 135, 0);
        g_guiStatus = guiControl(hwnd, L"STATIC", L"", SS_LEFT | SS_EDITCONTROL, 590, 402, 285, 75, kGuiStatus);

        double rate;
        std::uint64_t count;
        std::string messageText;
        {
            std::lock_guard<std::mutex> lock(g_configMutex);
            rate = g_config.rate;
            count = g_config.total;
            messageText = g_config.message;
        }
        guiSetText(g_guiRate, guiW(std::to_string(rate)));
        guiSetText(g_guiCount, guiW(std::to_string(count)));
        guiSetText(g_guiMessage, guiW(messageText));
        SendMessageW(g_guiMessage, EM_SETLIMITTEXT, 1024, 0);
        guiUpdateServerLabel();
        guiRefresh();
        SetTimer(hwnd, 1, 250, nullptr);
        return 0;
    }
    case WM_TIMER:
        guiStatus();
        return 0;
    case WM_COMMAND:
        if (HIWORD(wParam) == BN_CLICKED) {
            if (LOWORD(wParam) == kGuiRefresh) guiRefresh();
            else if (LOWORD(wParam) == kGuiSelectAll) guiSelectAll(true);
            else if (LOWORD(wParam) == kGuiSelectNone) guiSelectAll(false);
            else if (LOWORD(wParam) == kGuiStart) guiStart();
            else if (LOWORD(wParam) == kGuiStop) { g_controller.stop(); guiStatus(); }
            return 0;
        }
        break;
    case WM_NOTIFY:
        if (lParam != 0) {
            const NMHDR* header = reinterpret_cast<const NMHDR*>(lParam);
            if (header->idFrom == kGuiList && header->code == LVN_ITEMCHANGED) {
                const NMLISTVIEW* change = reinterpret_cast<const NMLISTVIEW*>(lParam);
                if ((change->uChanged & LVIF_STATE) != 0) guiReadSelection();
            }
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        g_guiWindow = nullptr;
        g_guiList = nullptr;
        g_guiRate = nullptr;
        g_guiCount = nullptr;
        g_guiMessage = nullptr;
        g_guiStatus = nullptr;
        g_guiServer = nullptr;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}
static void showGui(uint64 schid) {
    g_guiSchid = schid != 0 ? schid : currentSchid();
    if (g_guiWindow != nullptr && IsWindow(g_guiWindow) != FALSE) {
        ShowWindow(g_guiWindow, SW_SHOWNORMAL);
        SetForegroundWindow(g_guiWindow);
        guiUpdateServerLabel();
        guiRefresh();
        return;
    }
    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&controls);
    const wchar_t* className = L"PokeWaveGuiWindow";
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW klass{};
    klass.cbSize = sizeof(klass);
    klass.hInstance = instance;
    klass.lpfnWndProc = guiProc;
    klass.lpszClassName = className;
    klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    klass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (GetClassInfoExW(instance, className, &klass) == FALSE) RegisterClassExW(&klass);
    g_guiWindow = CreateWindowExW(WS_EX_APPWINDOW, className, L"PokeWave",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 920, 600, nullptr, nullptr, instance, nullptr);
    if (g_guiWindow == nullptr) {
        logLine(g_guiSchid, LogLevel_ERROR, "Unable to create the PokeWave GUI window.");
    } else {
        ShowWindow(g_guiWindow, SW_SHOWNORMAL);
        UpdateWindow(g_guiWindow);
        SetForegroundWindow(g_guiWindow);
    }
}
#endif
}

extern "C" {
const char* ts3plugin_name() { return "PokeWave"; }
const char* ts3plugin_version() { return "0.3.2"; }
int ts3plugin_apiVersion() { return kApiVersion; }
const char* ts3plugin_author() { return "Local server administrator"; }
const char* ts3plugin_description() { return "Controlled multi-target TeamSpeak poke test tool."; }
void ts3plugin_setFunctionPointers(const struct TS3Functions funcs) { g_ts3 = funcs; }
int ts3plugin_init() { logLine(0, LogLevel_INFO, "PokeWave loaded. No task starts automatically."); return 0; }
void ts3plugin_shutdown() {
    g_controller.stop();
#ifdef _WIN32
    if (g_guiWindow != nullptr && IsWindow(g_guiWindow) != FALSE) DestroyWindow(g_guiWindow);
#endif
    g_pluginId.clear();
}
int ts3plugin_offersConfigure() { return PLUGIN_OFFERS_NO_CONFIGURE; }
void ts3plugin_configure(void*, void*) {}
void ts3plugin_registerPluginID(const char* id) { g_pluginId = id == nullptr ? "" : id; }
const char* ts3plugin_commandKeyword() { return "pokewave"; }
int ts3plugin_requestAutoload() { return 1; }

void ts3plugin_initMenus(struct PluginMenuItem*** menuItems, char** menuIcon) {
    if (menuItems == nullptr) return;
    *menuItems = static_cast<PluginMenuItem**>(std::malloc(sizeof(PluginMenuItem*) * 2));
    if (*menuItems == nullptr) return;
    (*menuItems)[0] = static_cast<PluginMenuItem*>(std::malloc(sizeof(PluginMenuItem)));
    if ((*menuItems)[0] == nullptr) { std::free(*menuItems); *menuItems = nullptr; return; }
    (*menuItems)[0]->type = PLUGIN_MENU_TYPE_GLOBAL;
    (*menuItems)[0]->id = kMenuOpen;
    std::strncpy((*menuItems)[0]->text, "PokeWave GUI", PLUGIN_MENU_BUFSZ - 1);
    (*menuItems)[0]->text[PLUGIN_MENU_BUFSZ - 1] = '\0';
    (*menuItems)[0]->icon[0] = '\0';
    (*menuItems)[1] = nullptr;
    if (menuIcon != nullptr) *menuIcon = nullptr;
}
void ts3plugin_freeMemory(void* data) { std::free(data); }
void ts3plugin_currentServerConnectionChanged(uint64 serverConnectionHandlerID) {
    if (g_controller.running() && currentSchid() != serverConnectionHandlerID) g_controller.stop();
#ifdef _WIN32
    if (g_guiWindow != nullptr && g_guiSchid != serverConnectionHandlerID) DestroyWindow(g_guiWindow);
#endif
    setCurrentSchid(serverConnectionHandlerID);
}
void ts3plugin_onConnectStatusChangeEvent(uint64 schid, int status, unsigned int) {
    if (status == STATUS_CONNECTION_ESTABLISHED) {
        if (currentSchid() == 0) setCurrentSchid(schid);
        logLine(schid, LogLevel_INFO, "Connected. Use /pokewave help.");
    } else if (status == STATUS_DISCONNECTED) {
        g_controller.stop();
        if (currentSchid() == schid) setCurrentSchid(0);
    }
}
void ts3plugin_onMenuItemEvent(uint64 schid, enum PluginMenuType type, int menuItemID, uint64) {
    if (type == PLUGIN_MENU_TYPE_GLOBAL && menuItemID == kMenuOpen) {
#ifdef _WIN32
        showGui(schid);
#else
        help(schid);
#endif
    }
}

int ts3plugin_processCommand(uint64 serverConnectionHandlerID, const char* command) {
    const uint64 schid = serverConnectionHandlerID != 0 ? serverConnectionHandlerID : currentSchid();
    std::istringstream input(command == nullptr ? "" : command);
    std::string verb; input >> verb;
    if (verb.empty() || verb == "help") { help(schid); return 0; }
    if (verb == "list") {
        std::string error; const auto clients = visibleClients(schid, &error);
        if (!error.empty()) commandReply(schid, LogLevel_ERROR, "list failed: " + error);
        else {
            for (const auto& target : clients) commandReply(schid, LogLevel_INFO, label(target));
            commandReply(schid, LogLevel_INFO, "Visible clients: " + std::to_string(clients.size()));
        }
        return 0;
    }
    if (verb == "select" || verb == "add" || verb == "remove") {
        std::string rest; std::getline(input, rest);
        const auto ids = parseIds(rest);
        if (ids.empty()) { commandReply(schid, LogLevel_WARNING, "No valid client ID."); return 0; }
        std::lock_guard<std::mutex> lock(g_configMutex);
        if (verb == "select") g_config.selected.clear();
        for (const anyID id : ids) {
            auto it = std::find(g_config.selected.begin(), g_config.selected.end(), id);
            if (verb == "remove") {
                if (it != g_config.selected.end()) g_config.selected.erase(it);
            } else if (it == g_config.selected.end()) {
                g_config.selected.push_back(id);
            }
        }
        commandReply(schid, LogLevel_INFO, "Selected targets: " + std::to_string(g_config.selected.size()));
        return 0;
    }
    if (verb == "speed") {
        std::string value; input >> value;
        try {
            std::size_t end = 0; const double rate = std::stod(value, &end);
            if (end != value.size() || !validRate(rate)) throw std::invalid_argument("range");
            std::lock_guard<std::mutex> lock(g_configMutex); g_config.rate = rate;
            commandReply(schid, LogLevel_INFO, "Speed set to " + value + " poke/s.");
        } catch (...) { commandReply(schid, LogLevel_WARNING, "Speed must be a positive finite number."); }
        return 0;
    }
    if (verb == "count") {
        std::string value; input >> value;
        try {
            std::size_t end = 0; const unsigned long long total = std::stoull(value, &end, 10);
            if (end != value.size() || total == 0) throw std::invalid_argument("range");
            std::lock_guard<std::mutex> lock(g_configMutex); g_config.total = static_cast<std::uint64_t>(total);
            commandReply(schid, LogLevel_INFO, "Count set to " + value + ".");
        } catch (...) { commandReply(schid, LogLevel_WARNING, "Count must be a positive uint64 integer."); }
        return 0;
    }
    if (verb == "message") {
        std::string message; std::getline(input, message); message = leftTrim(message);
        if (!validMessage(message)) commandReply(schid, LogLevel_WARNING, "Message must be non-empty and at most 256 UTF-8 bytes.");
        else { std::lock_guard<std::mutex> lock(g_configMutex); g_config.message = message; commandReply(schid, LogLevel_INFO, "Poke message updated."); }
        return 0;
    }
    if (verb == "start") {
        double rate; std::uint64_t total; std::string message;
        { std::lock_guard<std::mutex> lock(g_configMutex); rate = g_config.rate; total = g_config.total; message = g_config.message; }
        std::string error;
        if (g_controller.start(schid, configuredTargets(), rate, total, std::move(message), &error))
            commandReply(schid, LogLevel_INFO, "PokeWave start accepted.");
        else commandReply(schid, LogLevel_WARNING, "Start failed: " + error);
        return 0;
    }
    if (verb == "stop") { g_controller.stop(); commandReply(schid, LogLevel_INFO, "PokeWave stop requested."); return 0; }
    if (verb == "status") { commandReply(schid, LogLevel_INFO, statusText()); return 0; }
    commandReply(schid, LogLevel_WARNING, "Unknown command: " + verb + ". Use /pokewave help."); return 0;
}
}
