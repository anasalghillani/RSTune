// RSTune - reads the parts of the game's own Rocksmith.ini that decide whether the
// shifter can reach the audio at all, and whether a capture stream is likely a mic.
#pragma once

#include "RSTuneShared.h"

// Reads <game>\Rocksmith.ini. Safe to call from a non-realtime path only; the result
// is cached after the first call.
const RSTuneGameConfig& RSTuneGetGameConfig();
