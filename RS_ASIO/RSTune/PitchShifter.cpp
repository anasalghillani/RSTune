#include "stdafx.h"
#include <cmath>

#include "PitchShifter.h"
#include "RSTuneShared.h"

namespace
{
	const float kGateLinear = 0.0056f;   // about -45 dBFS
	const float kOnsetRatio = 2.5f;
	const double kPi = 3.14159265358979323846;
}

void PitchShifter::Init(double sampleRate)
{
	m_sampleRate = (sampleRate > 8000.0) ? sampleRate : 48000.0;

	m_buf.assign(kBufSize, 0.0f);
	m_ana.assign(kAnaSize, 0.0f);
	m_anaLin.assign(kAnaSize, 0.0f);
	m_nsdf.assign(kMaxLag + 2, 0.0f);
	m_prefixSq.assign(kAnaSize + 1, 0.0f);

	// sin(pi * x) over one full phase cycle, with one extra entry so the linear
	// interpolation at the top of the table does not need a wrap test
	m_winSin.assign(kWinSize + 1, 0.0f);
	for (int i = 0; i <= kWinSize; ++i)
		m_winSin[i] = (float)std::sin(kPi * (double)i / (double)kWinSize);

	m_fadeStep = 1.0f / 64.0f;
	m_wetStep = (float)(1.0 / (0.020 * m_sampleRate));   // 20 ms engage/bypass ramp

	SetQuality(RSTuneQuality_Balanced);
	Reset();
}

void PitchShifter::Reset()
{
	std::fill(m_buf.begin(), m_buf.end(), 0.0f);
	std::fill(m_ana.begin(), m_ana.end(), 0.0f);

	m_write = 0;
	m_phase = 0.0;
	m_grain = m_halfDefault * 2;
	m_pendingGrain = m_grain;
	m_grainSwitchPending = false;

	m_ratio = m_ratioTarget;
	m_wet = m_wetTarget;

	m_fadeDir = 0;
	m_fadeGain = 1.0f;

	m_dcX1 = m_dcY1 = 0.0f;
	m_lpA = m_lpB = 0.0f;
	m_decimCount = 0;
	m_anaWrite = 0;
	m_sinceAnalyse = 0;
	m_detectedHz = 0.0f;
	m_confidence = 0.0f;
	m_periodSamples = 0;
	m_envFast = m_envSlow = 0.0f;
}

void PitchShifter::SetRatio(float ratio)
{
	if (!(ratio > 0.2f && ratio < 4.0f))
		ratio = 1.0f;

	m_ratioTarget = ratio;

	// a ratio of exactly 1 means pass the signal through untouched
	m_wetTarget = (std::fabs(ratio - 1.0f) > 1.0e-5f) ? 1.0f : 0.0f;
}

void PitchShifter::SetQuality(int quality)
{
	if (quality == m_quality)
		return;
	m_quality = quality;

	// bounds are expressed at 48 kHz and scaled to the running rate.
	// half a grain is also the mean latency the shifter adds.
	const double s = m_sampleRate / 48000.0;
	switch (quality)
	{
	case RSTuneQuality_Tight:
		m_halfMin = (int)(160 * s); m_halfMax = (int)(800 * s);  m_halfDefault = (int)(288 * s);
		break;
	case RSTuneQuality_Smooth:
		m_halfMin = (int)(384 * s); m_halfMax = (int)(2000 * s); m_halfDefault = (int)(800 * s);
		break;
	case RSTuneQuality_Balanced:
	default:
		m_halfMin = (int)(256 * s); m_halfMax = (int)(1400 * s); m_halfDefault = (int)(480 * s);
		break;
	}

	m_pendingGrain = m_halfDefault * 2;
	m_grainSwitchPending = (m_pendingGrain != m_grain);
}

float PitchShifter::WinSin(double phase) const
{
	double f = phase * (double)kWinSize;
	int i = (int)f;
	if (i < 0) { i = 0; f = 0.0; }
	else if (i >= kWinSize) { i = kWinSize - 1; f = (double)kWinSize; }
	const float frac = (float)(f - (double)i);
	const float a = m_winSin[i];
	return a + (m_winSin[i + 1] - a) * frac;
}

float PitchShifter::ReadHermite(double delay) const
{
	const double pos = (double)m_write - delay;
	const double fi = std::floor(pos);
	const float frac = (float)(pos - fi);
	const unsigned i0 = (unsigned)(long long)fi;

	const float ym1 = m_buf[(i0 - 1) & kBufMask];
	const float y0  = m_buf[(i0)     & kBufMask];
	const float y1  = m_buf[(i0 + 1) & kBufMask];
	const float y2  = m_buf[(i0 + 2) & kBufMask];

	const float c0 = y0;
	const float c1 = 0.5f * (y1 - ym1);
	const float c2 = ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
	const float c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);

	return ((c3 * frac + c2) * frac + c1) * frac + c0;
}

