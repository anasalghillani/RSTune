#include "stdafx.h"
#include "GameConfig.h"

static RSTuneGameConfig s_config{};
static bool s_loaded = false;

static int ReadIniInt(const std::wstring& path, const wchar_t* key, int def)
{
	return (int)GetPrivateProfileIntW(L"Audio", key, def, path.c_str());
}

const RSTuneGameConfig& RSTuneGetGameConfig()
{
	if (s_loaded)
		return s_config;
	s_loaded = true;

	const std::wstring path = GetGamePath() + L"Rocksmith.ini";

	if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
	{
		rslog::info_ts() << "RSTune: Rocksmith.ini not found, assuming defaults" << std::endl;
		s_config.valid = 0;
		s_config.enableMicrophone = 0;
		return s_config;
	}

	s_config.valid = 1;
	s_config.enableMicrophone        = ReadIniInt(path, L"EnableMicrophone", 0);
	s_config.exclusiveMode           = ReadIniInt(path, L"ExclusiveMode", 1);
	s_config.latencyBuffer           = ReadIniInt(path, L"LatencyBuffer", 4);
	s_config.maxOutputBufferSize     = ReadIniInt(path, L"MaxOutputBufferSize", 0);
	s_config.forceWDM                = ReadIniInt(path, L"ForceWDM", 0);
	s_config.forceDirectXSink        = ReadIniInt(path, L"ForceDirectXSink", 0);
	s_config.realToneCableOnly       = ReadIniInt(path, L"RealToneCableOnly", 0);
	s_config.win32UltraLowLatencyMode= ReadIniInt(path, L"Win32UltraLowLatencyMode", 0);

	rslog::info_ts() << "RSTune: Rocksmith.ini - EnableMicrophone " << s_config.enableMicrophone
		<< ", ExclusiveMode " << s_config.exclusiveMode
		<< ", ForceWDM " << s_config.forceWDM
		<< ", ForceDirectXSink " << s_config.forceDirectXSink << std::endl;

	if (s_config.forceWDM || s_config.forceDirectXSink)
	{
		rslog::error_ts() << "RSTune: ForceWDM or ForceDirectXSink is set. The game routes audio "
			"around the interfaces RSTune hooks, so no shifting will happen." << std::endl;
	}

	return s_config;
}
