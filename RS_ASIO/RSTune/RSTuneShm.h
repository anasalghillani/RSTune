// RSTune - shared memory endpoint used by the audio thread inside RS_ASIO.dll.
//
// The mapping is created lazily by whichever of RS_ASIO.dll and RSTune.exe starts
// first. Reads on the audio thread go through a seqlock and never block, so a stalled
// or crashed GUI cannot glitch the audio.
#pragma once

#include "RSTuneShared.h"

class RSTuneShm
{
public:
	static RSTuneShm& Get();

	bool IsValid() const { return m_shared != nullptr; }

	// Returns the last consistently read control block. If the GUI happened to be
	// mid-write, the previous values are reused rather than blocking.
	const RSTuneControl& Control();

	void PublishTelemetry(const RSTuneTelemetry& t);

	// Only one audio client should publish telemetry; the first input client to ask
	// wins and keeps the claim for the lifetime of the process.
	bool ClaimTelemetry(const void* owner);

private:
	RSTuneShm();
	~RSTuneShm();
	RSTuneShm(const RSTuneShm&) = delete;

	void* m_mapping = nullptr;
	RSTuneShared* m_shared = nullptr;
	RSTuneControl m_cached{};
	const void* m_telemetryOwner = nullptr;
};
