// RSTune - shared memory contract between RS_ASIO.dll (audio threads) and RSTune.exe.
// Compiled into the DLL, the GUI and the offline harness, so it must not depend on
// anything beyond <stdint.h> and windows.h.
#pragma once

#include <stdint.h>

#define RSTUNE_SHM_NAME   L"RSTune_v1_shm"   // session namespace: game and GUI share a session
#define RSTUNE_MAGIC      0x554E5452u        // 'RTNU'
#define RSTUNE_VERSION    2

// Rocksmith opens at most a couple of capture streams, but leave room.
#define RSTUNE_MAX_STREAMS 8
#define RSTUNE_LABEL_LEN   64
#define RSTUNE_ID_LEN      96

// Larger grains sound smoother on chords but add latency and onset jitter.
enum RSTuneQuality
{
	RSTuneQuality_Tight = 0,
	RSTuneQuality_Balanced = 1,
	RSTuneQuality_Smooth = 2,
	RSTuneQuality_Count
};

// Which of RS_ASIO's two input implementations a stream came through.
enum RSTunePath
{
	RSTunePath_Asio = 0,
	RSTunePath_Wasapi = 1,
};

#pragma pack(push, 8)

// One capture stream the game has opened. Published by the DLL so the GUI can list
// what is actually in play rather than guessing from a device enumeration.
struct RSTuneStreamInfo
{
	int32_t active;         // the game currently holds this stream open
	int32_t path;           // RSTunePath
	int32_t looksLikeMic;   // best effort hint only, never authoritative
	int32_t shifted;        // the shifter processed audio for it on the last block
	float   sampleRate;
	int32_t channels;
	float   detectedHz;
	float   addedLatencyMs;
	wchar_t label[RSTUNE_LABEL_LEN];
	wchar_t id[RSTUNE_ID_LEN];
};

// Relevant parts of the game's own Rocksmith.ini, read by the DLL and surfaced in the
// GUI so a misconfigured audio path explains itself instead of looking like a bug.
struct RSTuneGameConfig
{
	int32_t valid;
	int32_t enableMicrophone;
	int32_t exclusiveMode;
	int32_t latencyBuffer;
	int32_t maxOutputBufferSize;
	int32_t forceWDM;
	int32_t forceDirectXSink;
	int32_t realToneCableOnly;
	int32_t win32UltraLowLatencyMode;
};

// Written only by the GUI, read only by the audio threads.
struct RSTuneControl
{
	int32_t enabled;
	int32_t semitones;
	float   cents;
	int32_t quality;
	// Per stream opt out, indexed the same way as RSTuneTelemetry::streams.
	// Defaults to 1; the GUI clears it for anything the user does not want shifted.
	int32_t streamEnabled[RSTUNE_MAX_STREAMS];
};

// Written only by the audio threads, read only by the GUI.
struct RSTuneTelemetry
{
	int32_t heartbeat;      // increments every audio block; stalls when the game is closed
	int32_t streamActive;   // at least one input stream is running
	float   sampleRate;
	int32_t blockFrames;
	float   inputPeakDb;
	float   outputPeakDb;
	float   detectedHz;
	float   addedLatencyMs;
	float   cpuPercent;
	int32_t numStreams;
	RSTuneStreamInfo streams[RSTUNE_MAX_STREAMS];
	RSTuneGameConfig gameConfig;
};

struct RSTuneShared
{
	uint32_t magic;
	uint32_t version;

	volatile uint32_t ctlSeq;
	RSTuneControl control;

	volatile uint32_t telSeq;
	RSTuneTelemetry telemetry;
};

#pragma pack(pop)

// ---------------------------------------------------------------------------
// Seqlock helpers. The writer bumps the counter to an odd value, writes, then bumps
// it to the next even value. A reader retries while it saw an odd count or the count
// moved underneath it. Readers never block writers, which is what keeps the audio
// threads free of locks.
// ---------------------------------------------------------------------------

#ifdef __cplusplus
#include <atomic>

template<typename T>
inline void RSTuneSeqWrite(volatile uint32_t& seq, T& dst, const T& src)
{
	std::atomic_thread_fence(std::memory_order_acquire);
	const uint32_t s = seq;
	seq = s + 1;
	std::atomic_thread_fence(std::memory_order_release);
	dst = src;
	std::atomic_thread_fence(std::memory_order_release);
	seq = s + 2;
}

// Returns false if the value could not be read consistently within the retry budget.
template<typename T>
inline bool RSTuneSeqRead(const volatile uint32_t& seq, const T& src, T& dst, int retries = 8)
{
	for (int i = 0; i < retries; ++i)
	{
		const uint32_t s0 = seq;
		if (s0 & 1u)
			continue;
		std::atomic_thread_fence(std::memory_order_acquire);
		dst = src;
		std::atomic_thread_fence(std::memory_order_acquire);
		if (seq == s0)
			return true;
	}
	return false;
}
#endif
