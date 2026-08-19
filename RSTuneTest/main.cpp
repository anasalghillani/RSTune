// RSTune offline harness.
//
// Runs the shifter over synthesised guitar-like material and checks that the output
// fundamental really is the input fundamental times the requested ratio. Pitch is
// measured with an independent autocorrelation at full rate, deliberately not reusing
// the tracker inside PitchShifter, so a bug there cannot hide itself.
#include "stdafx.h"
#include "Wav.h"
#include "PitchShifter.h"
#include "RSTuneShared.h"
#include "Tunings.h"

static const int   kSR = 48000;
static const double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------------------
// synthesis
// ---------------------------------------------------------------------------

// A plucked string: harmonic stack with per-harmonic decay, slight inharmonicity,
// and a short broadband attack. Close enough to a guitar for pitch-tracking purposes.
static void AddPluck(std::vector<float>& out, double f0, double startSec, double lenSec,
                     double amp, unsigned seed)
{
	const int start = (int)(startSec * kSR);
	const int len = (int)(lenSec * kSR);
	const int nHarm = 24;

	unsigned rng = seed * 2654435761u + 1u;
	auto frand = [&rng]() {
		rng = rng * 1664525u + 1013904223u;
		return (float)((rng >> 8) & 0xFFFF) / 32768.0f - 1.0f;
	};

	std::vector<double> phase(nHarm), tau(nHarm), gain(nHarm);
	for (int k = 0; k < nHarm; ++k)
	{
		phase[k] = frand() * kPi;
		gain[k] = 1.0 / std::pow((double)(k + 1), 1.25);
		tau[k] = 1.6 / std::pow((double)(k + 1), 0.55);
	}

	for (int i = 0; i < len; ++i)
	{
		const int idx = start + i;
		if (idx < 0 || idx >= (int)out.size())
			continue;

		const double t = (double)i / kSR;
		double s = 0.0;
		for (int k = 0; k < nHarm; ++k)
		{
			const double B = 0.00008;   // string stiffness
			const double kk = (double)(k + 1);
			const double fk = f0 * kk * std::sqrt(1.0 + B * kk * kk);
			if (fk > kSR * 0.45)
				break;
			s += gain[k] * std::exp(-t / tau[k]) * std::sin(2.0 * kPi * fk * t + phase[k]);
		}

		// pick attack
		if (t < 0.006)
			s += 0.35 * frand() * (1.0 - t / 0.006);

		out[idx] += (float)(amp * s * 0.25);
	}
}

// ---------------------------------------------------------------------------
// independent pitch measurement
// ---------------------------------------------------------------------------
static double MeasureF0(const std::vector<float>& x, int from, int count)
{
	const int minLag = (int)(kSR / 1400.0);
	const int maxLag = (int)(kSR / 60.0);
	const int N = count - maxLag;
	if (N < 1024 || from + count > (int)x.size())
		return 0.0;

	const float* p = &x[from];

	double e0 = 0.0;
	for (int i = 0; i < N; ++i) e0 += (double)p[i] * p[i];
	if (e0 < 1e-9) return 0.0;

	std::vector<double> nsdf(maxLag + 2, 0.0);
	double best = 0.0;
	for (int lag = minLag; lag <= maxLag; ++lag)
	{
		double r = 0.0, e1 = 0.0;
		for (int i = 0; i < N; ++i)
		{
			r += (double)p[i] * p[i + lag];
			e1 += (double)p[i + lag] * p[i + lag];
		}
		const double d = e0 + e1;
		const double n = (d > 1e-12) ? (2.0 * r / d) : 0.0;
		nsdf[lag] = n;
		if (n > best) best = n;
	}
	if (best <= 0.0) return 0.0;

	int bestLag = 0;
	const double thr = 0.85 * best;
	for (int lag = minLag + 1; lag < maxLag; ++lag)
	{
		if (nsdf[lag] >= thr && nsdf[lag] >= nsdf[lag - 1] && nsdf[lag] >= nsdf[lag + 1])
		{
			bestLag = lag;
			break;
		}
	}
	if (bestLag == 0) return 0.0;

	double lagF = bestLag;
	const double den = 2.0 * (2.0 * nsdf[bestLag] - nsdf[bestLag - 1] - nsdf[bestLag + 1]);
	if (std::fabs(den) > 1e-12)
	{
		const double dd = (nsdf[bestLag + 1] - nsdf[bestLag - 1]) / den;
		if (dd > -1.0 && dd < 1.0) lagF += dd;
	}
	return (double)kSR / lagF;
}

