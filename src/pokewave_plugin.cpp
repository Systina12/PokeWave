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
    logLine(schid, LogLevel_INFO,
            "PokeWave: list | select <id,...> | add <id,...> | remove <id,...> | "
            "speed <positive> | count <positive uint64> | message <text> | start | stop | status");
}
}

extern "C" {
const char* ts3plugin_name() { return "PokeWave"; }
const char* ts3plugin_version() { return "0.2.0"; }
int ts3plugin_apiVersion() { return kApiVersion; }
const char* ts3plugin_author() { return "Local server administrator"; }
const char* ts3plugin_description() { return "Controlled multi-target TeamSpeak poke test tool."; }
void ts3plugin_setFunctionPointers(const struct TS3Functions funcs) { g_ts3 = funcs; }
int ts3plugin_init() { logLine(0, LogLevel_INFO, "PokeWave loaded. No task starts automatically."); return 0; }
void ts3plugin_shutdown() { g_controller.stop(); g_pluginId.clear(); }
int ts3plugin_offersConfigure() { return PLUGIN_OFFERS_NO_CONFIGURE; }
void ts3plugin_configure(void*, void*) { logLine(currentSchid(), LogLevel_INFO, "Use /pokewave help."); }
void ts3plugin_registerPluginID(const char* id) { g_pluginId = id == nullptr ? "" : id; }
int ts3plugin_requestAutoload() { return 1; }

void ts3plugin_initMenus(struct PluginMenuItem*** menuItems, char** menuIcon) {
    if (menuItems == nullptr) return;
    *menuItems = static_cast<PluginMenuItem**>(std::malloc(sizeof(PluginMenuItem*) * 2));
    if (*menuItems == nullptr) return;
    (*menuItems)[0] = static_cast<PluginMenuItem*>(std::malloc(sizeof(PluginMenuItem)));
    if ((*menuItems)[0] == nullptr) { std::free(*menuItems); *menuItems = nullptr; return; }
    (*menuItems)[0]->type = PLUGIN_MENU_TYPE_GLOBAL;
    (*menuItems)[0]->id = kMenuOpen;
    std::strncpy((*menuItems)[0]->text, "PokeWave command help", PLUGIN_MENU_BUFSZ - 1);
    (*menuItems)[0]->text[PLUGIN_MENU_BUFSZ - 1] = '\0';
    (*menuItems)[0]->icon[0] = '\0';
    (*menuItems)[1] = nullptr;
    if (menuIcon != nullptr) *menuIcon = nullptr;
}
void ts3plugin_freeMemory(void* data) { std::free(data); }
void ts3plugin_currentServerConnectionChanged(uint64 serverConnectionHandlerID) {
    if (g_controller.running() && currentSchid() != serverConnectionHandlerID) g_controller.stop();
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
    if (type == PLUGIN_MENU_TYPE_GLOBAL && menuItemID == kMenuOpen) help(schid);
}

int ts3plugin_processCommand(uint64 serverConnectionHandlerID, const char* command) {
    const uint64 schid = serverConnectionHandlerID != 0 ? serverConnectionHandlerID : currentSchid();
    std::istringstream input(command == nullptr ? "" : command);
    std::string verb; input >> verb;
    if (verb.empty() || verb == "help") { help(schid); return 0; }
    if (verb == "list") {
        std::string error; const auto clients = visibleClients(schid, &error);
        if (!error.empty()) logLine(schid, LogLevel_ERROR, "list failed: " + error);
        else {
            for (const auto& target : clients) logLine(schid, LogLevel_INFO, label(target));
            logLine(schid, LogLevel_INFO, "Visible clients: " + std::to_string(clients.size()));
        }
        return 0;
    }
    if (verb == "select" || verb == "add" || verb == "remove") {
        std::string rest; std::getline(input, rest);
        const auto ids = parseIds(rest);
        if (ids.empty()) { logLine(schid, LogLevel_WARNING, "No valid client ID."); return 0; }
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
        logLine(schid, LogLevel_INFO, "Selected targets: " + std::to_string(g_config.selected.size()));
        return 0;
    }
    if (verb == "speed") {
        std::string value; input >> value;
        try {
            std::size_t end = 0; const double rate = std::stod(value, &end);
            if (end != value.size() || !validRate(rate)) throw std::invalid_argument("range");
            std::lock_guard<std::mutex> lock(g_configMutex); g_config.rate = rate;
            logLine(schid, LogLevel_INFO, "Speed set to " + value + " poke/s.");
        } catch (...) { logLine(schid, LogLevel_WARNING, "Speed must be a positive finite number."); }
        return 0;
    }
    if (verb == "count") {
        std::string value; input >> value;
        try {
            std::size_t end = 0; const unsigned long long total = std::stoull(value, &end, 10);
            if (end != value.size() || total == 0) throw std::invalid_argument("range");
            std::lock_guard<std::mutex> lock(g_configMutex); g_config.total = static_cast<std::uint64_t>(total);
            logLine(schid, LogLevel_INFO, "Count set to " + value + ".");
        } catch (...) { logLine(schid, LogLevel_WARNING, "Count must be a positive uint64 integer."); }
        return 0;
    }
    if (verb == "message") {
        std::string message; std::getline(input, message); message = leftTrim(message);
        if (!validMessage(message)) logLine(schid, LogLevel_WARNING, "Message must be non-empty and at most 256 UTF-8 bytes.");
        else { std::lock_guard<std::mutex> lock(g_configMutex); g_config.message = message; logLine(schid, LogLevel_INFO, "Poke message updated."); }
        return 0;
    }
    if (verb == "start") {
        double rate; std::uint64_t total; std::string message;
        { std::lock_guard<std::mutex> lock(g_configMutex); rate = g_config.rate; total = g_config.total; message = g_config.message; }
        std::string error;
        if (g_controller.start(schid, configuredTargets(), rate, total, std::move(message), &error))
            logLine(schid, LogLevel_INFO, "PokeWave start accepted.");
        else logLine(schid, LogLevel_WARNING, "Start failed: " + error);
        return 0;
    }
    if (verb == "stop") { g_controller.stop(); logLine(schid, LogLevel_INFO, "PokeWave stop requested."); return 0; }
    if (verb == "status") { logLine(schid, LogLevel_INFO, statusText()); return 0; }
    help(schid); return 0;
}
}
