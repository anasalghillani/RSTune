// RSTune - control panel.
//
// Publishes a tuning shift into shared memory; the copy of RS_ASIO.dll loaded inside
// Rocksmith picks it up on its audio thread and pitch shifts the guitar input. Because
// the game receives the shifted signal, its tuner and its note detection both agree
// with the tuning you selected here.
//
// Only uniform shifts are offered. Standard-to-standard and drop-to-drop conversions
// move every string by the same number of semitones and are exactly what a single
// pitch shift can do. Anything that would need one string moved on its own, such as
// E Standard to Drop D, is filtered out of the target list rather than faked.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <windowsx.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <stdio.h>
#include <math.h>

#include "RSTuneShared.h"
#include "Tunings.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

enum
{
	IDC_CURRENT = 1001,
	IDC_TARGET,
	IDC_ENABLE,
	IDC_QUALITY,
	IDC_CENTS,
	IDC_CENTSPIN,
	IDC_SHIFT,
	IDC_GAMESTATE,
	IDC_LEVEL,
	IDC_DETECTED,
	IDC_LATENCY,
	IDC_HINT,
	IDC_LAUNCH,
};

static HINSTANCE g_inst;
static HWND  g_main, g_curCombo, g_tgtCombo, g_enable, g_quality;
static HFONT g_font, g_fontBold;
static wchar_t g_iniPath[MAX_PATH];

static RSTuneShared* g_shm = nullptr;
static HANDLE g_shmHandle = nullptr;

static int g_curIndex = 0;
static int g_tgtIndex = 0;
static bool g_loading = false;   // suppresses control notifications while settings load
static int g_lastHeartbeat = -1;
static int g_staleTicks = 0;

// ---------------------------------------------------------------------------
// shared memory
// ---------------------------------------------------------------------------
static bool ShmOpen()
{
	g_shmHandle = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
	                                 0, sizeof(RSTuneShared), RSTUNE_SHM_NAME);
	if (!g_shmHandle)
		return false;

	g_shm = (RSTuneShared*)MapViewOfFile(g_shmHandle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(RSTuneShared));
	if (!g_shm)
	{
		CloseHandle(g_shmHandle);
		g_shmHandle = nullptr;
		return false;
	}

	if (InterlockedCompareExchange((volatile LONG*)&g_shm->magic, (LONG)RSTUNE_MAGIC, 0) == 0)
		g_shm->version = RSTUNE_VERSION;

	return true;
}

static void ShmClose()
{
	if (g_shm) UnmapViewOfFile(g_shm);
	if (g_shmHandle) CloseHandle(g_shmHandle);
	g_shm = nullptr;
	g_shmHandle = nullptr;
}

// ---------------------------------------------------------------------------
// settings
// ---------------------------------------------------------------------------
static void SettingsPath()
{
	GetModuleFileNameW(nullptr, g_iniPath, MAX_PATH);
	PathRemoveFileSpecW(g_iniPath);
	PathAppendW(g_iniPath, L"RSTune.ini");
}

static int IniGet(const wchar_t* key, int def)
{
	return (int)GetPrivateProfileIntW(L"RSTune", key, def, g_iniPath);
}

static void IniSet(const wchar_t* key, int value)
{
	wchar_t buf[32];
	swprintf_s(buf, L"%d", value);
	WritePrivateProfileStringW(L"RSTune", key, buf, g_iniPath);
}

// ---------------------------------------------------------------------------
// control state
// ---------------------------------------------------------------------------

// Returns the semitone delta for the current selection, or false when the pair is not
// a uniform shift (which the target list should already have prevented).
static bool CurrentShift(int* outSemitones)
{
	return RSTuningDelta(g_curIndex, g_tgtIndex, outSemitones);
}

static void PushControl()
{
	if (!g_shm)
		return;

	int semis = 0;
	const bool uniform = CurrentShift(&semis);

	RSTuneControl c;
	c.enabled = (Button_GetCheck(g_enable) == BST_CHECKED && uniform) ? 1 : 0;
	c.semitones = uniform ? semis : 0;

	// reserved for a later build; v1 shifts by whole semitones only
	c.cents = 0.0f;

	c.quality = (int)SendMessageW(g_quality, CB_GETCURSEL, 0, 0);
	if (c.quality < 0 || c.quality >= RSTuneQuality_Count)
		c.quality = RSTuneQuality_Balanced;

	c.applyInput0 = 1;
	c.applyInput1 = 1;

	RSTuneSeqWrite(g_shm->ctlSeq, g_shm->control, c);
}

static void SaveSettings()
{
	IniSet(L"CurrentTuning", g_curIndex);
	IniSet(L"TargetTuning", g_tgtIndex);
	IniSet(L"Enabled", Button_GetCheck(g_enable) == BST_CHECKED ? 1 : 0);
	IniSet(L"Quality", (int)SendMessageW(g_quality, CB_GETCURSEL, 0, 0));

}

