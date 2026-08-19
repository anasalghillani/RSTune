// RSTune - shifts a real WASAPI capture stream.
//
// This is the path used when RS_ASIO hands the game the actual system devices
// (EnableWasapiInputs=1), which is also how a Real Tone cable is normally seen. The
// hook lives in DebugWrapperCaptureClient::GetBuffer, which already sits between the
// driver and the game.
//
// All the packet and format handling lives in PacketShifter, which the offline harness
// exercises directly. This class only adds the parts that need a running game: reading
// the control block, registering the stream and publishing telemetry.
#pragma once

#include <string>

#include "PacketShifter.h"

class WasapiShifter
{
public:
	// deviceId is the MMDevice id, used to label the stream in the GUI. maxFrames comes
	// from the audio client's buffer size; larger packets are passed through untouched
	// rather than allocating on the realtime thread.
	void Init(const WAVEFORMATEX* fmt, unsigned maxFrames, const std::wstring& deviceId);
	void Shutdown();

	bool IsActive() const { return m_packet.IsActive(); }

	// Returns the buffer the game should receive: our processed copy when active,
	// otherwise the driver's original pointer untouched.
	BYTE* Process(BYTE* src, unsigned numFrames, DWORD flags);

private:
	PacketShifter m_packet;
	int m_slot = -1;
};

// Best effort friendly name for an MMDevice id. Not realtime safe.
std::wstring RSTuneGetDeviceFriendlyName(const std::wstring& deviceId);

// Private marker interface. RSAsioAudioClient answers to it, which lets the WASAPI hook
// recognise that a stream is already handled by the ASIO path and stand down. Without
// this the ASIO devices, which are themselves wrapped by DebugWrapperAudioClient, would
// be shifted twice.
extern const GUID IID_RSTuneAsioClientMarker;
