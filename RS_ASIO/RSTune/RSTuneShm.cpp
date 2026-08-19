#include "stdafx.h"
#include "RSTuneShm.h"

RSTuneShm& RSTuneShm::Get()
{
	static RSTuneShm s_instance;
	return s_instance;
}

RSTuneShm::RSTuneShm()
{
	// sensible defaults in case no GUI ever connects: do nothing to the audio
	m_cached.enabled = 0;
	m_cached.semitones = 0;
	m_cached.cents = 0.0f;
	m_cached.quality = RSTuneQuality_Balanced;
	m_cached.applyInput0 = 1;
	m_cached.applyInput1 = 1;

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
	if (m_shared && m_shared->version == RSTUNE_VERSION)
	{
		RSTuneControl tmp;
		if (RSTuneSeqRead(m_shared->ctlSeq, m_shared->control, tmp))
			m_cached = tmp;
	}
	return m_cached;
}

void RSTuneShm::PublishTelemetry(const RSTuneTelemetry& t)
{
	if (m_shared)
		RSTuneSeqWrite(m_shared->telSeq, m_shared->telemetry, t);
}

bool RSTuneShm::ClaimTelemetry(const void* owner)
{
	if (!m_telemetryOwner)
		m_telemetryOwner = owner;
	return m_telemetryOwner == owner;
}