// Rebuilds the target list so it only offers tunings reachable by a uniform shift.
static void RefreshTargets()
{
	const int previous = g_tgtIndex;

	SendMessageW(g_tgtCombo, CB_RESETCONTENT, 0, 0);

	int selectIdx = -1, n = 0;
	for (int i = 0; i < kNumRSTunings; ++i)
	{
		int semis;
		if (!RSTuningDelta(g_curIndex, i, &semis))
			continue;

		wchar_t label[128];
		if (semis == 0)
			swprintf_s(label, L"%s  (no shift)", kRSTunings[i].name);
		else
			swprintf_s(label, L"%s  (%+d)", kRSTunings[i].name, semis);

		SendMessageW(g_tgtCombo, CB_ADDSTRING, 0, (LPARAM)label);
		SendMessageW(g_tgtCombo, CB_SETITEMDATA, n, (LPARAM)i);
		if (i == previous)
			selectIdx = n;
		++n;
	}

	if (selectIdx < 0)
	{
		// previous target is not reachable from the new tuning, fall back to no shift
		for (int k = 0; k < n; ++k)
		{
			if ((int)SendMessageW(g_tgtCombo, CB_GETITEMDATA, k, 0) == g_curIndex)
			{
				selectIdx = k;
				break;
			}
		}
	}
	if (selectIdx < 0) selectIdx = 0;

	SendMessageW(g_tgtCombo, CB_SETCURSEL, selectIdx, 0);
	g_tgtIndex = (int)SendMessageW(g_tgtCombo, CB_GETITEMDATA, selectIdx, 0);
}

static void UpdateShiftLabel()
{
	int semis = 0;
	wchar_t text[256];

	if (!CurrentShift(&semis))
	{
		swprintf_s(text, L"Not a uniform shift");
	}
	else if (semis == 0)
	{
		swprintf_s(text, L"No shift  -  passing your guitar through untouched");
	}
	else
	{
		swprintf_s(text, L"%+d semitone%s   (%s  \x2192  %s)",
		           semis, (semis == 1 || semis == -1) ? L"" : L"s",
		           kRSTunings[g_curIndex].notes, kRSTunings[g_tgtIndex].notes);
	}
	SetDlgItemTextW(g_main, IDC_SHIFT, text);
}

// ---------------------------------------------------------------------------
// telemetry
// ---------------------------------------------------------------------------
static void UpdateStatus()
{
	wchar_t buf[160];

	RSTuneTelemetry t;
	bool haveTelemetry = false;
	if (g_shm && g_shm->magic == RSTUNE_MAGIC)
		haveTelemetry = RSTuneSeqRead(g_shm->telSeq, g_shm->telemetry, t);

	if (!haveTelemetry)
	{
		SetDlgItemTextW(g_main, IDC_GAMESTATE, L"Game: not running");
		SetDlgItemTextW(g_main, IDC_LEVEL, L"Input: -");
		SetDlgItemTextW(g_main, IDC_DETECTED, L"Detected: -");
		SetDlgItemTextW(g_main, IDC_LATENCY, L"Added latency: -");
		return;
	}

	// the audio thread bumps the heartbeat every block, so a frozen value means the
	// game is closed or its audio stream is not running
	if (t.heartbeat == g_lastHeartbeat)
	{
		if (g_staleTicks < 100) ++g_staleTicks;
	}
	else
	{
		g_staleTicks = 0;
		g_lastHeartbeat = t.heartbeat;
	}
	const bool live = (g_staleTicks < 5);

	if (!live)
	{
		SetDlgItemTextW(g_main, IDC_GAMESTATE, L"Game: not running");
		SetDlgItemTextW(g_main, IDC_LEVEL, L"Input: -");
		SetDlgItemTextW(g_main, IDC_DETECTED, L"Detected: -");
		SetDlgItemTextW(g_main, IDC_LATENCY, L"Added latency: -");
		return;
	}

	swprintf_s(buf, L"Game: connected   %.0f Hz, %d frames", t.sampleRate, t.blockFrames);
	SetDlgItemTextW(g_main, IDC_GAMESTATE, buf);

	if (t.inputPeakDb < -90.0f)
		swprintf_s(buf, L"Input: silent");
	else
		swprintf_s(buf, L"Input: %.1f dB", t.inputPeakDb);
	SetDlgItemTextW(g_main, IDC_LEVEL, buf);

	if (t.detectedHz > 1.0f)
		swprintf_s(buf, L"Detected: %.1f Hz in", t.detectedHz);
	else
		swprintf_s(buf, L"Detected: -");
	SetDlgItemTextW(g_main, IDC_DETECTED, buf);

	swprintf_s(buf, L"Added latency: %.1f ms   CPU: %.1f%%", t.addedLatencyMs, t.cpuPercent);
	SetDlgItemTextW(g_main, IDC_LATENCY, buf);
}

