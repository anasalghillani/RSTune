// RSTune - tuning table.
//
// Tunings are stored exactly the way Rocksmith stores them: six per-string offsets in
// semitones relative to E Standard (E2 A2 D3 G3 B3 E4), low string first.
//
// RSTune only performs a *uniform* pitch shift, so a conversion is only offered when
// target[i] - current[i] is the same for all six strings. That covers every
// standard->standard and drop->drop conversion, and correctly excludes things like
// E Standard -> Drop D which would need the low string shifted on its own.
#pragma once

struct RSTuning
{
	const wchar_t* name;
	const wchar_t* notes;
	int offsets[6];
};

static const RSTuning kRSTunings[] =
{
	// --- standard family -------------------------------------------------
	{ L"E Standard",   L"E A D G B E",       {  0,  0,  0,  0,  0,  0 } },
	{ L"Eb Standard",  L"Eb Ab Db Gb Bb Eb", { -1, -1, -1, -1, -1, -1 } },
	{ L"D Standard",   L"D G C F A D",       { -2, -2, -2, -2, -2, -2 } },
	{ L"C# Standard",  L"C# F# B E G# C#",   { -3, -3, -3, -3, -3, -3 } },
	{ L"C Standard",   L"C F Bb Eb G C",     { -4, -4, -4, -4, -4, -4 } },
	{ L"B Standard",   L"B E A D F# B",      { -5, -5, -5, -5, -5, -5 } },
	{ L"Bb Standard",  L"Bb Eb Ab Db F Bb",  { -6, -6, -6, -6, -6, -6 } },
	{ L"A Standard",   L"A D G C E A",       { -7, -7, -7, -7, -7, -7 } },

	// --- drop family -----------------------------------------------------
	{ L"Drop D",       L"D A D G B E",       { -2,  0,  0,  0,  0,  0 } },
	{ L"Drop C#",      L"C# G# C# F# A# D#", { -3, -1, -1, -1, -1, -1 } },
	{ L"Drop C",       L"C G C F A D",       { -4, -2, -2, -2, -2, -2 } },
	{ L"Drop B",       L"B F# B E G# C#",    { -5, -3, -3, -3, -3, -3 } },
	{ L"Drop Bb",      L"Bb F Bb Eb G C",    { -6, -4, -4, -4, -4, -4 } },
	{ L"Drop A",       L"A E A D F# B",      { -7, -5, -5, -5, -5, -5 } },
	{ L"Drop G",       L"G D G C E A",       { -9, -7, -7, -7, -7, -7 } },
};

static const int kNumRSTunings = (int)(sizeof(kRSTunings) / sizeof(kRSTunings[0]));

// If every string moves by the same number of semitones, writes that number to
// outSemitones and returns true. Otherwise the conversion needs per-string shifting,
// which RSTune deliberately does not attempt, and this returns false.
inline bool RSTuningDelta(int fromIndex, int toIndex, int* outSemitones)
{
	if (fromIndex < 0 || fromIndex >= kNumRSTunings) return false;
	if (toIndex   < 0 || toIndex   >= kNumRSTunings) return false;

	const int* a = kRSTunings[fromIndex].offsets;
	const int* b = kRSTunings[toIndex].offsets;
	const int delta = b[0] - a[0];

	for (int i = 1; i < 6; ++i)
	{
		if (b[i] - a[i] != delta)
			return false;
	}

	if (outSemitones)
		*outSemitones = delta;
	return true;
}
