#include "stdafx.h"
#include "RSTuneShm.h"
#include "GameConfig.h"

RSTuneShm& RSTuneShm::Get()
{
	static RSTuneShm s_instance;
	return s_instance;
}

RSTuneShm::RSTuneShm()
{
	// defaults if no GUI ever connects: do nothing to the audio
	m_cached.enabled = 0;
	m_cached.semitones = 0;
	m_cached.cents = 0.0f;
	m_cached.quality = RSTuneQuality_Balanced;
	for (int i = 0; i < RSTUNE_MAX_STREAMS; ++i)
		m_cached.streamEnabled[i] = 1;

	m_staging.gameConfig = RSTuneGetGameConfig();

	HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
	                              0, sizeof(RSTuneShared), RSTUNE_SHM_NAME);
	if (!h)
	{
		rslog::error_ts() << "RSTune: CreateFileMapping failed, err " << GetLastError() << std::endl;
		return;
	}

	void* view = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(RSTuneShared));
	if (!view)
	{
		rslog::error_ts() << "RSTune: MapViewOfFile failed, err " << GetLastError() << std::endl;
		CloseHandle(h);
		return;
	}

	m_mapping = h;
	m_shared = (RSTuneShared*)view;

	// a freshly created section is zero filled, so a zero magic means we are first
	if (InterlockedCompareExchange((volatile LONG*)&m_shared->magic, (LONG)RSTUNE_MAGIC, 0) == 0)
	{
		m_shared->version = RSTUNE_VERSION;
		RSTuneSeqWrite(m_shared->ctlSeq, m_shared->control, m_cached);
		rslog::info_ts() << "RSTune: created shared control block" << std::endl;
	}
	else if (m_shared->version != RSTUNE_VERSION)
	{
		// a GUI from a different build is attached; refuse rather than misread it
		rslog::error_ts() << "RSTune: shared block version " << m_shared->version
			<< " does not match " << RSTUNE_VERSION << ", ignoring the GUI" << std::endl;
		UnmapViewOfFile(m_shared);
		CloseHandle(h);
		m_shared = nullptr;
		m_mapping = nullptr;
		return;
	}
	else
	{
		rslog::info_ts() << "RSTune: attached to existing shared control block" << std::endl;
	}
}

RSTuneShm::~RSTuneShm()
{
	if (m_shared) UnmapViewOfFile(m_shared);
	if (m_mapping) CloseHandle(m_mapping);
}

const RSTuneControl& RSTuneShm::Control()
{
	if (m_shared)
	{
		RSTuneControl tmp;
		if (RSTuneSeqRead(m_shared->ctlSeq, m_shared->control, tmp))
			m_cached = tmp;
	}
	return m_cached;
}

int RSTuneShm::RegisterStream(int path, const wchar_t* label, const wchar_t* id,
                              float sampleRate, int channels, bool looksLikeMic)
{
	std::lock_guard<std::mutex> g(m_registryMutex);

	int slot = -1;
	for (int i = 0; i < RSTUNE_MAX_STREAMS; ++i)
	{
		if (!m_staging.streams[i].active)
		{
			slot = i;
			break;
		}
	}
	if (slot < 0)
	{
		rslog::error_ts() << "RSTune: no free stream slot" << std::endl;
		return -1;
	}

	RSTuneStreamInfo& s = m_staging.streams[slot];
	memset(&s, 0, sizeof(s));
	s.active = 1;
	s.path = path;
	s.looksLikeMic = looksLikeMic ? 1 : 0;
	s.sampleRate = sampleRate;
	s.channels = channels;
	if (label) wcsncpy_s(s.label, label, RSTUNE_LABEL_LEN - 1);
	if (id)    wcsncpy_s(s.id, id, RSTUNE_ID_LEN - 1);

	if (m_publisher < 0)
		m_publisher = slot;

	if (slot + 1 > m_staging.numStreams)
		m_staging.numStreams = slot + 1;

	m_staging.gameConfig = RSTuneGetGameConfig();

	rslog::info_ts() << "RSTune: registered stream " << slot << " - "
		<< (path == RSTunePath_Asio ? "ASIO" : "WASAPI") << " - " << (label ? label : L"")
		<< (looksLikeMic ? " (looks like a microphone)" : "") << std::endl;

	return slot;
}

void RSTuneShm::UnregisterStream(int slot)
{
	if (slot < 0 || slot >= RSTUNE_MAX_STREAMS)
		return;

	std::lock_guard<std::mutex> g(m_registryMutex);
	m_staging.streams[slot].active = 0;
	m_staging.streams[slot].shifted = 0;

	if (m_publisher == slot)
	{
		m_publisher = -1;
		for (int i = 0; i < RSTUNE_MAX_STREAMS; ++i)
		{
			if (m_staging.streams[i].active) { m_publisher = i; break; }
		}
	}
}

bool RSTuneShm::IsStreamEnabled(int slot)
{
	if (slot < 0 || slot >= RSTUNE_MAX_STREAMS)
		return false;
	return Control().streamEnabled[slot] != 0;
}

void RSTuneShm::UpdateStream(int slot, bool shifted, float detectedHz, float addedLatencyMs)
{
	if (slot < 0 || slot >= RSTUNE_MAX_STREAMS)
		return;

	RSTuneStreamInfo& s = m_staging.streams[slot];
	s.shifted = shifted ? 1 : 0;
	s.detectedHz = detectedHz;
	s.addedLatencyMs = addedLatencyMs;
}

void RSTuneShm::PublishGlobal(int slot, float sampleRate, int blockFrames, float inPeakDb,
                              float outPeakDb, float detectedHz, float addedLatencyMs, float cpuPercent)
{
	if (!m_shared || slot != m_publisher)
		return;

	m_staging.heartbeat++;
	m_staging.streamActive = 1;
	m_staging.sampleRate = sampleRate;
	m_staging.blockFrames = blockFrames;
	m_staging.inputPeakDb = inPeakDb;
	m_staging.outputPeakDb = outPeakDb;
	m_staging.detectedHz = detectedHz;
	m_staging.addedLatencyMs = addedLatencyMs;
	m_staging.cpuPercent = cpuPercent;

	RSTuneSeqWrite(m_shared->telSeq, m_shared->telemetry, m_staging);
}
