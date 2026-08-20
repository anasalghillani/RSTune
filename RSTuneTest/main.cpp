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
#include "PacketShifter.h"

static int kSR = 48000;   // varied by the sample-rate coverage test
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
// minHz defaults to 60 because widening the search costs real time on every call; the
// bass cases pass a lower floor explicitly.
static double MeasureF0(const std::vector<float>& x, int from, int count, double minHz = 60.0)
{
	const int minLag = (int)(kSR / 1400.0);
	const int maxLag = (int)(kSR / minHz);
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
	const char* qnames[] = { "LowLatency", "Smooth" };

	printf("RSTune DSP harness  (sample rate %d, block %d)\n", kSR, block);
	printf("=========================================================================\n\n");

	// ---- 1. bypass must be bit exact -------------------------------------
	{
		std::vector<float> in((size_t)(kSR * 1.0), 0.0f);
		AddPluck(in, 110.0, 0.05, 0.9, 1.0, 7);
		std::vector<float> out = in;

		PitchShifter ps;
		ps.Init(kSR);
		ps.SetQuality(RSTuneQuality_LowLatency);
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
	for (int qi = 0; qi < RSTuneQuality_Count; ++qi)
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

		const char* qnames2[] = { "LowLatency", "Smooth" };
		for (int q = 0; q < RSTuneQuality_Count; ++q)
		for (const ChordDef& cd : chords)
		{
			for (int st : { -2 })
			{
				const float ratio = (float)std::pow(2.0, st / 12.0);

				std::vector<float> buf((size_t)(kSR * 2.2), 0.0f);
				for (int i = 0; i < cd.n; ++i)
					AddPluck(buf, cd.hz[i], 0.05 + i * 0.008, 2.0, 0.8, (unsigned)(cd.hz[i] * 10) + st);

				PitchShifter ps;
				ps.Init(kSR);
				ps.SetQuality(q);
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
		ps.SetQuality(RSTuneQuality_LowLatency);
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
		ps.SetQuality(RSTuneQuality_LowLatency);
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
			ps.SetQuality(RSTuneQuality_LowLatency);
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

	// ---- 7. sample rate coverage ------------------------------------------
	// Interfaces run at 44.1, 48, 88.2, 96 and sometimes 192 kHz. The pitch tracker
	// works on a decimated copy, so the decimation factor has to adapt or its usable
	// range slides with the device rate and low chords stop aligning.
	{
		printf("--- sample rate coverage: low E note and E5 power chord, -2 st ---\n");
		const int rates[] = { 44100, 48000, 88200, 96000, 192000 };
		const float ratio = (float)std::pow(2.0, -2.0 / 12.0);

		for (int sr : rates)
		{
			kSR = sr;

			// single low E
			std::vector<float> note((size_t)(kSR * 2.2), 0.0f);
			AddPluck(note, 82.41, 0.05, 2.0, 1.0, 3);
			PitchShifter a; a.Init(kSR); a.SetQuality(RSTuneQuality_LowLatency); a.SetRatio(ratio); a.Reset();
			RunShifter(a, note, 128);
			const double noteHz = MeasureF0(note, (int)(0.35 * kSR), (int)(1.0 * kSR));
			const double noteCents = Cents(noteHz, 82.41 * ratio);

			// E5 power chord, the case that needs the composite period
			std::vector<float> chord((size_t)(kSR * 2.2), 0.0f);
			AddPluck(chord, 82.41, 0.05, 2.0, 0.8, 4);
			AddPluck(chord, 123.47, 0.058, 2.0, 0.8, 5);
			PitchShifter b; b.Init(kSR); b.SetQuality(RSTuneQuality_LowLatency); b.SetRatio(ratio); b.Reset();
			RunShifter(b, chord, 128);

			const int from = (int)(0.35 * kSR), count = (int)(0.8 * kSR);
			double worst = 1e9;
			for (double f : { 82.41, 123.47 })
			{
				const double ms = MagAt(chord, from, count, f * ratio);
				const double mo = MagAt(chord, from, count, f);
				worst = (std::min)(worst, 20.0 * std::log10((ms + 1e-12) / (mo + 1e-12)));
			}

			checks += 2;
			const bool noteOk = std::fabs(noteCents) < 15.0 && !HasBadSamples(note);
			const bool chordOk = worst > 12.0 && !HasBadSamples(chord);
			if (!noteOk) ++failures;
			if (!chordOk) ++failures;

			printf("  %6d Hz   note %+6.1fc %-4s   chord %+6.1f dB %-4s   tracker saw %.1f Hz\n",
			       sr, noteCents, noteOk ? "ok" : "FAIL",
			       worst, chordOk ? "ok" : "FAIL", b.GetDetectedHz());
		}
		kSR = 48000;
		printf("\n");
	}

	// ---- 8. WASAPI packet path --------------------------------------------
	// The WASAPI hook receives interleaved packets of varying size in whatever format
	// the device negotiated, which is a different shape of problem from the ASIO path.
	// This drives the real PacketShifter, the same code the hook calls.
	{
		printf("--- WASAPI packet path: interleaved formats and ragged packet sizes ---\n");

		struct FmtCase { const wchar_t* name; WORD bits; bool isFloat; WORD channels; };
		const FmtCase fmts[] = {
			{ L"16 bit PCM mono",    16, false, 1 },
			{ L"16 bit PCM stereo",  16, false, 2 },
			{ L"24 bit PCM mono",    24, false, 1 },
			{ L"32 bit PCM stereo",  32, false, 2 },
			{ L"32 bit float mono",  32, true,  1 },
			{ L"32 bit float stereo",32, true,  2 },
		};

		// deliberately uneven, the way WASAPI hands out packets
		const unsigned packets[] = { 160, 441, 128, 1024, 96, 480 };
		const float ratio = (float)std::pow(2.0, -2.0 / 12.0);
		const double srcHz = 110.0;   // A2

		for (const FmtCase& f : fmts)
		{
			WAVEFORMATEXTENSIBLE wf{};
			wf.Format.wFormatTag = f.isFloat ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
			wf.Format.nChannels = f.channels;
			wf.Format.nSamplesPerSec = kSR;
			wf.Format.wBitsPerSample = f.bits;
			wf.Format.nBlockAlign = (WORD)(f.channels * (f.bits / 8));
			wf.Format.nAvgBytesPerSec = wf.Format.nBlockAlign * kSR;
			wf.Format.cbSize = 0;

			PacketShifter pk;
			if (!pk.Init(&wf.Format, 2048))
			{
				++checks; ++failures;
				printf("  %-22ls  Init FAILED\n", f.name);
				continue;
			}
			pk.SetQuality(RSTuneQuality_LowLatency);
			pk.SetRatio(ratio);
			pk.Reset();

			// build an interleaved source: channel 0 the note, channel 1 an octave up so
            // cross-channel bleed would be obvious
			const int total = (int)(kSR * 2.2);
			std::vector<float> chA(total, 0.0f), chB(total, 0.0f);
			AddPluck(chA, srcHz, 0.05, 2.0, 0.8, 21);
			AddPluck(chB, srcHz * 2.0, 0.05, 2.0, 0.8, 22);

			std::vector<BYTE> in((size_t)total * wf.Format.nBlockAlign, 0);
			for (int i = 0; i < total; ++i)
			{
				for (WORD c = 0; c < f.channels; ++c)
				{
					const float v = (c == 0) ? chA[i] : chB[i];
					BYTE* dst = in.data() + (size_t)i * wf.Format.nBlockAlign + c * (f.bits / 8);
					if (f.isFloat) { *(float*)dst = v; }
					else if (f.bits == 16) { *(int16_t*)dst = (int16_t)(v * 32767.0f); }
					else if (f.bits == 24) { const int32_t s = (int32_t)(v * 8388607.0f); memcpy(dst, &s, 3); }
					else { *(int32_t*)dst = (int32_t)(v * 2147483000.0); }
				}
			}

			// feed it through in ragged packets and reassemble channel 0
			std::vector<float> outA(total, 0.0f);
			int pos = 0, pi = 0;
			bool refused = false;
			while (pos < total)
			{
				unsigned n = packets[pi++ % (sizeof(packets) / sizeof(packets[0]))];
				if ((int)n > total - pos) n = (unsigned)(total - pos);

				BYTE* got = pk.Process(in.data() + (size_t)pos * wf.Format.nBlockAlign, n, false);
				if (!got) { refused = true; break; }

				for (unsigned i = 0; i < n; ++i)
				{
					const BYTE* s = got + (size_t)i * wf.Format.nBlockAlign;
					float v = 0.0f;
					if (f.isFloat) v = *(const float*)s;
					else if (f.bits == 16) v = *(const int16_t*)s / 32768.0f;
					else if (f.bits == 24) { int32_t t = 0; memcpy((BYTE*)&t + 1, s, 3); v = t / 2147483648.0f; }
					else v = *(const int32_t*)s / 2147483648.0f;
					outA[pos + i] = v;
				}
				pos += n;
			}

			const double measured = MeasureF0(outA, (int)(0.35 * kSR), (int)(1.0 * kSR));
			const double cents = Cents(measured, srcHz * ratio);

			++checks;
			const bool ok = !refused && std::fabs(cents) < 15.0 && !HasBadSamples(outA);
			if (!ok) ++failures;

			printf("  %-22ls  %8.2f Hz -> %8.2f Hz  %+6.1fc  %s\n",
			       f.name, srcHz, measured, cents, ok ? "ok" : "<-- FAIL");
		}

		// oversized packets must be refused, not silently truncated
		{
			WAVEFORMATEXTENSIBLE wf{};
			wf.Format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
			wf.Format.nChannels = 1;
			wf.Format.nSamplesPerSec = kSR;
			wf.Format.wBitsPerSample = 32;
			wf.Format.nBlockAlign = 4;
			wf.Format.nAvgBytesPerSec = 4 * kSR;

			PacketShifter pk;
			pk.Init(&wf.Format, 512);
			std::vector<BYTE> big((size_t)4096 * 4, 0);

			++checks;
			const bool refused = (pk.Process(big.data(), 4096, false) == nullptr);
			const bool silentOk = (pk.Process(nullptr, 256, true) != nullptr);
			if (!refused || !silentOk) ++failures;
			printf("  oversized packet refused: %s   silent packet handled: %s\n",
			       refused ? "yes" : "NO", silentOk ? "yes" : "NO");

		// a format we cannot convert must decline cleanly, so the caller hands the
		// driver's buffer through untouched instead of producing garbage or crashing
		{
			struct Bad { const wchar_t* name; WORD bits; bool isFloat; };
			const Bad bad[] = {
				{ L"8 bit PCM",    8,  false },
				{ L"20 bit PCM",   20, false },
				{ L"16 bit float", 16, true  },
			};
			for (const Bad& b : bad)
			{
				WAVEFORMATEXTENSIBLE wf{};
				wf.Format.wFormatTag = b.isFloat ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
				wf.Format.nChannels = 1;
				wf.Format.nSamplesPerSec = kSR;
				wf.Format.wBitsPerSample = b.bits;
				wf.Format.nBlockAlign = (WORD)((b.bits / 8) ? (b.bits / 8) : 1);

				PacketShifter pk;
				const bool declined = !pk.Init(&wf.Format, 512);
				++checks;
				if (!declined) ++failures;
				printf("  %-14ls declined cleanly: %s\n", b.name, declined ? "yes" : "NO");
			}
		}
		}
		printf("\n");
	}

	// ---- 9. bass range ----------------------------------------------------
	// Rocksmith has bass arrangements and a uniform shift applies to bass identically,
	// so the tracker has to reach below a 4 string low E at 41.2 Hz and ideally a
	// 5 string low B at 30.9 Hz.
	{
		printf("--- bass range ---\n");
		struct BNote { const char* name; double hz; };
		const BNote notes[] = {
			{ "B0 (5 string low B)", 30.87 },
			{ "E1 (4 string low E)", 41.20 },
			{ "A1",                  55.00 },
			{ "D2",                  73.42 },
			{ "G2",                  98.00 },
		};

		const char* qn[] = { "LowLatency", "Smooth" };
		for (const BNote& n : notes)
		{
			for (int q = 0; q < RSTuneQuality_Count; ++q)
			for (int st : { -2 })
			{
				const float ratio = (float)std::pow(2.0, st / 12.0);
				const double expected = n.hz * ratio;

				std::vector<float> buf((size_t)(kSR * 2.6), 0.0f);
				AddPluck(buf, n.hz, 0.05, 2.4, 1.0, (unsigned)(n.hz * 100) + st);

				PitchShifter ps;
				ps.Init(kSR);
				ps.SetQuality(q);
				ps.SetRatio(ratio);
				ps.Reset();

				const int from = (int)(0.5 * kSR), count = (int)(1.2 * kSR);
				const int mid = from + count / 2;
				float latencyMs = 0.0f, detected = 0.0f;
				for (int i = 0; i < (int)buf.size(); i += 128)
				{
					const int m = (std::min)(128, (int)buf.size() - i);
					ps.Process(&buf[i], m);
					if (i <= mid && i + 128 > mid)
					{
						latencyMs = ps.GetLatencySamples() * 1000.0f / kSR;
						detected = ps.GetDetectedHz();
					}
				}

				const double measured = MeasureF0(buf, from, count, 22.0);
				const double cents = Cents(measured, expected);

				++checks;
				const bool ok = std::fabs(cents) < 20.0 && !HasBadSamples(buf);
				if (!ok) ++failures;

				printf("  %-22s %+3d st  %7.2f -> %7.2f Hz  %+6.1fc  tracker %6.1f Hz  %5.1fms  %s\n",
				       n.name, st, expected, measured, cents, detected, latencyMs,
				       ok ? "ok" : "<-- FAIL");
			}
		}
		printf("\n");
	}

	// ---- 10. grain switch while notes are still ringing --------------------
	// Every chord case above starts from silence, which is the one condition where a
	// pending grain change is guaranteed to be applied. Real playing is not like that:
	// a chord struck while the previous notes still ring produces neither a quiet gate
	// nor an onset, so the grain the tracker asked for may never be installed.
	{
		printf("--- chord preceded by ringing notes vs from silence ---\n");
		const float ratio = (float)std::pow(2.0, -2.0 / 12.0);
		const double root = 82.41, fifth = 123.47;

		struct Ctx { const char* name; bool leadIn; };
		const Ctx ctxs[] = { { "from silence", false }, { "after a ringing run", true } };

		for (const Ctx& c : ctxs)
		{
			std::vector<float> buf((size_t)(kSR * 4.0), 0.0f);

			// lead-in notes that are still sounding when the chord arrives
			if (c.leadIn)
			{
				AddPluck(buf, 146.83, 0.05, 1.6, 0.9, 61);
				AddPluck(buf, 196.00, 0.35, 1.4, 0.9, 62);
				AddPluck(buf, 246.94, 0.65, 1.2, 0.9, 63);
			}
			AddPluck(buf, root,  0.95, 2.6, 0.8, 64);
			AddPluck(buf, fifth, 0.958, 2.6, 0.8, 65);

			PitchShifter ps;
			ps.Init(kSR);
			ps.SetQuality(RSTuneQuality_LowLatency);
			ps.SetRatio(ratio);
			ps.Reset();

			const int from = (int)(1.4 * kSR), count = (int)(0.8 * kSR);
			const int mid = from + count / 2;
			int grainAtMid = 0;
			for (int i = 0; i < (int)buf.size(); i += 128)
			{
				const int n = (std::min)(128, (int)buf.size() - i);
				ps.Process(&buf[i], n);
				if (i <= mid && i + 128 > mid) grainAtMid = ps.GetGrain();
			}

			// spread across the chord's shifted partials: a comb notch shows up as one
			// partial being far quieter than the others
			double lo = 1e9, hi = -1e9;
			for (double f : { root, fifth })
				for (int k = 1; k <= 3; ++k)
				{
					const double db = 20.0 * std::log10(MagAt(buf, from, count, f * ratio * k) + 1e-12);
					if (db > hi) hi = db;
					if (db < lo) lo = db;
				}

			++checks;
			// 40 dB was loose enough that the stale-grain bug passed it at 30.6 dB. The
			// aligned case measures about 15 dB, so 20 fails the bug and passes the fix.
			const bool ok = !HasBadSamples(buf) && (hi - lo) < 20.0;
			if (!ok) ++failures;

			printf("  %-20s grain %5d (half %4d)   partial spread %5.1f dB   %s\n",
			       c.name, grainAtMid, grainAtMid / 2, hi - lo, ok ? "" : "<-- FAIL");
		}
		printf("  (an E5 power chord needs half ~1166 at 48 kHz to stay aligned)\n\n");
	}

	printf("=========================================================================\n");
	printf("%d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
