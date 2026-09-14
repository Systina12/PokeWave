#ifndef POKEWAVE_PLUGIN_API_H
#define POKEWAVE_PLUGIN_API_H

#include <cstddef>
#include "plugin_definitions.h"
#include "teamspeak/public_definitions.h"
#include "ts3_functions.h"

#if defined(WIN32) || defined(__WIN32__) || defined(_WIN32)
#define POKEWAVE_EXPORT __declspec(dllexport)
#else
#define POKEWAVE_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif
POKEWAVE_EXPORT const char* ts3plugin_name();
POKEWAVE_EXPORT const char* ts3plugin_version();
POKEWAVE_EXPORT int ts3plugin_apiVersion();
POKEWAVE_EXPORT const char* ts3plugin_author();
POKEWAVE_EXPORT const char* ts3plugin_description();
POKEWAVE_EXPORT void ts3plugin_setFunctionPointers(const struct TS3Functions funcs);
POKEWAVE_EXPORT int ts3plugin_init();
POKEWAVE_EXPORT void ts3plugin_shutdown();
POKEWAVE_EXPORT int ts3plugin_offersConfigure();
POKEWAVE_EXPORT void ts3plugin_configure(void* handle, void* qParentWidget);
POKEWAVE_EXPORT void ts3plugin_registerPluginID(const char* id);
POKEWAVE_EXPORT int ts3plugin_processCommand(uint64 serverConnectionHandlerID, const char* command);
POKEWAVE_EXPORT void ts3plugin_currentServerConnectionChanged(uint64 serverConnectionHandlerID);
POKEWAVE_EXPORT int ts3plugin_requestAutoload();
POKEWAVE_EXPORT void ts3plugin_initMenus(struct PluginMenuItem*** menuItems, char** menuIcon);
POKEWAVE_EXPORT void ts3plugin_freeMemory(void* data);
POKEWAVE_EXPORT void ts3plugin_onConnectStatusChangeEvent(uint64 serverConnectionHandlerID, int newStatus, unsigned int errorNumber);
POKEWAVE_EXPORT void ts3plugin_onMenuItemEvent(uint64 serverConnectionHandlerID, enum PluginMenuType type, int menuItemID, uint64 selectedItemID);
#ifdef __cplusplus
}
#endif
#endif
