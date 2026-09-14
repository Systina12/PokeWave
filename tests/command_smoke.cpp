#include "plugin_api.h"
#include "teamspeak/public_errors.h"

#include <cstring>
#include <iostream>
#include <string>

namespace {
std::string lastMessage;

unsigned int smokeLogMessage(const char*, LogLevel, const char*, uint64) {
    return ERROR_ok;
}

void smokePrintMessage(const char* message) {
    lastMessage = message == nullptr ? "" : message;
}
}

int main() {
    TS3Functions functions{};
    functions.logMessage = &smokeLogMessage;
    functions.printMessageToCurrentTab = &smokePrintMessage;
    ts3plugin_setFunctionPointers(functions);

    if (std::strcmp(ts3plugin_commandKeyword(), "pokewave") != 0) {
        std::cerr << "unexpected command keyword\n";
        return 1;
    }
    if (ts3plugin_processCommand(0, "help") != 0) {
        std::cerr << "help was not handled\n";
        return 1;
    }
    if (lastMessage.find("[PokeWave]") == std::string::npos ||
        lastMessage.find("list") == std::string::npos) {
        std::cerr << "help response was not printed to the current tab\n";
        return 1;
    }

    lastMessage.clear();
    if (ts3plugin_processCommand(0, "speed 25") != 0 ||
        lastMessage.find("Speed set to 25") == std::string::npos) {
        std::cerr << "speed response was not printed\n";
        return 1;
    }

    ts3plugin_shutdown();
    return 0;
}
