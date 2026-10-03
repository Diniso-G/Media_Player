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


//Playback control
static void StartTimers() {
	SetTimer(g_hwndMain, TIMER_PROGRESS, 100, nullptr);
	if (!g_hasVideo) SetTimer(g_hwndMain, TIMER_ANIM, 40, nullptr);
}

static void KillTimers() {
	KillTimer(g_hwndMain, TIMER_PROGRESS);
	KillTimer(g_hwndMain, TIMER_ANIM);
}

static void ApplyVolume() {
	if (g_pVolume) {
		g_pVolume->SetMasterVolume(g_volume);
		g_pVolume->SetMute(g_muted ? TRUE : FALSE);
	}
}

static void SetVolume(float v) {
	g_volume = Clamp01(v);
	if (g_volume > 0.0f) g_muted = false;
	ApplyVolume();
	InvalidateControls();
}

static void ToggleMute() { 
	g_muted = !g_muted;
	ApplyVolume();
	InvalidateControls();
}

static void ApplyRate() {
	if (g_pRate) g_pRate->SetRate(FALSE, kRates[g_rateIdx]);
}

static void SetRateIndex(int i) {
	g_rateIdx = max(0, min(kRateCount - 1, i));
	ApplyRate();
	InvalidateControls();
}

static void SessionStart(const MFTIME* pos) {
	PROPVARIANT v;
	PropVariantInit(&v);
	if (pos) {
		v.vt = VT_I8;
		v.hVal.QuadPart = *pos;
	}
	g_pSession->Start(&GUID_NULL, &v);
	PropVariantClear(&v);
}

static void UpdateTitle() {
	std::wstring t = g_title.empty() ? L"Media Player" : g_title + L" - Media Player";
	if (g_playlist.size() > 1) {
		wchar_t n[32];
		swprintf_s(n, L"  (%d/%d)", g_index + 1, (int)g_playlist.size());
		t += n;
	}
	SetWindowTextW(g_hwndMain, t.c_str());
}

static void CloseSession() {
	KillTimers();
	g_gen++;

	SafeRelease(&g_pVideoControl);
	SafeRelease(&g_pVolume);
	SafeRelease(&g_pRate);

	if (g_pSession) {
		g_pSession->Close();
		if (g_pCallback) WaitForSingleObject(g_pCallback->ClosedEvent(), 2000);
	}

	if (g_pSource) {
		g_pSource->Shutdown();
		SafeRelease(&g_pSource);
	}

	if (g_pSession) {
		g_pSession->Shutdown();
		SafeRelease(&g_pSession);
	}

	if (g_pCallback) {
		g_pCallback->Detach();
		SafeRelease(&g_pCallback);
	}

	g_state = PlayerState::CLOSED;
	g_duration = 0;
	g_currentPos = 0;
	g_hasVideo = false;
	g_pauseAfterStart = false;
	Layout();
	InvalidateControls();
}

