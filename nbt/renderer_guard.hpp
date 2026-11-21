#ifndef NBT_CARLIFE_RENDERER_GUARD_HPP
#define NBT_CARLIFE_RENDERER_GUARD_HPP

#include "car_config.hpp"
#include "display_commands.hpp"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

namespace carlife {

const char ProjectionSurface[] = "1284";

bool beginFactoryBorrow();
void factoryRestored();

inline bool setProjectionVisibility(bool visible) {
    // Expose the destination before hiding the old surface, so failure leaves a visible fallback.
    const char* projection = ProjectionSurface;
    const char* native = "256";
    const char* tool = "/opt/sys/bin/LayerManagerControl";
    char command[256];
    snprintf(command, sizeof(command), "%s set surface %s visibility 1 && %s set surface %s visibility 0",
             tool, visible ? projection : native, tool, visible ? native : projection);
    return displayRun(command) == 0;
}

inline void configureDisplaySize(const char* executable) {
    const char* override = getenv("NBT_DISPLAY_SIZE");
    if (override && *override) return;
    // The CarPlay coding can describe a smaller picture than the panel.
    int width = 1280, height = 480;
    FILE* query = displayCaptureProgram(executable, "--panel-size");
    if (query) {
        char line[256];
        if (fgets(line, sizeof(line), query)) {
            int w, h;
            if (sscanf(line, "%dx%d", &w, &h) == 2 && nbt::carcfg_detail::sane(w, h)) {
                width = w;
                height = h;
            }
        }
        fclose(query);
    }
    char value[32];
    snprintf(value, sizeof(value), "%dx%d", width, height);
    setenv("NBT_DISPLAY_SIZE", value, 1);
}

inline bool liveDisplayRequested() {
    const char* option = getenv("NBT_CARLIFE_LIVE_DISPLAY");
    return option && !strcmp(option, "1");
}

// Written after negotiation and before starting any screen or input thread.
extern int nativeVideoWidth;

inline int videoWidth() { return liveDisplayRequested() ? nativeVideoWidth : nbt::videoCanvasWidth(); }
inline int videoHeight() { return liveDisplayRequested() ? 480 : nbt::videoCanvasHeight(); }
inline int outputWidth() { return liveDisplayRequested() ? videoWidth() : nbt::mmrDstWidth(); }
inline int outputHeight() { return liveDisplayRequested() ? videoHeight() : nbt::mmrDstHeight(); }

inline int outputX() {
    if (!liveDisplayRequested()) return 0;
    const int spare = nbt::carConfig().screenW - videoWidth();
    return spare > 0 ? (spare / 2) & ~1 : 0;
}

inline int outputY() {
    if (!liveDisplayRequested()) return 0;
    const int spare = nbt::carConfig().screenH - videoHeight();
    return spare > 0 ? (spare / 2) & ~1 : 0;
}

inline int factoryVisibility() {
    FILE* query = displayCapture("/opt/sys/bin/LayerManagerControl get surface 1280 2>/dev/null");
    if (!query) return -1;
    char line[512];
    int visible = -1;
    while (fgets(line, sizeof(line), query)) {
        int value;
        if (sscanf(line, "- visibility: %d", &value) == 1) visible = value;
    }
    fclose(query);
    return visible;
}

inline bool factoryProjectionHidden() {
    int visible = factoryVisibility();
    if (visible < 0) {
        usleep(350000);
        visible = factoryVisibility();
    }
    return visible == 0;
}

inline bool rendererAvailable(std::string* factory = NULL) {
    DIR* contexts = opendir("/pps/services/multimedia/renderer/context");
    if (!contexts) {
        puts("Cannot check MM-Renderer contexts; leaving the display alone.");
        return false;
    }
    bool available = true;
    bool factory_found = false;
    while (struct dirent* entry = readdir(contexts)) {
        const char* name = entry->d_name;
        if (!strncmp(name, "cpvideoctx", 10) && liveDisplayRequested() && !factory_found) {
            factory_found = true;
            if (factory) factory->assign(name);
            continue;
        }
        if (!strncmp(name, "cpvideoctx", 10) || !strncmp(name, "aadvideoctx", 11) ||
            !strncmp(name, "clvideoctx", 10)) {
            printf("MM-Renderer context %s already exists; leaving it alone.\n", name);
            available = false;
            break;
        }
    }
    closedir(contexts);
    return available;
}

}

#endif
