#include "stdafx.h"
#include <cmath>

#include "WasapiShifter.h"
#include "RSTuneShm.h"
#include "GameConfig.h"


// {6F2B1A54-9C3E-4D77-9A16-2C0E7B8F41D2}
const GUID IID_RSTuneAsioClientMarker =
	{ 0x6f2b1a54, 0x9c3e, 0x4d77, { 0x9a, 0x16, 0x2c, 0x0e, 0x7b, 0x8f, 0x41, 0xd2 } };

std::wstring RSTuneGetDeviceFriendlyName(const std::wstring& deviceId)
{
	std::wstring name;

	IMMDeviceEnumerator* enumerator = nullptr;
	if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
	                            __uuidof(IMMDeviceEnumerator), (void**)&enumerator)) || !enumerator)
		return name;

	IMMDevice* dev = nullptr;
	if (SUCCEEDED(enumerator->GetDevice(deviceId.c_str(), &dev)) && dev)
	{
		IPropertyStore* props = nullptr;
		if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)) && props)
		{
			PROPVARIANT v;
			PropVariantInit(&v);
			if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal)
				name = v.pwszVal;
			PropVariantClear(&v);
			props->Release();
		}
		dev->Release();
	}
	enumerator->Release();

	return name;
}

// A guitar interface and a headset both enumerate as capture endpoints and there is no
// reliable way to tell them apart, so this is only ever a hint the GUI may act on. It
// stays quiet unless the game actually has microphone support switched on.
static bool LooksLikeMicrophone(const std::wstring& friendlyName)
{
	if (!RSTuneGetGameConfig().enableMicrophone)
		return false;

	std::wstring lower = friendlyName;
	std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);

	return lower.find(L"microphone") != std::wstring::npos
		|| lower.find(L"headset") != std::wstring::npos
		|| lower.find(L"webcam") != std::wstring::npos;
}

void WasapiShifter::Init(const WAVEFORMATEX* fmt, unsigned maxFrames, const std::wstring& deviceId)
{
	Shutdown();

	if (!m_packet.Init(fmt, maxFrames))
	{
		if (fmt)
			rslog::error_ts() << "RSTune (wasapi): unsupported format, " << fmt->wBitsPerSample
				<< " bits, " << fmt->nChannels << " ch" << std::endl;
		return;
	}

	const std::wstring friendly = RSTuneGetDeviceFriendlyName(deviceId);
	const std::wstring label = friendly.empty() ? deviceId : friendly;

	m_slot = RSTuneShm::Get().RegisterStream(
		RSTunePath_Wasapi, label.c_str(), deviceId.c_str(),
		(float)m_packet.SampleRate(), (int)m_packet.Channels(), LooksLikeMicrophone(friendly));

	rslog::info_ts() << "RSTune (wasapi): ready - " << label << " - " << m_packet.Channels()
		<< " ch, " << (unsigned)m_packet.SampleRate() << " Hz, up to " << maxFrames
		<< " frames, slot " << m_slot << std::endl;
}

void WasapiShifter::Shutdown()
{
	if (m_slot >= 0)
	{
		RSTuneShm::Get().UnregisterStream(m_slot);
		m_slot = -1;
	}
}

BYTE* WasapiShifter::Process(BYTE* src, unsigned numFrames, DWORD flags)
{
	if (!m_packet.IsActive())
		return src;

	RSTuneShm& shm = RSTuneShm::Get();
	const RSTuneControl& ctl = shm.Control();

	float ratio = 1.0f;
	if (ctl.enabled && shm.IsStreamEnabled(m_slot))
	{
		float st = (float)ctl.semitones + ctl.cents * 0.01f;
		if (st < -12.0f) st = -12.0f;
		else if (st > 12.0f) st = 12.0f;
		ratio = powf(2.0f, st / 12.0f);
	}

	static LARGE_INTEGER s_qpf = { 0 };
	if (s_qpf.QuadPart == 0)
		QueryPerformanceFrequency(&s_qpf);

	LARGE_INTEGER t0;
	QueryPerformanceCounter(&t0);

	m_packet.SetQuality(ctl.quality);
	m_packet.SetRatio(ratio);

	BYTE* out = m_packet.Process(src, numFrames, (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || !src);
	if (!out)
		return src;   // packet we cannot handle, hand the original straight back

	LARGE_INTEGER t1;
	QueryPerformanceCounter(&t1);

	const double sr = m_packet.SampleRate();
	const double elapsed = (double)(t1.QuadPart - t0.QuadPart) / (double)s_qpf.QuadPart;
	const double budget = (double)numFrames / sr;

	const float detectedHz = m_packet.GetDetectedHz();
	const float latencyMs = (float)(m_packet.GetLatencySamples() * 1000.0 / sr);

	shm.UpdateStream(m_slot, ratio != 1.0f, detectedHz, latencyMs);
	shm.PublishGlobal(m_slot, (float)sr, (int)numFrames,
		20.0f * log10f(m_packet.GetInputPeak() + 1.0e-9f),
		20.0f * log10f(m_packet.GetOutputPeak() + 1.0e-9f),
		detectedHz, latencyMs,
		(budget > 0.0) ? (float)(100.0 * elapsed / budget) : 0.0f);

	return out;
}