static HRESULT OpenURL(const std::wstring& url) {
	CloseSession();
	g_error.clear();
	g_title = FileNameOf(url);

	HRESULT hr = MFCreateMediaSession(nullptr, &g_pSession);

	if (SUCCEEDED(hr)) {
		g_gen++;
		g_pCallback = new SessionCallback(g_hwndMain, g_pSession, g_gen);
		hr = g_pSession->BeginGetEvent(g_pCallback, nullptr);
	}
	if (SUCCEEDED(hr)) {
		Com<IMFSourceResolver> resolver;
		Com<IUnknown> unk;
		MF_OBJECT_TYPE type = MF_OBJECT_INVALID;

		hr = MFCreateSourceResolver(resolver.put());
		if (SUCCEEDED(hr)) {
			hr = resolver->CreateObjectFromURL(url.c_str(), MF_RESOLUTION_MEDIASOURCE | MF_RESOLUTION_CONTENT_DOES_NOT_HAVE_TO_MATCH_EXTENSION_OR_MIME_TYPE, nullptr, &type, unk.put());
		}
		if (SUCCEEDED(hr)) hr = unk->QueryInterface(IID_PPV_ARGS(&g_pSource));
	}
	if (SUCCEEDED(hr)) {
		Com<IMFTopology> topo;
		bool hasVideo = false;
		hr = CreateTopology(topo.put(), &hasVideo);

		if (SUCCEEDED(hr)) {
			g_hasVideo = hasVideo;
			hr = g_pSession->SetTopology(0, topo.get());
		}
	}
	if (SUCCEEDED(hr)) {
		g_state = PlayerState::OPEN_PENDING;
		Layout();
	}
	else {
		std::wstring name = g_title;
		CloseSession();
		g_title = name;
		g_error = L"This file couldn't be opened. The format or codec may bot be supported.";
	}
	UpdateTitle();
	InvalidateControls();
	InvalidateRect(g_hwndMain, nullptr, FALSE);
	return hr;
}

static void PlayIndex(int i) {
	if (i < 0 || i >= (int)g_playlist.size()) return;
	g_index = i;
	AddRecent(g_playlist[i]);
	OpenURL(g_playlist[i]);
}

static void SetPlaylist(const std::vector<std::wstring>& files) {
	if (files.empty()) return;
	g_playlist = files;
	PlayIndex(0);
}

static void Stop() {
	if (!CanControl() || g_state == PlayerState::STOPPED) return;
	g_pauseAfterStart = false;
	g_pSession->Stop();
	g_state = PlayerState::STOPPED;
	g_currentPos = 0;
	KillTimers();
	InvalidateControls();
	InvalidateStage();
}

static void Play() {
	if (!CanControl()) {
		if (g_state == PlayerState::CLOSED && g_index >= 0) PlayIndex(g_index);
		return;
	}
	if (g_state == PlayerState::STOPPED) return;
	g_pauseAfterStart = false;
	MFTIME zero = 0;
	if (g_state == PlayerState::STOPPED) SessionStart(&zero);
	else SessionStart(nullptr);
	g_state = PlayerState::STARTED;
	StartTimers();
	InvalidateControls();
}

static void Pause() {
	if (!CanControl() || g_state != PlayerState::STARTED) return;
	g_pSession->Pause();
	g_state = PlayerState::PAUSED;
	KillTimer(g_hwndMain, TIMER_PROGRESS);
	InvalidateControls();
	InvalidateStage();
}

static void TogglePlay() {
	if (g_state == PlayerState::STARTED) Pause();
	else Play();
}

static void SeekTo(MFTIME pos) {
	if (!CanControl() || g_duration <= 0) return;
	if (pos < 0) pos = 0;
	if (pos > g_duration) pos = g_duration;
	g_currentPos = pos;
	if (g_state == PlayerState::PAUSED) g_pauseAfterStart = true;
	SessionStart(&pos);
	if (g_state == PlayerState::STOPPED) {
		g_state = PlayerState::STARTED;
		StartTimers();
	}
	InvalidateControls();
}

static void SkipBy(double seconds) {
	SeekTo(g_currentPos + (MFTIME)(seconds * 10000000.0));
}

static void NextTrack() {
	if (g_index + 1 < (int)g_playlist.size()) PlayIndex(g_index + 1);
}

static void PrevTrack() {
	if (g_currentPos > 30000000LL || g_index <= 0) {
		SeekTo(0);
		return;
	}
	PlayIndex(g_index - 1);
}

static void UpdateProgress() {
	if (!CanControl() || g_duration <= 0 || g_state != PlayerState::STARTED || g_dragSeek) return;
	Com<IMFClock> clock;
	if (FAILED(g_pSession->GetClock(clock.put()))) return;
	Com<IMFPresentationClock> pc;
	if (FAILED(clock->QueryInterface(IID_PPV_ARGS(pc.put())))) return;
	MFTIME pos = 0;
	if (SUCCEEDED(pc->GetTime(&pos))) {
		if (pos > g_duration) pos = g_duration;
		g_currentPos = pos;
		InvalidateControls();
	}
}

