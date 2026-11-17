#pragma once
#include <stddef.h>

typedef struct {
    int bundled;          // all 4 plugin files available in app0:data/plugins/ or ux0:data/
    int files_installed;  // all 4 plugin files present in ur0:tai/
    int config_installed; // *KERNEL, *main, *ALL entries present in ur0:tai/config.txt
    int core_active;      // background plugin IPC detected in SceShell
} PluginStatus;

void plugin_get_status(PluginStatus *out);
int  plugin_install(char *errbuf, size_t errlen);
int  plugin_uninstall(char *errbuf, size_t errlen);
void plugin_request_reboot(void);
