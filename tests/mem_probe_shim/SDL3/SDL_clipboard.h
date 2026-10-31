// Stand-in for SDL3 in tests/mem_probe.py: the one declaration platform/linux.c takes from it.
#pragma once
#include <stdbool.h>
bool SDL_SetClipboardText(const char *text);