static void HandleSessionEvent(IMFMediaEvent* ev) {
	MediaEventType type = MEUnknown;
	ev->GetType(&type);

	switch (type) {
	case MESessionTopologyStatus: {
		UINT32 status = 0;
		ev->GetUINT32(MF_EVENT_TOPOLOGY_STATUS, &status);

		if (status != MF_TOPOSTATUS_READY) break; 
		
		Com<IMFGetService> gs;
		if (SUCCEEDED(g_pSession->QueryInterface(IID_PPV_ARGS(gs.put())))) {
			gs->GetService(MR_VIDEO_RENDER_SERVICE, IID_PPV_ARGS(&g_pVideoControl));
			gs->GetService(MR_POLICY_VOLUME_SERVICE, IID_PPV_ARGS(&g_pVolume));
			gs->GetService(MF_RATE_CONTROL_SERVICE, IID_PPV_ARGS(&g_pRate));
		}

		if (g_pVideoControl) {
			g_pVideoControl->SetBorderColor(RGB(0, 0, 0));
			RECT vr;
			GetClientRect(g_hwndVideo, &vr);
			g_pVideoControl->SetVideoPosition(nullptr, &vr);
		}
		Com<IMFPresentationDescriptor> pd;
		if (SUCCEEDED(g_pSource->CreatePresentationDescriptor(pd.put()))) {
			UINT64 d = 0;
			if (SUCCEEDED(pd->GetUINT64(MF_PD_DURATION, &d))) g_duration = (MFTIME)d;
		}
		ApplyVolume();
		ApplyRate();
		g_currentPos = 0;
		SessionStart(nullptr);
		g_state = PlayerState::STARTED;
		StartTimers();
		InvalidateControls();
		InvalidateRect(g_hwndMain, nullptr, FALSE);
		break;
	}

	case MESessionStarted:
		if (g_pauseAfterStart) {
			g_pauseAfterStart = false;
			g_pSession->Pause();
		}
		break;

	case MESessionEnded: {
		KillTimers();
		if (g_index + 1 < (int)g_playlist.size()) {
			PlayIndex(g_index + 1);
		}
		else {
			g_state = PlayerState::STOPPED;
			g_currentPos = 0;
			InvalidateControls();
			InvalidateStage();
		}
		break;
	}

	case MEError: {
		HRESULT hrStatus = S_OK;
		ev->GetStatus(&hrStatus);
		std::wstring name = g_title;
		CloseSession();
		g_title = name;
		wchar_t msg[160];
		swprintf_s(msg, L"Playback error: 0x%08X", (unsigned)hrStatus);
		g_error = msg;
		InvalidateRect(g_hwndMain, nullptr, FALSE);
		break;
	}
	default:
		break;
	}
}


//Open file dialog(allow for multi select)
static std::vector<std::wstring> OpenMediaFiles(HWND owner) {
	std::vector<std::wstring> result;
	std::vector<wchar_t> buf(32768, 0);
	OPENFILENAME ofn = {};

	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = owner;
	ofn.lpstrFile = buf.data();
	ofn.nMaxFile = (DWORD)buf.size();
	ofn.lpstrFilter = 
		L"Media Files\0*.mp4;*.m4v;*.mov;*.mkv;*.avi;*.wmv;*.mp3;*.m4a;*.aac;*.wav;*.wma;*.flac\0" 
		L"Video Files\0*.mp4;*.m4v;*.mov;*.mkv;*.avi;*.wmv\0"
		L"Audio Files\0*.mp3;*.m4a;*.aac;*.wav;*.wma;*.flac\0"
		L"All Files\0*.*\0";
	ofn.Flags = OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;

	if (!g_lastFolder.empty())
		ofn.lpstrInitialDir = g_lastFolder.c_str();

	if (!GetOpenFileName(&ofn))
		return result;

	std::wstring first(buf.data());
	const wchar_t* p = buf.data() + first.size() + 1;
	if (*p == 0) {
		result.push_back(first);
		size_t slash = first.find_last_of(L"\\/");
		if (slash != std::wstring::npos)
			g_lastFolder = first.substr(0, slash);
	}
	else {
		g_lastFolder = first;
		while (*p) {
			std::wstring name(p);
			result.push_back(first + L"\\" + name);
			p += name.size() + 1;
		}
	}
	return result;
}


