#include "stdafx.h"
#include <cmath>

#include "PacketShifter.h"
#include "AudioProcessing.h"

bool PacketShifter::Init(const WAVEFORMATEX* fmt, unsigned maxFrames)
{
	m_valid = false;
	m_shifters.clear();
	m_out.clear();
	m_scratch.clear();

	if (!fmt || fmt->nChannels == 0 || maxFrames == 0)
		return false;

	bool isFloat = (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
	if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && fmt->cbSize >= 22)
	{
		const WAVEFORMATEXTENSIBLE* ext = (const WAVEFORMATEXTENSIBLE*)fmt;
		isFloat = (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
	}

	ASIOSampleType type = ASIOSTInt32LSB;
	if (!AsioSampleTypeFromFormat(&type, fmt->wBitsPerSample, isFloat))
		return false;

	const WORD bytes = GetAsioSampleTypeNumBytes(type);
	if (!bytes)
		return false;

	m_channels    = fmt->nChannels;
	m_maxFrames   = maxFrames;
	m_blockAlign  = fmt->nBlockAlign ? fmt->nBlockAlign : (WORD)(bytes * m_channels);
	m_sampleRate  = (double)fmt->nSamplesPerSec;
	m_sampleType  = type;
	m_sampleBytes = bytes;

	m_out.assign((size_t)m_maxFrames * m_blockAlign, 0);
	m_scratch.assign(m_maxFrames, 0.0f);

	m_shifters.resize(m_channels);
	for (PitchShifter& ps : m_shifters)
		ps.Init(m_sampleRate);

	m_valid = true;
	return true;
}

void PacketShifter::Reset()
{
	for (PitchShifter& ps : m_shifters)
		ps.Reset();
}

void PacketShifter::SetRatio(float ratio)
{
	for (PitchShifter& ps : m_shifters)
		ps.SetRatio(ratio);
}

void PacketShifter::SetQuality(int quality)
{
	for (PitchShifter& ps : m_shifters)
		ps.SetQuality(quality);
}

float PacketShifter::GetDetectedHz() const
{
	return m_shifters.empty() ? 0.0f : m_shifters[0].GetDetectedHz();
}

float PacketShifter::GetLatencySamples() const
{
	return m_shifters.empty() ? 0.0f : m_shifters[0].GetLatencySamples();
}

BYTE* PacketShifter::Process(const BYTE* src, unsigned numFrames, bool silent)
{
	if (!m_valid || numFrames == 0 || numFrames > m_maxFrames)
		return nullptr;
	if (!silent && !src)
		return nullptr;

	// a silent packet may point at uninitialised driver memory, so synthesise the
	// silence rather than reading it, and still run it through the shifter so the
	// delay line stays continuous
	if (silent)
		memset(m_out.data(), 0, (size_t)numFrames * m_blockAlign);
	else
		memcpy(m_out.data(), src, (size_t)numFrames * m_blockAlign);

	m_inPeak = 0.0f;
	m_outPeak = 0.0f;

	for (unsigned ch = 0; ch < m_channels; ++ch)
	{
		BYTE* chBase = m_out.data() + ch * m_sampleBytes;

		AudioProcessing::CopyConvertFormat(
			chBase, m_sampleType, (WORD)m_blockAlign,
			numFrames,
			(BYTE*)m_scratch.data(), ASIOSTFloat32LSB, (WORD)sizeof(float));

		if (ch == 0)
			for (unsigned i = 0; i < numFrames; ++i)
			{
				const float a = std::fabs(m_scratch[i]);
				if (a > m_inPeak) m_inPeak = a;
			}

		m_shifters[ch].Process(m_scratch.data(), (int)numFrames);

		if (ch == 0)
			for (unsigned i = 0; i < numFrames; ++i)
			{
				const float a = std::fabs(m_scratch[i]);
				if (a > m_outPeak) m_outPeak = a;
			}

		AudioProcessing::CopyConvertFormat(
			(BYTE*)m_scratch.data(), ASIOSTFloat32LSB, (WORD)sizeof(float),
			numFrames,
			chBase, m_sampleType, (WORD)m_blockAlign);
	}

	return m_out.data();
}
