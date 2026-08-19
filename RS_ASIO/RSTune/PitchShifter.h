// RSTune - real-time monophonic-friendly granular pitch shifter.
//
// Design notes
// ------------
// A two-head granular delay line. Both heads read the same ring buffer at 'ratio'
// samples per output sample while the write head advances at 1, so the read position
// drifts and the pitch is scaled by exactly 'ratio'. The heads sit half a grain apart
// and are crossfaded with sin/cos windows, which sum to unit power.
//
// The audible weakness of this family of shifters is that both heads are always
// contributing, separated by half a grain, so you hear the signal doubled at a fixed
// delay. When that separation is an exact multiple of the fundamental period the two
// copies line up in phase and reinforce instead of comb-filtering. That is what the
// pitch tracker is for: it sizes the grain to a whole number of periods.
//
// Changing grain length moves both read heads discontinuously, so it is only ever done
// while the output is faded out, and only at a note onset or during silence where the
// 64-sample gap is inaudible.
//
// Everything is preallocated in Init(). Process() does no allocation, no locking and
// no system calls, because it runs on the ASIO callback thread.
#pragma once

#include <vector>

class PitchShifter
{
public:
	void  Init(double sampleRate);
	void  Reset();

	// Both are cheap and safe to call every block from the audio thread.
	void  SetRatio(float ratio);      // 2^(semitones/12), 1.0 == no shift
	void  SetQuality(int quality);    // RSTuneQuality

	// In-place, mono, any block size up to what Init was given.
	void  Process(float* io, int numSamples);

	float GetLatencySamples() const { return (float)(kMinDelay + m_grain * 0.5f); }
	float GetDetectedHz() const     { return m_detectedHz; }
	int   GetGrain() const          { return m_grain; }
	float GetConfidence() const     { return m_confidence; }

private:
	void  AnalysePitch();
	void  ChooseGrain();
	float ReadHermite(double delay) const;
	float WinSin(double phase) const;
	void  ApplyPendingGrain();

	static const int kBufBits   = 15;
	static const int kBufSize   = 1 << kBufBits;
	static const int kBufMask   = kBufSize - 1;
	static const int kMinDelay  = 2;      // headroom for the 4-point interpolator
	static const int kWinBits   = 12;
	static const int kWinSize   = 1 << kWinBits;

	// pitch analysis runs on a x4 decimated copy
	static const int kDecim     = 4;
	static const int kAnaSize   = 1024;
	static const int kAnaMask   = kAnaSize - 1;
	static const int kMinLag    = 9;      // ~1333 Hz at 12 kHz
	static const int kMaxLag    = 340;    // ~35 Hz at 12 kHz, low enough to catch the
	                                      // composite period of a low power chord

	double m_sampleRate   = 48000.0;

	// delay line
	std::vector<float> m_buf;
	unsigned m_write      = 0;

	// grain state
	double m_phase        = 0.0;
	int    m_grain        = 960;
	int    m_pendingGrain = 960;

	// windows, tabulated to keep the inner loop free of transcendentals
	std::vector<float> m_winSin;   // sin(pi * x), kWinSize+1 entries


	// ratio smoothing
	float  m_ratio        = 1.0f;
	float  m_ratioTarget  = 1.0f;

	// wet/dry so that engaging and bypassing never clicks
	float  m_wet          = 0.0f;
	float  m_wetTarget    = 0.0f;
	float  m_wetStep      = 0.001f;

	// grain-change fade
	int    m_fadeDir      = 0;     // -1 fading out, +1 fading in, 0 idle
	float  m_fadeGain     = 1.0f;
	float  m_fadeStep     = 1.0f / 64.0f;
	bool   m_grainSwitchPending = false;

	// dc blocker
	float  m_dcX1 = 0.0f, m_dcY1 = 0.0f;

	// decimation + analysis
	float  m_lpA = 0.0f, m_lpB = 0.0f;
	int    m_decimCount   = 0;
	std::vector<float> m_ana;
	std::vector<float> m_anaLin;
	std::vector<float> m_nsdf;
	std::vector<float> m_prefixSq;
	int    m_anaWrite     = 0;
	int    m_sinceAnalyse = 0;
	float  m_detectedHz   = 0.0f;
	float  m_confidence   = 0.0f;
	int    m_periodSamples = 0;    // at full rate, 0 when unvoiced

	// envelopes for onset / silence detection
	float  m_envFast = 0.0f, m_envSlow = 0.0f;

	// quality preset bounds, in samples at the current rate
	int    m_halfMin = 256, m_halfMax = 700, m_halfDefault = 480;
	int    m_quality = -1;
};
