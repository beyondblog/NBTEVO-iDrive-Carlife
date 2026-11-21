#ifndef CARLIFE_DISPLAY_COMMANDS_HPP
#define CARLIFE_DISPLAY_COMMANDS_HPP

#include <stdio.h>

namespace carlife {

int displayRun(const char* command);
// Returns completed output positioned at the beginning. Close it with fclose().
FILE* displayCapture(const char* command);
FILE* displayCaptureProgram(const char* executable, const char* option);

}

#endif
