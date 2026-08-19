// RSTune - shared memory contract between RS_ASIO.dll (audio thread) and RSTune.exe (GUI).
// This header is compiled into the DLL, the GUI and the offline test harness, so it must stay
// free of any dependency other than <stdint.h> and windows.h.
#pragma once

#include <stdint.h>

#define RSTUNE_SHM_NAME   L"RSTune_v1_shm"   // session namespace: game and GUI share a session
#define RSTUNE_MAGIC      0x554E5452u   // 'RTNU'
#define RSTUNE_VERSION    1

// Quality presets. Larger grains sound smoother on chords but add latency and onset jitter.
enum RSTuneQuality
{
	RSTuneQuality_Tight = 0,   // small grains, lowest latency, a bit grainier
	RSTuneQuality_Balanced = 1,
	RSTuneQuality_Smooth = 2,  // large grains, smoothest, most latency
	RSTuneQuality_Count
};

#pragma pack(push, 8)

// Written only by the GUI, read only by the audio thread.
struct RSTuneControl
{
	int32_t enabled;        // master on/off
	int32_t semitones;      // uniform shift applied to every string, -12..+12
	float   cents;          // extra fine offset, -100..+100
	int32_t quality;        // RSTuneQuality
	int32_t applyInput0;    // process the Asio.Input.0 client
	int32_t applyInput1;    // process the Asio.Input.1 client
};

// Written only by the audio thread, read only by the GUI.
struct RSTuneTelemetry
{
	int32_t heartbeat;      // increments every audio block; stalls if the game is not running
	int32_t streamActive;   // an input client is initialised and started
	float   sampleRate;
	int32_t blockFrames;
	float   inputPeakDb;
	float   outputPeakDb;
	float   detectedHz;     // fundamental of the *incoming* signal, 0 when unvoiced
	float   addedLatencyMs; // mean latency contributed by the shifter
	float   cpuPercent;     // share of the audio block budget spent in the shifter
};

struct RSTuneShared
{
	uint32_t magic;
	uint32_t version;

	// seqlock guarding 'control'
	volatile uint32_t ctlSeq;
	RSTuneControl control;

	// seqlock guarding 'telemetry'
	volatile uint32_t telSeq;
	RSTuneTelemetry telemetry;
};

#pragma pack(pop)

// ---------------------------------------------------------------------------
// Seqlock helpers. The writer bumps the counter to an odd value, writes, then
// bumps it to the next even value. The reader retries while it saw an odd count
// or the count changed underneath it. Readers never block writers, which is what
// keeps the audio thread free of locks.
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
