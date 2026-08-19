// RSTune - shifts one interleaved audio packet.
//
// Split out of WasapiShifter so the risky part, format conversion and interleaving over
// variable sized packets, can be exercised by the offline harness without any COM,
// shared memory or a running game.
//
// The caller's buffer is never written to; a processed copy is produced in memory this
// object owns. Everything is preallocated in Init.
#pragma once

#include <vector>

#include "PitchShifter.h"

class PacketShifter
{
public:
	// maxFrames is the largest packet that will be processed. Larger packets are
	// refused rather than allocating on a realtime thread.
	bool Init(const WAVEFORMATEX* fmt, unsigned maxFrames);
	void Reset();

	bool IsActive() const { return m_valid; }
	unsigned Channels() const { return m_channels; }
	double   SampleRate() const { return m_sampleRate; }

	void SetRatio(float ratio);
	void SetQuality(int quality);

	// Returns the processed interleaved buffer, or nullptr when the packet cannot be
	// handled and the caller should pass the original through untouched.
	// 'silent' synthesises silence instead of reading src.
	BYTE* Process(const BYTE* src, unsigned numFrames, bool silent);

	float GetDetectedHz() const;
	float GetLatencySamples() const;
	float GetInputPeak() const  { return m_inPeak; }
	float GetOutputPeak() const { return m_outPeak; }

private:
	bool     m_valid = false;
	unsigned m_channels = 0;
	unsigned m_maxFrames = 0;
	unsigned m_blockAlign = 0;
	double   m_sampleRate = 48000.0;
	ASIOSampleType m_sampleType = ASIOSTInt16LSB;
	WORD     m_sampleBytes = 0;

	float m_inPeak = 0.0f;
	float m_outPeak = 0.0f;

	std::vector<BYTE>  m_out;
	std::vector<float> m_scratch;
	std::vector<PitchShifter> m_shifters;
};