void PitchShifter::ApplyPendingGrain()
{
	if (m_pendingGrain < 64) m_pendingGrain = 64;
	if (m_pendingGrain > kBufSize / 2) m_pendingGrain = kBufSize / 2;
	m_grain = m_pendingGrain;
	m_grainSwitchPending = false;
}

void PitchShifter::Process(float* io, int numSamples)
{
	if (numSamples <= 0)
		return;

	// smooth the ratio once per block; inside a block it is constant, which keeps the
	// phase increment out of the per-sample critical path
	{
		const float coef = (float)(1.0 - std::exp(-(double)numSamples / (0.010 * m_sampleRate)));
		m_ratio += (m_ratioTarget - m_ratio) * coef;
		if (std::fabs(m_ratioTarget - m_ratio) < 1.0e-6f)
			m_ratio = m_ratioTarget;
	}

	const float dcR        = 1.0f - (float)(2.0 * kPi * 20.0 / m_sampleRate);
	const float lpCoef     = (float)(1.0 - std::exp(-2.0 * kPi * 2500.0 / m_sampleRate));
	const float envFastCoef= (float)(1.0 - std::exp(-1.0 / (0.003 * m_sampleRate)));
	const float envSlowCoef= (float)(1.0 - std::exp(-1.0 / (0.120 * m_sampleRate)));

	for (int n = 0; n < numSamples; ++n)
	{
		const float dry = io[n];

		// a dc offset would put a step at every grain splice
		const float x = dry - m_dcX1 + dcR * m_dcY1;
		m_dcX1 = dry;
		m_dcY1 = x;

		m_buf[m_write & kBufMask] = x;

		const float mag = std::fabs(x);
		m_envFast += (mag - m_envFast) * envFastCoef;
		m_envSlow += (mag - m_envSlow) * envSlowCoef;

		// decimate by 4 into the pitch analysis ring
		m_lpA += (x - m_lpA) * lpCoef;
		m_lpB += (m_lpA - m_lpB) * lpCoef;
		if (++m_decimCount >= kDecim)
		{
			m_decimCount = 0;
			m_ana[m_anaWrite & kAnaMask] = m_lpB;
			++m_anaWrite;
			++m_sinceAnalyse;
		}

		// changing grain length moves both read heads, so only do it behind a fade and
		// only where the 64-sample dip is masked by silence or a fresh transient
		if (m_fadeDir == 0 && m_grainSwitchPending)
		{
			const bool quiet = m_envFast < kGateLinear;
			const bool onset = (m_envFast > kGateLinear) && (m_envFast > m_envSlow * kOnsetRatio);
			if (quiet || onset)
				m_fadeDir = -1;
		}
		if (m_fadeDir < 0)
		{
			m_fadeGain -= m_fadeStep;
			if (m_fadeGain <= 0.0f) { m_fadeGain = 0.0f; ApplyPendingGrain(); m_fadeDir = 1; }
		}
		else if (m_fadeDir > 0)
		{
			m_fadeGain += m_fadeStep;
			if (m_fadeGain >= 1.0f) { m_fadeGain = 1.0f; m_fadeDir = 0; }
		}

		// the read heads move at 'ratio' samples per output sample while the write head
		// moves at 1, which is where the shift comes from
		const double dphase = (1.0 - (double)m_ratio) / (double)m_grain;
		m_phase += dphase;
		if (m_phase >= 1.0) m_phase -= 1.0;
		else if (m_phase < 0.0) m_phase += 1.0;

		if (m_wet < m_wetTarget) { m_wet += m_wetStep; if (m_wet > m_wetTarget) m_wet = m_wetTarget; }
		else if (m_wet > m_wetTarget) { m_wet -= m_wetStep; if (m_wet < m_wetTarget) m_wet = m_wetTarget; }

		float y = dry;
		if (m_wet > 0.0001f)
		{
			double p2 = m_phase + 0.5;
			if (p2 >= 1.0) p2 -= 1.0;

			const float w1 = WinSin(m_phase);
			const float w2 = WinSin(p2);
			const float s1 = ReadHermite((double)kMinDelay + m_phase * (double)m_grain);
			const float s2 = ReadHermite((double)kMinDelay + p2 * (double)m_grain);

			const float wetSig = (w1 * s1 + w2 * s2) * m_fadeGain;
			y = m_wet * wetSig + (1.0f - m_wet) * dry;
		}

		io[n] = y;
		++m_write;
	}

	// one pitch estimate per ~10 ms of audio
	if (m_sinceAnalyse >= 128)
	{
		m_sinceAnalyse = 0;
		AnalysePitch();
		ChooseGrain();
	}
}