//GDI nad drawing helpers and icons
static const FontFamily* UIFamily() {
	static FontFamily* fam = nullptr;
	if (!fam) {
		fam = new FontFamily(L"Segoe UI");
		if (!fam->IsAvailable()) {
			delete fam;
			fam = FontFamily::GenericSansSerif()->Clone();
		}
	}
	return fam;
}
  
static void RoundPath(GraphicsPath& p, const RectF& r, float rad) {
	rad = min(rad, min(r.Width, r.Height) / 2.0f);
	if (rad <= 0.5f) {
		p.AddRectangle(r);
		return;
	}
	float d = rad * 2;
	p.AddArc(r.X, r.Y, d, d, 180, 90);
	p.AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
	p.AddArc(r.GetRight() - d, r.GetBottom()- d, d, d, 0, 90);
	p.AddArc(r.X, r.GetBottom() - d, d, d, 90, 90);
	p.CloseFigure();
}

static void FillRound(Graphics& g, const Brush& b, const RectF& r, float rad) {
	GraphicsPath p;
	RoundPath(p, r, rad);
	g.FillPath(&b, &p);
}

static void DrawLabel(Graphics& g, const std::wstring& s, float px, FontStyle style, const RectF& r, const Color& col, StringAlignment h = StringAlignmentNear, StringAlignment v = StringAlignmentCenter) {
	Font font(UIFamily(), px, style, UnitPixel);
	StringFormat fmt;
	fmt.SetAlignment(h);
	fmt.SetLineAlignment(v);
	fmt.SetTrimming(StringTrimmingEllipsisCharacter);
	fmt.SetFormatFlags(StringFormatFlagsNoWrap);
	SolidBrush br(col);
	g.DrawString(s.c_str(), -1, &font, r, &fmt, &br);
}

