// RSTune - shared memory endpoint and stream registry.
//
// Both input paths register here: RSAsioAudioClient for ASIO devices and
// DebugWrapperCaptureClient for real WASAPI devices. The registry owns the single
// telemetry block so the GUI sees one consistent picture regardless of which path
// the game ended up using.
//
// Reads on the audio threads go through a seqlock and never block, so a stalled or
// crashed GUI cannot glitch the audio.
#pragma once

#include "RSTuneShared.h"

class RSTuneShm
{
public:
	static RSTuneShm& Get();

	bool IsValid() const { return m_shared != nullptr; }

	// Last consistently read control block. If the GUI was mid-write the previous
	// values are reused rather than blocking.
	const RSTuneControl& Control();

	// Returns a slot index, or -1 when there is no room. Not realtime safe; call it
	// from Initialize / GetService, never from a buffer callback.
	int RegisterStream(int path, const wchar_t* label, const wchar_t* id,
	                   float sampleRate, int channels, bool looksLikeMic);
	void UnregisterStream(int slot);

	// Has the user opted this stream out in the GUI?
	bool IsStreamEnabled(int slot);

	// Realtime safe. Updates this stream's own slot and, for the publishing stream,
	// pushes the whole block to shared memory.
	void UpdateStream(int slot, bool shifted, float detectedHz, float addedLatencyMs);
	void PublishGlobal(int slot, float sampleRate, int blockFrames, float inPeakDb,
	                   float outPeakDb, float detectedHz, float addedLatencyMs, float cpuPercent);

private:
	RSTuneShm();
	~RSTuneShm();
	RSTuneShm(const RSTuneShm&) = delete;

	void* m_mapping = nullptr;
	RSTuneShared* m_shared = nullptr;
	RSTuneControl m_cached{};

	// staging copy; each stream only ever touches its own slot
	RSTuneTelemetry m_staging{};
	int m_publisher = -1;
	std::mutex m_registryMutex;
};
