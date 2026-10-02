//Headers, constants, colours
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

//Smart pointer, global state and helpers
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
static int SI(float v) { return (int)(SF(v) + 0.5f); }
static float Clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

static std::wstring FormatTime(MFTIME t) {
	if (t < 0) t = 0;
	long long secs = t / 10000000LL;
	wchar_t buf[32];
	if (secs >= 3600)
		swprintf_s(buf, 32, L"%02I64d:%02I64d:%02I64d", secs / 3600, (secs / 60) % 60, secs % 60);
	else 
		swprintf_s(buf, 32, L"%02I64d:%02I64d", secs / 60, secs % 60);
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


//Settings(volume and recent files)
static const wchar_t* kRegKey = L"Software\\DesktopMediaPlayer";

static void LoadSettings() {
	HKEY k;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return;
	DWORD v = 0, sz = sizeof(v), type = 0;
	if (RegQueryValueExW(k, L"Volume", nullptr, &type, (LPBYTE)&v, &sz) == ERROR_SUCCESS && type == REG_DWORD)
		g_volume = Clamp01(v / 100.0f);
	for (int i = 0; i < kMaxRecent; i++) {
		wchar_t name[16], buf[1024];
		swprintf_s(name, L"Recent%d", i);
		DWORD bsz = sizeof(buf);
		if (RegQueryValueExW(k, name, nullptr, &type, (LPBYTE)buf, &bsz) == ERROR_SUCCESS && type == REG_SZ && buf[0])
			g_recent.push_back(buf);
	}
	RegCloseKey(k);
}

static void SaveSettings() {
	HKEY k;
	if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
	DWORD v = (DWORD)(g_volume * 100.0f + 0.5f);
	RegSetValueExW(k, L"Volume", 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
	for (int i = 0; i < kMaxRecent; i++) {
		wchar_t name[16];
		swprintf_s(name, L"Recent%d", i);
		if (i < (int)g_recent.size())
			RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)g_recent[i].c_str(), (DWORD)((g_recent[i].size() + 1) * sizeof(wchar_t)));
		else
			RegDeleteValueW(k, name);
	}
	RegCloseKey(k);
}

static void AddRecent(const std::wstring& path) {
	g_recent.erase(std::remove(g_recent.begin(), g_recent.end(), path), g_recent.end());
	g_recent.insert(g_recent.begin(), path);
	if ((int)g_recent.size() > kMaxRecent) g_recent.resize(kMaxRecent);
	SaveSettings();
}


//Media foundation session callback
class SessionCallback final : public IMFAsyncCallback {
public:
	SessionCallback(HWND hwnd, IMFMediaSession* s, DWORD gen) : m_hwnd(hwnd), m_session(s), m_gen(gen), m_cRef(1) {
		m_session->AddRef();
		InitializeCriticalSection(&m_cs);
		m_closed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	}
	~SessionCallback() {
		CloseHandle(m_closed);
		DeleteCriticalSection(&m_cs);
	}
	HANDLE ClosedEvent() const { return m_closed; }
	void Detach() {
		EnterCriticalSection(&m_cs);
		SafeRelease(&m_session);
		LeaveCriticalSection(&m_cs);
	}

	STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
		if (riid == IID_IUnknown || riid == __uuidof(IMFAsyncCallback)) {
			*ppv = static_cast<IMFAsyncCallback*>(this);
			AddRef();
			return S_OK;
		}
		*ppv = nullptr;
		return E_NOINTERFACE;
	}

	STDMETHODIMP_(ULONG) AddRef() override {
		return InterlockedIncrement(&m_cRef);
	}

	STDMETHODIMP_(ULONG) Release() override {
		ULONG n = InterlockedDecrement(&m_cRef);
		if (n == 0) delete this;
		return n;
	}

	STDMETHODIMP GetParameters(DWORD*, DWORD*) override {
		return E_NOTIMPL;
	}

	STDMETHODIMP Invoke(IMFAsyncResult* pResult) override {
		IMFMediaSession* s = nullptr;
		EnterCriticalSection(&m_cs);
		if (m_session) { 
			s = m_session; 
			s->AddRef(); 
		}
		LeaveCriticalSection(&m_cs);
		if (!s) return S_OK;

		IMFMediaEvent* ev = nullptr;
		if (SUCCEEDED(s->EndGetEvent(pResult, &ev))) {
			MediaEventType t = MEUnknown;
			ev->AddRef();

			if (t == MESessionClosed) SetEvent(m_closed);
			if (!PostMessage(m_hwnd, WM_APP_SESSION_EVENT, (WPARAM)m_gen, (LPARAM)ev))
				ev->Release();
			if (t != MESessionClosed) s->BeginGetEvent(this, nullptr);
		}
		s->Release();
		return S_OK;
	}
private:
	HWND m_hwnd;
	IMFMediaSession* m_session;
	DWORD m_gen;
	LONG m_cRef;
	CRITICAL_SECTION m_cs;
	HANDLE m_closed;
};

static SessionCallback* g_pCallback = nullptr;