// Magnitude of one frequency in a Hann-windowed segment, via a single tuned DFT bin.
static double MagAt(const std::vector<float>& x, int from, int count, double freq)
{
	if (from + count > (int)x.size() || count < 64 || freq <= 0.0)
		return 0.0;

	double re = 0.0, im = 0.0, wsum = 0.0;
	for (int i = 0; i < count; ++i)
	{
		const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * i / (count - 1));
		const double a = 2.0 * kPi * freq * i / kSR;
		re += w * x[from + i] * std::cos(a);
		im -= w * x[from + i] * std::sin(a);
		wsum += w;
	}
	return std::sqrt(re * re + im * im) / wsum;
}

static double Cents(double measured, double expected)
{
	if (measured <= 0.0 || expected <= 0.0) return 9999.0;
	return 1200.0 * std::log2(measured / expected);
}

static void RunShifter(PitchShifter& ps, std::vector<float>& buf, int block)
{
	for (int i = 0; i < (int)buf.size(); i += block)
	{
		const int n = (std::min)(block, (int)buf.size() - i);
		ps.Process(&buf[i], n);
	}
}

static bool HasBadSamples(const std::vector<float>& x)
{
	for (float v : x)
		if (!std::isfinite(v) || std::fabs(v) > 8.0f)
			return true;
	return false;
}

static double PeakAbs(const std::vector<float>& x, int from, int count)
{
	double m = 0.0;
	for (int i = from; i < from + count && i < (int)x.size(); ++i)
		m = (std::max)(m, (double)std::fabs(x[i]));
	return m;
}

