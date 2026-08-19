// Minimal stand-in for the RS_ASIO precompiled header so the DSP sources can be
// compiled into the offline test harness without dragging in COM / WASAPI.
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <vector>
#include <array>
#include <string>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cwchar>