//Topology
static HRESULT AddBranch(IMFTopology* topo, IMFPresentationDescriptor* pd, IMFStreamDescriptor* sd, HWND videoHwnd, bool* isVideo) {
	
	Com<IMFMediaTypeHandler> handler;
	HRESULT hr = sd->GetMediaTypeHandler(handler.put());
	if (FAILED(hr)) return hr;

	GUID major = {};
	hr = handler->GetMajorType(&major);
	if (FAILED(hr)) return hr;

	Com<IMFActivate> activate;
	if (major == MFMediaType_Audio)
		hr = MFCreateAudioRendererActivate(activate.put());
	else if (major == MFMediaType_Video) {
		hr = MFCreateVideoRendererActivate(videoHwnd, activate.put());
		if (isVideo) *isVideo = true;
	}
	else
		return MF_E_INVALIDMEDIATYPE;

	if (FAILED(hr)) return hr;

	Com<IMFTopologyNode> src, sink;
	hr = MFCreateTopologyNode(MF_TOPOLOGY_SOURCESTREAM_NODE, src.put());
	if (FAILED(hr)) return hr;

	src->SetUnknown(MF_TOPONODE_SOURCE, g_pSource);
	src->SetUnknown(MF_TOPONODE_PRESENTATION_DESCRIPTOR, pd);
	src->SetUnknown(MF_TOPONODE_STREAM_DESCRIPTOR, sd);

	hr = MFCreateTopologyNode(MF_TOPOLOGY_OUTPUT_NODE, sink.put());
	if (FAILED(hr)) return hr;

	sink->SetObject(activate.get());

	topo->AddNode(src.get());
	topo->AddNode(sink.get());
	return src->ConnectOutput(0, sink.get(), 0);
}

static HRESULT CreateTopology(IMFTopology** ppTopo, bool* hasVideo) {
	Com<IMFTopology> topo;
	HRESULT hr = MFCreateTopology(topo.put());
	if (FAILED(hr)) return hr;

	Com<IMFPresentationDescriptor> pd;
	hr = g_pSource->CreatePresentationDescriptor(pd.put());
	if (FAILED(hr)) return hr;
	DWORD count = 0;
	pd->GetStreamDescriptorCount(&count);
	int connected = 0;
	*hasVideo = false;
	for (DWORD i = 0; i < count; i++) {
		BOOL selected = FALSE;
		Com<IMFStreamDescriptor> sd;
		if (SUCCEEDED(pd->GetStreamDescriptorByIndex(i, &selected, sd.put())) && selected) {
			bool isVideo = false;
			if (SUCCEEDED(AddBranch(topo.get(), pd.get(), sd.get(), g_hwndVideo, &isVideo))) {
				connected++;
				if (isVideo) 
					*hasVideo = true;
			}
		}
	}
	if (connected == 0) return MF_E_TOPO_CODEC_NOT_FOUND;

	*ppTopo = topo.get();
	(*ppTopo)->AddRef();
	return S_OK;
}


//Layout, fullscreen and autohide
static RECT g_stageRect = {};
static void Layout() {
	if (!g_hwndMain) return;
	RECT rc;
	GetClientRect(g_hwndMain, &rc);
	int w = rc.right, h = rc.bottom;
	int barH = SI(kBarHeight);

	int stageH = g_fullscreen ? h : max(0, h - barH);
	g_stageRect = { 0, 0, w, stageH };

	if (g_hwndVideo) {
		MoveWindow(g_hwndVideo, 0, 0, w, stageH, TRUE);
		ShowWindow(g_hwndVideo, g_hasVideo ? SW_SHOWNA : SW_HIDE);
	}
	if (g_hwndCtl) {
		MoveWindow(g_hwndCtl, 0, h - barH, w, barH, TRUE);
		SetWindowPos(g_hwndCtl, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
		bool show = !g_fullscreen || !g_uiHidden;
		ShowWindow(g_hwndCtl, show ? SW_SHOWNA : SW_HIDE);
	}
	if (g_pVideoControl) {
		RECT vr = { 0, 0, w, stageH };
		g_pVideoControl->SetVideoPosition(nullptr, &vr);
	}
	InvalidateRect(g_hwndMain, nullptr, FALSE);
}

static void ActivityPing() {
	g_lastActivity = GetTickCount64();
	if (g_uiHidden) {
		g_uiHidden = false;
		if (g_hwndCtl && g_fullscreen) ShowWindow(g_hwndCtl, SW_SHOWNA);
		SetCursor(LoadCursor(nullptr, IDC_ARROW));
	}
}

static void ToggleFullscreen() {
	HWND hwnd = g_hwndMain;
	if (!g_fullscreen) {
		g_prevStyle = GetWindowLongPtr(hwnd, GWL_STYLE);
		g_prevPlacement.length = sizeof(g_prevPlacement);
		GetWindowPlacement(hwnd, &g_prevPlacement);
		MONITORINFO mi = { sizeof(mi) };
		GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
		g_fullscreen = true;
		SetWindowLongPtr(hwnd, GWL_STYLE, g_prevStyle & ~WS_OVERLAPPEDWINDOW);
		SetWindowPos(hwnd, g_topmost ? HWND_TOPMOST : HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
	}
	else {
		g_fullscreen = false;
		g_uiHidden = false;
		SetWindowLongPtr(hwnd, GWL_STYLE, g_prevStyle);
		SetWindowPlacement(hwnd, &g_prevPlacement);
		SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
	}
	ActivityPing();
	Layout();
	InvalidateControls();
}