int main(int argc, char** argv)
{
	const int block = 128;   // matches the game's ASIO buffer
	int failures = 0;
	int checks = 0;

	struct NoteDef { const char* name; double hz; };
	const NoteDef notes[] = {
		{ "E2 (low E)", 82.41 }, { "A2", 110.00 }, { "D3", 146.83 },
		{ "G3", 196.00 }, { "B3", 246.94 }, { "E4 (high e)", 329.63 },
	};
	const int semis[] = { -1, -2, -3, -4, -5 };
	const char* qnames[] = { "Tight", "Balanced", "Smooth" };

	printf("RSTune DSP harness  (sample rate %d, block %d)\n", kSR, block);
	printf("=========================================================================\n\n");

	// ---- 1. bypass must be bit exact -------------------------------------
	{
		std::vector<float> in((size_t)(kSR * 1.0), 0.0f);
		AddPluck(in, 110.0, 0.05, 0.9, 1.0, 7);
		std::vector<float> out = in;

		PitchShifter ps;
		ps.Init(kSR);
		ps.SetQuality(RSTuneQuality_Balanced);
		ps.SetRatio(1.0f);
		ps.Reset();
		RunShifter(ps, out, block);

		double maxDiff = 0.0;
		for (size_t i = 0; i < in.size(); ++i)
			maxDiff = (std::max)(maxDiff, (double)std::fabs(in[i] - out[i]));

		++checks;
		const bool ok = (maxDiff == 0.0);
		if (!ok) ++failures;
		printf("[%s] bypass at ratio 1.0 is bit exact   (max diff %.3g)\n\n",
		       ok ? "PASS" : "FAIL", maxDiff);
	}

	// ---- 2. pitch accuracy across the fretboard ---------------------------
	for (int qi = 0; qi < 3; ++qi)
	{
		printf("--- quality: %s ---\n", qnames[qi]);
		printf("%-14s %7s  %10s %10s %9s %8s %7s\n",
		       "note", "shift", "expected", "measured", "error", "latency", "peak");

		for (const NoteDef& nd : notes)
		{
			for (int st : semis)
			{
				const float ratio = (float)std::pow(2.0, st / 12.0);
				const double expected = nd.hz * ratio;

				std::vector<float> buf((size_t)(kSR * 2.2), 0.0f);
				AddPluck(buf, nd.hz, 0.05, 2.0, 1.0, (unsigned)(nd.hz * 100) + st);

				PitchShifter ps;
				ps.Init(kSR);
				ps.SetQuality(qi);
				ps.SetRatio(ratio);
				ps.Reset();
				// run up to the middle of the measurement window so the reported latency
				// reflects the grain actually in use while the note is sounding, not the
				// default the shifter falls back to once the note has decayed
				const int from = (int)(0.35 * kSR);
				const int count = (int)(1.0 * kSR);
				const int mid = from + count / 2;
				float latencyMs = 0.0f;
				for (int i = 0; i < (int)buf.size(); i += block)
				{
					const int n = (std::min)(block, (int)buf.size() - i);
					ps.Process(&buf[i], n);
					if (i <= mid && i + block > mid)
						latencyMs = ps.GetLatencySamples() * 1000.0f / kSR;
				}
				const double measured = MeasureF0(buf, from, count);
				const double cents = Cents(measured, expected);

				++checks;
				const bool ok = std::fabs(cents) < 15.0 && !HasBadSamples(buf);
				if (!ok) ++failures;

				printf("%-14s %+4d st  %9.2fHz %9.2fHz %+7.1fc %7.1fms %7.3f  %s\n",
				       nd.name, st, expected, measured, cents,
				       latencyMs,
				       PeakAbs(buf, from, count),
				       ok ? "" : "  <-- FAIL");
			}
		}
		printf("\n");
	}

	// ---- 3. chords -------------------------------------------------------
	// A chord has no single period, so measuring "the" pitch is meaningless. What
	// matters is that every partial moved to its shifted position and that nothing is
	// left behind at the original one, so test that directly with a tuned DFT bin.
	{
		printf("--- chords (Balanced): energy at shifted vs original partial ---\n");

		struct ChordDef { const char* name; double hz[4]; int n; };
		const ChordDef chords[] = {
			{ "E5 power (E2 B2)",       { 82.41, 123.47, 0, 0 }, 2 },
			{ "E5 octave (E2 B2 E3)",   { 82.41, 123.47, 164.81, 0 }, 3 },
			{ "Em open (E2 B2 E3 G3)",  { 82.41, 123.47, 164.81, 196.00 }, 4 },
			{ "D5 power (D3 A3)",       { 146.83, 220.00, 0, 0 }, 2 },
		};

		for (const ChordDef& cd : chords)
		{
			for (int st : { -1, -2, -4 })
			{
				const float ratio = (float)std::pow(2.0, st / 12.0);

				std::vector<float> buf((size_t)(kSR * 2.2), 0.0f);
				for (int i = 0; i < cd.n; ++i)
					AddPluck(buf, cd.hz[i], 0.05 + i * 0.008, 2.0, 0.8, (unsigned)(cd.hz[i] * 10) + st);

				PitchShifter ps;
				ps.Init(kSR);
				ps.SetQuality(RSTuneQuality_Balanced);
				ps.SetRatio(ratio);
				ps.Reset();
				RunShifter(ps, buf, block);

				const int from = (int)(0.35 * kSR);
				const int count = (int)(0.8 * kSR);

				double worstDb = 1e9;
				char detail[256] = {0};
				for (int i = 0; i < cd.n; ++i)
				{
					// skip a probe when another tone's shifted harmonic lands on it, otherwise
					// we would be measuring that harmonic and calling it leftover energy
					bool collides = false;
					for (int j = 0; j < cd.n && !collides; ++j)
						for (int k = 1; k <= 8; ++k)
							if (std::fabs(cd.hz[j] * ratio * k - cd.hz[i]) < 3.0)
								{ collides = true; break; }
					if (collides)
						continue;

					const double magShift = MagAt(buf, from, count, cd.hz[i] * ratio);
					const double magOrig  = MagAt(buf, from, count, cd.hz[i]);
					const double db = 20.0 * std::log10((magShift + 1e-12) / (magOrig + 1e-12));
					worstDb = (std::min)(worstDb, db);
					char one[64];
					sprintf_s(one, " [%.0f:%+.0f]", cd.hz[i], db);
					strcat_s(detail, one);
				}

				++checks;
				const bool ok = worstDb > 12.0 && !HasBadSamples(buf);
				if (!ok) ++failures;

				printf("%-24s %+3d st  worst %+6.1f dB  %s  %s\n",
				       cd.name, st, worstDb, detail, ok ? "ok" : "<-- FAIL");
			}
		}
		printf("\n");
	}

	// ---- 3b. spectrum of one failing chord case --------------------------
	{
		printf("--- spectrum: E5 power chord (E2 82.41 + B2 123.47) shifted -4 st ---\n");
		const float ratio = (float)std::pow(2.0, -4.0 / 12.0);

		std::vector<float> buf((size_t)(kSR * 2.2), 0.0f);
		AddPluck(buf, 82.41, 0.05, 2.0, 0.8, 11);
		AddPluck(buf, 123.47, 0.058, 2.0, 0.8, 12);

		std::vector<float> dry = buf;

		PitchShifter ps;
		ps.Init(kSR);
		ps.SetQuality(RSTuneQuality_Balanced);
		ps.SetRatio(ratio);
		ps.Reset();
		// walk the run so we can watch what the grain tracker decides
		printf("  time  grain  latency  detected  conf\n");
		for (int i = 0, s = 0; i < (int)buf.size(); i += block, ++s)
		{
			const int n = (std::min)(block, (int)buf.size() - i);
			ps.Process(&buf[i], n);
			if (s % 60 == 0 && i < (int)(1.4 * kSR))
				printf("  %4.2fs %6d %6.1fms %8.1fHz %5.2f\n",
				       (double)i / kSR, ps.GetGrain(),
				       ps.GetLatencySamples() * 1000.0f / kSR,
				       ps.GetDetectedHz(), ps.GetConfidence());
		}

		const int from = (int)(0.35 * kSR);
		const int count = (int)(0.8 * kSR);

		printf("   freq     dry      wet     (expect peaks at %.1f and %.1f)\n",
		       82.41 * ratio, 123.47 * ratio);
		// fine scan, reporting only local maxima so the true peak positions show up
		const double sf0 = 45.0, sf1 = 285.0, sdf = 0.25;
		const int nf = (int)((sf1 - sf0) / sdf) + 1;
		std::vector<double> sw(nf), sd(nf);
		for (int i = 0; i < nf; ++i)
		{
			const double f = sf0 + i * sdf;
			sd[i] = 20.0 * std::log10(MagAt(dry, from, count, f) + 1e-12);
			sw[i] = 20.0 * std::log10(MagAt(buf, from, count, f) + 1e-12);
		}
		printf("  DRY peaks: ");
		for (int i = 1; i < nf - 1; ++i)
			if (sd[i] > sd[i - 1] && sd[i] >= sd[i + 1] && sd[i] > -55.0)
				printf("%.1f(%.0f) ", sf0 + i * sdf, sd[i]);
		printf("\n  WET peaks: ");
		for (int i = 1; i < nf - 1; ++i)
			if (sw[i] > sw[i - 1] && sw[i] >= sw[i + 1] && sw[i] > -55.0)
				printf("%.1f(%.0f) ", sf0 + i * sdf, sw[i]);
		printf("\n");
		printf("\n");
	}

	// ---- 4. stability under a live tuning change -------------------------
	{
		std::vector<float> buf((size_t)(kSR * 3.0), 0.0f);
		for (int i = 0; i < 6; ++i)
			AddPluck(buf, 82.41 * std::pow(2.0, i * 5 / 12.0), 0.05 + i * 0.45, 0.5, 1.0, 100u + i);

		PitchShifter ps;
		ps.Init(kSR);
		ps.SetQuality(RSTuneQuality_Balanced);
		ps.SetRatio(1.0f);
		ps.Reset();

		// sweep the shift while audio is flowing, the way the GUI would
		const int steps = (int)buf.size() / block;
		for (int i = 0, s = 0; i < (int)buf.size(); i += block, ++s)
		{
			if (s == steps / 4)     ps.SetRatio((float)std::pow(2.0, -1.0 / 12.0));
			if (s == steps / 2)     ps.SetRatio((float)std::pow(2.0, -4.0 / 12.0));
			if (s == 3 * steps / 4) ps.SetRatio(1.0f);
			const int n = (std::min)(block, (int)buf.size() - i);
			ps.Process(&buf[i], n);
		}

		++checks;
		const bool ok = !HasBadSamples(buf);
		if (!ok) ++failures;
		printf("[%s] live shift changes stay finite and bounded (peak %.3f)\n\n",
		       ok ? "PASS" : "FAIL", PeakAbs(buf, 0, (int)buf.size()));

		WriteWavMono32f("rstune_sweep.wav", buf, kSR);
	}

	// ---- 5. audition files ------------------------------------------------
	{
		std::vector<float> dry((size_t)(kSR * 4.0), 0.0f);
		const double riff[] = { 82.41, 110.00, 123.47, 82.41, 146.83, 123.47, 110.00, 82.41 };
		for (int i = 0; i < 8; ++i)
			AddPluck(dry, riff[i], 0.05 + i * 0.45, 0.8, 1.0, 200u + i);

		WriteWavMono32f("rstune_dry.wav", dry, kSR);

		for (int st : { -1, -2, -4 })
		{
			std::vector<float> wet = dry;
			PitchShifter ps;
			ps.Init(kSR);
			ps.SetQuality(RSTuneQuality_Balanced);
			ps.SetRatio((float)std::pow(2.0, st / 12.0));
			ps.Reset();
			RunShifter(ps, wet, block);

			char name[64];
			sprintf_s(name, "rstune_shift%d.wav", st);
			WriteWavMono32f(name, wet, kSR);
		}
		printf("wrote audition wavs: rstune_dry.wav, rstune_shift-1/-2/-4.wav, rstune_sweep.wav\n\n");
	}

	// ---- 6. tuning table --------------------------------------------------
	{
		printf("--- tuning conversions ---\n");
		struct Case { const wchar_t* from; const wchar_t* to; bool uniform; int semis; };
		const Case cases[] = {
			{ L"E Standard",  L"Eb Standard", true,  -1 },
			{ L"E Standard",  L"D Standard",  true,  -2 },
			{ L"E Standard",  L"B Standard",  true,  -5 },
			{ L"Eb Standard", L"E Standard",  true,  +1 },
			{ L"Drop D",      L"Drop C",      true,  -2 },
			{ L"Drop D",      L"Drop C#",     true,  -1 },
			{ L"Drop C",      L"Drop A",      true,  -3 },
			{ L"Drop B",      L"Drop D",      true,  +3 },
			{ L"E Standard",  L"E Standard",  true,   0 },
			// these need one string moved on its own, so they must be rejected
			{ L"E Standard",  L"Drop D",      false,  0 },
			{ L"Drop D",      L"E Standard",  false,  0 },
			{ L"Eb Standard", L"Drop C#",     false,  0 },
			{ L"D Standard",  L"Drop C",      false,  0 },
		};

		auto find = [](const wchar_t* n) {
			for (int i = 0; i < kNumRSTunings; ++i)
				if (wcscmp(kRSTunings[i].name, n) == 0) return i;
			return -1;
		};

		for (const Case& c : cases)
		{
			const int a = find(c.from), b = find(c.to);
			int semis = 0;
			const bool uniform = (a >= 0 && b >= 0) && RSTuningDelta(a, b, &semis);
			const bool ok = (uniform == c.uniform) && (!c.uniform || semis == c.semis);
			++checks;
			if (!ok) ++failures;
			printf("  %-13ls -> %-13ls %-12s %s\n", c.from, c.to,
			       uniform ? "uniform" : "rejected", ok ? "ok" : "<-- FAIL");
		}
		printf("\n");
	}

	printf("=========================================================================\n");
	printf("%d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