// Normalised square difference over the decimated signal. Fills in m_detectedHz,
// m_periodSamples and a 0..1 m_confidence.
void PitchShifter::AnalysePitch()
{
	const int start = m_anaWrite & kAnaMask;
	for (int i = 0; i < kAnaSize; ++i)
		m_anaLin[i] = m_ana[(start + i) & kAnaMask];

	const int W = kAnaSize;
	const int N = W - kMaxLag;      // samples compared at every lag

	// prefix sums of squares give the lagged energy without a second pass
	m_prefixSq[0] = 0.0f;
	for (int i = 0; i < W; ++i)
		m_prefixSq[i + 1] = m_prefixSq[i] + m_anaLin[i] * m_anaLin[i];

	const float e0 = m_prefixSq[N] - m_prefixSq[0];
	if (e0 < 1.0e-7f)
	{
		m_confidence = 0.0f;
		m_detectedHz = 0.0f;
		m_periodSamples = 0;
		return;
	}

	float best = 0.0f;
	int   bestGlobalLag = 0;
	for (int lag = kMinLag; lag <= kMaxLag; ++lag)
	{
		float r = 0.0f;
		const float* a = &m_anaLin[0];
		const float* b = &m_anaLin[lag];
		for (int i = 0; i < N; ++i)
			r += a[i] * b[i];

		const float e1 = m_prefixSq[lag + N] - m_prefixSq[lag];
		const float denom = e0 + e1;
		const float nv = (denom > 1.0e-7f) ? (2.0f * r / denom) : 0.0f;
		m_nsdf[lag] = nv;
		if (nv > best)
		{
			best = nv;
			bestGlobalLag = lag;
		}
	}

	// take the first peak that gets close to the global maximum. taking the global max
	// outright tends to land an octave low, which would double the grain length.
	int bestLag = 0;
	if (best > 0.0f)
	{
		const float threshold = 0.85f * best;
		for (int lag = kMinLag + 1; lag < kMaxLag; ++lag)
		{
			if (m_nsdf[lag] >= threshold &&
			    m_nsdf[lag] >= m_nsdf[lag - 1] &&
			    m_nsdf[lag] >= m_nsdf[lag + 1])
			{
				bestLag = lag;
				break;
			}
	}

		// If the correlation is still climbing at the top of the lag range there is no
		// interior local maximum to find. Falling through with bestLag = 0 would throw
		// away a perfectly confident estimate and drop us onto an unaligned default
		// grain, which is what wrecked low power chords. Use the global argmax instead.
		if (bestLag == 0)
			bestLag = bestGlobalLag;
	}

	if (bestLag <= kMinLag || bestLag >= kMaxLag || best < 0.5f)
	{
		m_confidence = best;
		m_detectedHz = 0.0f;
		m_periodSamples = 0;
		return;
	}

	double lagF = (double)bestLag;
	{
		const float y0 = m_nsdf[bestLag - 1];
		const float y1 = m_nsdf[bestLag];
		const float y2 = m_nsdf[bestLag + 1];
		const float denom = 2.0f * (2.0f * y1 - y0 - y2);
		if (std::fabs(denom) > 1.0e-9f)
		{
			const double d = (double)(y2 - y0) / (double)denom;
			if (d > -1.0 && d < 1.0)
				lagF += d;
		}
	}

	m_confidence = m_nsdf[bestLag];
	const double periodFull = lagF * (double)kDecim;
	m_periodSamples = (int)(periodFull + 0.5);
	m_detectedHz = (float)(m_sampleRate / periodFull);
}

// Size the grain so half of it spans a whole number of fundamental periods. The two
// read heads then sit an exact number of periods apart and reinforce each other
// instead of comb filtering.
//
// Alignment is not optional: measurements showed that as soon as the grain is capped
// below one period of the note being played, the two heads comb and the pitch the game
// would detect drifts tens of cents flat. A low note therefore *requires* a long grain,
// and the quality presets can only choose how many periods to use, never fewer than one.
void PitchShifter::ChooseGrain()
{
	int half;

	if (m_confidence > 0.55f && m_periodSamples >= 16)
	{
		int k = 1;
		if (m_periodSamples < m_halfMin)
			k = (m_halfMin + m_periodSamples - 1) / m_periodSamples;

		half = k * m_periodSamples;

		// absolute safety ceiling, only reachable for implausibly low pitches
		if (half > m_halfMax)
			half = m_halfMax;
	}
	else
	{
		// no reliable fundamental, e.g. a dense chord or a muted scrape
		half = m_halfDefault;
	}

	const int grain = half * 2;

	// hysteresis, otherwise vibrato would have us re-splicing constantly
	if (std::abs(grain - m_grain) > 48)
	{
		m_pendingGrain = grain;
		m_grainSwitchPending = true;
	}
}
