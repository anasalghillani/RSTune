// Stand-in for the RS_ASIO precompiled header, so the DSP and packet handling sources
// can be built into the offline harness. It deliberately mirrors the real one for the
// declarations those files need, but leaves out initguid.h: the GUIDs are defined by
// the RS_ASIO translation units that are linked in alongside, and defining them twice
// would collide at link time.
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmdeviceapi.h>
#include <combaseapi.h>
#include <Audioclient.h>
#include <ks.h>
#include <ksmedia.h>

#include <vector>
#include <array>
#include <string>
#include <algorithm>
#include <functional>
#include <mutex>
#include <optional>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cwchar>

#include "asio.h"
#include "Utils.h"
