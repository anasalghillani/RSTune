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

// ---------------------------------------------------------------------------
// Working out what a shift lands on
//
// With a semitone slider the shift is whatever the user picked, and it is uniform by
// definition, so nothing needs filtering. The current tuning is only used to tell them
// what they will end up sounding like.
// ---------------------------------------------------------------------------

// MIDI note numbers of E standard, low string first: E2 A2 D3 G3 B3 E4
static const int kRSBaseNotes[6] = { 40, 45, 50, 55, 59, 64 };

static const wchar_t* kRSNoteNames[12] =
{
	L"C", L"C#", L"D", L"D#", L"E", L"F", L"F#", L"G", L"G#", L"A", L"A#", L"B"
};

// If the shifted tuning is one we have a name for, returns its index, otherwise -1.
// Named tunings carry the spelling guitarists actually write (Eb rather than D#), which
// is why it is worth looking them up instead of always computing note names.
inline int RSTuningFindShifted(int fromIndex, int shift)
{
	if (fromIndex < 0 || fromIndex >= kNumRSTunings)
		return -1;

	for (int i = 0; i < kNumRSTunings; ++i)
	{
		bool match = true;
		for (int s = 0; s < 6 && match; ++s)
		{
			if (kRSTunings[i].offsets[s] != kRSTunings[fromIndex].offsets[s] + shift)
				match = false;
		}
		if (match)
			return i;
	}
	return -1;
}

// Fallback spelling for shifts that do not land on a named tuning, e.g. "F A# D# G# C F".
inline void RSTuningNoteNames(int fromIndex, int shift, wchar_t* out, int cap)
{
	if (!out || cap <= 0) return;
	out[0] = 0;
	if (fromIndex < 0 || fromIndex >= kNumRSTunings) return;

	int len = 0;
	for (int s = 0; s < 6; ++s)
	{
		int n = kRSBaseNotes[s] + kRSTunings[fromIndex].offsets[s] + shift;
		while (n < 0) n += 12;
		const wchar_t* name = kRSNoteNames[n % 12];

		for (int k = 0; name[k] && len < cap - 2; ++k)
			out[len++] = name[k];
		if (s < 5 && len < cap - 2)
			out[len++] = L' ';
	}
	out[len] = 0;
}