static void DrawGlyph(Graphics& g, int id, const RectF& r, Color col) {
	float cx = r.X + r.Width / 2, cy = r.Y + r.Height / 2;
	float s = min(r.Width, r.Height) * 0.5f * 0.5f;
	SolidBrush fill(col);

	Pen pen(col, max(1.5f, SF(1.8f)));
	pen.SetLineJoin(LineJoinRound);
	pen.SetStartCap(LineCapRound);
	pen.SetEndCap(LineCapRound);

	switch (id) {
	case B_PLAY: {
		if (g_state == PlayerState::STARTED) {
			FillRound(g, fill, RectF(cx - 0.95f * s, cy - s, 0.7f * s, 2 * s), s * 0.2f);
			FillRound(g, fill, RectF(cx - 0.25f * s, cy - s, 0.7f * s, 2 * s), s * 0.2f);
		}
		else {
			PointF pts[3] = {
				PointF(cx - 0.6f * s + 0.15f * s, cy - 1.05f * s),
				PointF(cx - 0.6f * s + 0.15f * s, cy + 1.05f * s),
				PointF(cx + 1.0f * s + 0.15f * s, cy)
			};
			g.FillPolygon(&fill, pts, 3);
		}
		break;
	}
	case B_STOP: {
		FillRound(g, fill, RectF(cx - 0.85f * s, cy - 0.85f * s, 1.7f * s, 1.7f * s), s * 0.25f);
		break;
	}
	case B_PREV: {
		FillRound(g, fill, RectF(cx - 0.1f * s, cy - 0.9F * s, 0.3f * s, 1.8F * s), s * 0.1f);
		PointF pts[3] = {
				PointF(cx + 1.0f * s, cy - 0.9f * s),
				PointF(cx + 1.0f * s, cy + 0.9f * s),
				PointF(cx - 0.55f * s, cy)
		};
		g.FillPolygon(&fill, pts, 3);
		break;
	}
	case B_NEXT: {
		FillRound(g, fill, RectF(cx + 0.7f * s, cy - 0.9F * s, 0.3f * s, 1.8F * s), s * 0.1f);
		PointF pts[3] = {
				PointF(cx - 1.0f * s, cy - 0.9f * s),
				PointF(cx - 1.0f * s, cy + 0.9f * s),
				PointF(cx + 0.55f * s, cy)
		};
		g.FillPolygon(&fill, pts, 3);
		break;
	}
	case B_OPEN: {
		PointF pts[6] = {
				PointF(cx - 1.1f * s, cy + 0.9f * s),
				PointF(cx - 1.1f * s, cy - 0.5f * s),
				PointF(cx - 0.2f * s, cy - 0.9f * s),
				PointF(cx + 0.1f * s, cy - 0.5f * s),
				PointF(cx + 1.1f * s, cy - 0.5f * s),
				PointF(cx + 1.1f * s, cy + 0.9f * s)
		};
		g.DrawPolygon(&pen, pts, 6);
		break;
	}
	case B_MUTE: {
		float ox = cx = 0.5f * s;
		FillRound(g, fill, RectF(ox - 0.1f * s, cy - 0.5f * s, 0.6f * s, 1.0f * s), s * 0.1f);
		PointF cone[4] = {
				PointF(ox - 0.45f * s, cy - 0.5f * s),
				PointF(ox + 0.4f * s, cy - 1.1f * s),
				PointF(ox + 0.4f * s, cy + 1.1f * s),
				PointF(ox - 0.45f * s, cy + 0.5f * s)
		};
		g.FillPolygon(&fill, cone, 4);
		if (g_muted || g_volume <= 0.0f) {
			g.DrawLine(&pen, ox + 0.8f * s, cy - 0.5f * s, ox + 1.7f * s, cy + 0.5f * s);
			g.DrawLine(&pen, ox + 0.8f * s, cy + 0.5f * s, ox + 1.7f * s, cy - 0.5f * s);
		}
		else {
			float r1 = 0.7f * s, r2 = 1.3f * s;
			g.DrawArc(&pen, ox + 0.4f * s - r1 + 0.2f * s, cy - r1, 2 * r1, 2 * r1, -50, 100);
			if (g_volume > 0.45f)
				g.DrawArc(&pen, ox + 0.4f * s - r2 + 0.2f * s, cy - r2, 2 * r2, 2 * r2, -50, 100);
		}
		break;
	}
	case B_FS: {
		float a = 1.0f * s, b = 0.45f * s;
		const float sx[4] = { -1, 1, 1, -1 }, sy[4] = { -1, -1, 1, 1 };
		for (int i = 0; i < 4; i++) {
			PointF pts[3];
			if (!g_fullscreen) {
				pts[0] = PointF(cx + sx[i] * b, cy + sy[i] * a);
				pts[1] = PointF(cx + sx[i] * a, cy + sy[i] * a);
				pts[2] = PointF(cx + sx[i] * a, cy + sy[i] * b);
			}
			else {
				pts[0] = PointF(cx + sx[i] * a, cy + sy[i] * b);
				pts[1] = PointF(cx + sx[i] * b, cy + sy[i] * b);
				pts[2] = PointF(cx + sx[i] * b, cy + sy[i] * a);
			}
			g.DrawLines(&pen, pts, 3);
		}
		break;
	}
	}
}
