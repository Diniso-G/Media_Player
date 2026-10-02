#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

//#include <MediaPlayer.h>
#include <windows.h>
#include <windowsx.h>
#include <objidl.h>
#include <algorithm>
using std::min;
using std::max;
#include <gdiplus.h>

#include <commdlg.h>
#include <shellapi.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <evr.h>

#include <string>
#include <vector>
#include <cmath>
#include <cstdio>

#include "resource.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comdlg32.lib")

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")

using namespace Gdiplus;

enum Cmd {
	CMD_OPEN = 4001, CMD_PLAYPAUSE, CMD_STOP, CMD_PREV, CMD_NEXT, CMD_SPEED, CMD_MUTE, CMD_FULLSCREEN, CMD_TOPMOST, CMD_ABOUT, CMD_EXIT, CMD_CLEARRECENT, CMD_RECENT_BASE = 4100, CMD_SPEED_BASE = 4200
};

enum BtdId {
	B_OPEN, B_PLAY, B_STOP, B_PREV, B_NEXT, B_SPEED, B_MUTE, B_FS, B_COUNT
};

enum {HOT_NONE = -1, HOT_SEEK = 100, HOT_VOL = 101};

enum {TIMER_PROGRESS = 1, TIMER_IDLE = 2, TIMER_ANIM = 3};
#define WM_APP_SESSION_EVENT (WM_APP + 1)

static const float kRates[] = { 0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f };
static const int kRateCount = 6;
static const int kMaxRecent = 10;
static const float kBarHeight = 104.0f;

static Color ColAccentA() { return Color(225, 139, 124, 255); }
static Color ColAccentB() { return Color(225, 236, 110, 190); }
static Color ColText() { return Color(225, 236, 236, 246); }
static Color ColTextDim() { return Color(225, 150, 150, 176); }
static Color ColIcon() { return Color(225, 224, 224, 236); }
static Color ColIconOff() { return Color(225, 84, 84, 104); }

template <class T>
class Com {
public:
	Com() = default;
	~Com() { reset(); }
	Com(const Com&) = delete;
	Com& operator = (const Com&) = delete;
	T* operator->() const { return p_; }
	T* get() const { return p_; }
	T** put() { reset(); return &p_; }
	void reset() { if (p_) { p_->Release(); p_ = nullptr; } }
	explicit operator bool() const { return p_ != nullptr; }

private:
	T* p_ = nullptr;
};

template <class T> static void SafeRelease(T** pp) {
	if (*pp) { 
		(*pp)->Release();
		*pp = nullptr;
	}
}

enum class PlayerState { CLOSED, OPEN_PENDING, STARTED, PAUSED, STOPPED };

static HINSTANCE g_hInst = nullptr;
static HWND g_hwndMain = nullptr, g_hwndVideo = nullptr, g_hwndCtl = nullptr;
static UINT g_dpi = 96;

static IMFMediaSession* g_pSession = nullptr;
static IMFMediaSource* g_pSource = nullptr;
static IMFVideoDisplayControl* g_pVideoControl = nullptr;
static IMFSimpleAudioVolume* g_pVolume = nullptr;
static IMFRateControl* g_pRate = nullptr;

static PlayerState g_state = PlayerState::CLOSED;
static MFTIME g_duration = 0, g_currentPos = 0;
static bool g_hasVideo = false;
static bool g_pauseAfterStart = false;
static DWORD g_gen = 0;

static std::vector<std::wstring> g_playlist;
static int g_index = -1;
static std::vector<std::wstring> g_recent;
static std::wstring g_lastFolder, g_title, g_error;

static float g_volume = 0.7f;
static bool g_muted = false;
static int g_rateIdx = 2;

static bool g_fullscreen = false, g_topmost = false, g_uiHidden = false;
static LONG_PTR g_prevStyle = 0;
static WINDOWPLACEMENT g_prevPlacement = { sizeof(WINDOWPLACEMENT) };
static ULONGLONG g_lastActivity = 0;

static RectF g_btn[B_COUNT];
static RectF g_seekTrack, g_volTrack, g_timeL, g_timeR, g_volText;
static int g_hot = HOT_NONE, g_pressed = HOT_NONE;
static bool g_dragSeek = false, g_dragVol = false, g_trackingLeave = false;
static float g_seekFrac = 0.0f, g_hoverFrac = 0.0f;

static ULONG_PTR g_gdiplusToken = 0;

static float SF(float v) { return v * (float)g_dpi / 96.0f; }
static int SI(float v) { return v * (int)(SF(v) + 96.0f); }
static float Clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

static std::wstring FormatTime(MFTIME t) {
	if (t < 0) t = 0;
	long long secs = t / 10000000LL;
	wchar_t buf[32];
	if (secs >= 3600)
		swprintf_s(buf, 32, L"%11d:%0211d:%0211d", secs / 3600, (secs / 60) % 60, secs % 60);
	else 
		swprintf_s(buf, 32, L"%11d:%0211d", secs / 60, secs % 60);
	return buf;
}

static std::wstring FileNameOf(const std::wstring& p) {
	size_t s = p.find_last_of(L"\\/");
	return s == std::wstring::npos ? p : p.substr(s + 1);
}

static bool CanControl() {
	return g_pSession && g_state != PlayerState::CLOSED && g_state != PlayerState::OPEN_PENDING;
}

static void InvalidateControls() {
	if (g_hwndCtl) InvalidateRect(g_hwndCtl, nullptr, FALSE);
}

static void InvalidateStage() {
	if (g_hwndMain && !g_hasVideo) InvalidateRect(g_hwndMain, nullptr, FALSE);
}