// ---------------------------------------------------------------------------
// window
// ---------------------------------------------------------------------------
static HWND MakeLabel(HWND parent, const wchar_t* text, int x, int y, int w, int h, int id = -1, HFONT font = nullptr)
{
	HWND h2 = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE,
	                          x, y, w, h, parent, (HMENU)(INT_PTR)id, g_inst, nullptr);
	SendMessageW(h2, WM_SETFONT, (WPARAM)(font ? font : g_font), TRUE);
	return h2;
}

static void CreateControls(HWND hwnd)
{
	const int lx = 16, cx = 150, cw = 268;
	int y = 14;

	MakeLabel(hwnd, L"My guitar is tuned to", lx, y + 3, 130, 18);
	g_curCombo = CreateWindowExW(0, L"COMBOBOX", nullptr,
		WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
		cx, y, cw, 300, hwnd, (HMENU)IDC_CURRENT, g_inst, nullptr);
	SendMessageW(g_curCombo, WM_SETFONT, (WPARAM)g_font, TRUE);

	y += 32;
	MakeLabel(hwnd, L"Play songs written in", lx, y + 3, 130, 18);
	g_tgtCombo = CreateWindowExW(0, L"COMBOBOX", nullptr,
		WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
		cx, y, cw, 300, hwnd, (HMENU)IDC_TARGET, g_inst, nullptr);
	SendMessageW(g_tgtCombo, WM_SETFONT, (WPARAM)g_font, TRUE);

	y += 38;
	MakeLabel(hwnd, L"", lx, y, 402, 22, IDC_SHIFT, g_fontBold);

	y += 30;
	g_enable = CreateWindowExW(0, L"BUTTON", L"Shift my guitar to the target tuning",
		WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
		lx, y, 300, 22, hwnd, (HMENU)IDC_ENABLE, g_inst, nullptr);
	SendMessageW(g_enable, WM_SETFONT, (WPARAM)g_font, TRUE);

	y += 32;
	MakeLabel(hwnd, L"Quality", lx, y + 3, 130, 18);
	g_quality = CreateWindowExW(0, L"COMBOBOX", nullptr,
		WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
		cx, y, 150, 200, hwnd, (HMENU)IDC_QUALITY, g_inst, nullptr);
	SendMessageW(g_quality, WM_SETFONT, (WPARAM)g_font, TRUE);
	SendMessageW(g_quality, CB_ADDSTRING, 0, (LPARAM)L"Tight  (least latency)");
	SendMessageW(g_quality, CB_ADDSTRING, 0, (LPARAM)L"Balanced");
	SendMessageW(g_quality, CB_ADDSTRING, 0, (LPARAM)L"Smooth  (best on chords)");


	y += 36;
	MakeLabel(hwnd, L"", lx, y, 402, 18, IDC_GAMESTATE);
	y += 20;
	MakeLabel(hwnd, L"", lx, y, 200, 18, IDC_LEVEL);
	MakeLabel(hwnd, L"", cx + 60, y, 220, 18, IDC_DETECTED);
	y += 20;
	MakeLabel(hwnd, L"", lx, y, 402, 18, IDC_LATENCY);

	HWND launch = CreateWindowExW(0, L"BUTTON", L"Launch Rocksmith",
		WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
		306, y - 4, 112, 26, hwnd, (HMENU)IDC_LAUNCH, g_inst, nullptr);
	SendMessageW(launch, WM_SETFONT, (WPARAM)g_font, TRUE);

	y += 26;
	MakeLabel(hwnd,
		L"Leave this open while you play. Rocksmith's own tuner will read the target "
		L"tuning, so tune your guitar normally and let RSTune do the rest.",
		lx, y, 402, 40, IDC_HINT);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg)
	{
	case WM_CREATE:
	{
		// Controls raise notifications while they are being created and while settings
		// are loaded into them. Both would reach the save path before the state is
		// complete and overwrite the settings file with partial values, so gate the
		// whole of WM_CREATE.
		g_loading = true;

		g_main = hwnd;
		CreateControls(hwnd);

		for (int i = 0; i < kNumRSTunings; ++i)
		{
			wchar_t label[128];
			swprintf_s(label, L"%s   (%s)", kRSTunings[i].name, kRSTunings[i].notes);
			SendMessageW(g_curCombo, CB_ADDSTRING, 0, (LPARAM)label);
		}

		g_curIndex = IniGet(L"CurrentTuning", 0);
		if (g_curIndex < 0 || g_curIndex >= kNumRSTunings) g_curIndex = 0;
		g_tgtIndex = IniGet(L"TargetTuning", g_curIndex);
		if (g_tgtIndex < 0 || g_tgtIndex >= kNumRSTunings) g_tgtIndex = g_curIndex;

		SendMessageW(g_curCombo, CB_SETCURSEL, g_curIndex, 0);
		RefreshTargets();

		int q = IniGet(L"Quality", RSTuneQuality_Balanced);
		if (q < 0 || q >= RSTuneQuality_Count) q = RSTuneQuality_Balanced;
		SendMessageW(g_quality, CB_SETCURSEL, q, 0);

		Button_SetCheck(g_enable, IniGet(L"Enabled", 0) ? BST_CHECKED : BST_UNCHECKED);

		g_loading = false;

		UpdateShiftLabel();
		PushControl();
		UpdateStatus();
		SetTimer(hwnd, 1, 150, nullptr);
		return 0;
	}

	case WM_COMMAND:
	{
		const int id = LOWORD(wp);
		const int code = HIWORD(wp);

		if (g_loading || !g_enable || !g_quality)
			return 0;

		if (id == IDC_CURRENT && code == CBN_SELCHANGE)
		{
			g_curIndex = (int)SendMessageW(g_curCombo, CB_GETCURSEL, 0, 0);
			RefreshTargets();
			UpdateShiftLabel();
			PushControl();
			SaveSettings();
		}
		else if (id == IDC_TARGET && code == CBN_SELCHANGE)
		{
			const int sel = (int)SendMessageW(g_tgtCombo, CB_GETCURSEL, 0, 0);
			g_tgtIndex = (int)SendMessageW(g_tgtCombo, CB_GETITEMDATA, sel, 0);
			UpdateShiftLabel();
			PushControl();
			SaveSettings();
		}
		else if (id == IDC_ENABLE || (id == IDC_QUALITY && code == CBN_SELCHANGE))
		{
			PushControl();
			SaveSettings();
		}
		else if (id == IDC_LAUNCH)
		{
			ShellExecuteW(hwnd, L"open", L"steam://rungameid/221680", nullptr, nullptr, SW_SHOWNORMAL);
		}
		return 0;
	}

	case WM_TIMER:
		UpdateStatus();
		return 0;

	case WM_CTLCOLORSTATIC:
		SetBkMode((HDC)wp, TRANSPARENT);
		return (LRESULT)GetSysColorBrush(COLOR_WINDOW);

	case WM_ERASEBKGND:
	{
		RECT rc;
		GetClientRect(hwnd, &rc);
		FillRect((HDC)wp, &rc, GetSysColorBrush(COLOR_WINDOW));
		return 1;
	}

	case WM_CLOSE:
		SaveSettings();
		DestroyWindow(hwnd);
		return 0;

	case WM_DESTROY:
		KillTimer(hwnd, 1);
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int show)
{
	g_inst = inst;
	SettingsPath();

	// a second copy would fight over the control block
	HANDLE once = CreateMutexW(nullptr, FALSE, L"RSTune_single_instance");
	if (once && GetLastError() == ERROR_ALREADY_EXISTS)
	{
		HWND existing = FindWindowW(L"RSTuneMain", nullptr);
		if (existing) { ShowWindow(existing, SW_RESTORE); SetForegroundWindow(existing); }
		return 0;
	}

	INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_UPDOWN_CLASS | ICC_STANDARD_CLASSES };
	InitCommonControlsEx(&icc);

	if (!ShmOpen())
	{
		MessageBoxW(nullptr, L"Could not create the RSTune shared control block.",
		            L"RSTune", MB_ICONERROR | MB_OK);
		return 1;
	}

	NONCLIENTMETRICSW ncm = { sizeof(ncm) };
	SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
	g_font = CreateFontIndirectW(&ncm.lfMessageFont);
	ncm.lfMessageFont.lfWeight = FW_SEMIBOLD;
	g_fontBold = CreateFontIndirectW(&ncm.lfMessageFont);

	WNDCLASSEXW wc = { sizeof(wc) };
	wc.lpfnWndProc = WndProc;
	wc.hInstance = inst;
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	wc.lpszClassName = L"RSTuneMain";
	wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
	RegisterClassExW(&wc);

	RECT rc = { 0, 0, 434, 386 };
	AdjustWindowRect(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);

	HWND hwnd = CreateWindowExW(0, L"RSTuneMain", L"RSTune",
		WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
		CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
		nullptr, nullptr, inst, nullptr);
	if (!hwnd)
		return 1;

	ShowWindow(hwnd, show);
	UpdateWindow(hwnd);

	MSG msg;
	while (GetMessageW(&msg, nullptr, 0, 0) > 0)
	{
		if (!IsDialogMessageW(hwnd, &msg))
		{
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
	}

	ShmClose();
	if (once) CloseHandle(once);
	return 0;
}
