// MediaPlayer.cpp : Defines the entry point for the application.
//

//#include "framework.h"
#define WIN32_LEAN_AND_MEAN
//#include <MediaPlayer.h>
#include <windows.h>
#include <commctrl.h>
#include "resource.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mfreadwrite.h>
#include <evr.h>
#include <string>

#include <commdlg.h>
#include <shellapi.h>

#define IDM_OPEN 101
#define IDM_PLAY 102
#define IDM_PAUSE 103
#define IDM_STOP 104
#define TOOLBAR_H 115

#define CLR_BG  RGB(50, 25, 75)
#define CLR_TOOLBAR  RGB(30, 15, 45)
#define CLR_BUTTON  RGB(45, 45, 45)
#define CLR_BUTTON_HOT  RGB(65, 65, 65)
#define CLR_BUTTON_TXT  RGB(220, 220, 220)
#define CLR_ACCENT  RGB(0, 120, 215)
#define CLR_TIME_TXT  RGB(180, 180, 180)

#pragma comment(lib, "comctl32.lib")

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "strmiids.lib")

#pragma comment(lib, "comdlg32.lib")

#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")



LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

HRESULT CreateTopology(IMFMediaSource*, IMFTopology**, HWND videoHwnd);
HRESULT AddBranchToTopology(IMFTopology*, IMFPresentationDescriptor*, IMFStreamDescriptor*, HWND videoHwnd);

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow) {
    
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    MFStartup(MF_VERSION);
    
    INITCOMMONCONTROLSEX icc = {
        sizeof(icc), ICC_BAR_CLASSES
    };
    InitCommonControlsEx(&icc);
    
    //Register window class
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(CLR_BG);
    wc.lpszClassName = L"MediaPlayerClass";

    HICON hIcon = (HICON)LoadImage(nullptr, L"C:\\Projects\\Media_Player\\MediaPlayer\\MediaPlayer\\icon.ico",
        IMAGE_ICON, 32, 32, LR_LOADFROMFILE);
    HICON hIconSm = (HICON)LoadImage(nullptr, L"C:\\Projects\\Media_Player\\MediaPlayer\\MediaPlayer\\icon.ico",
        IMAGE_ICON, 16, 16, LR_LOADFROMFILE);
    
    wc.hIcon = hIcon;
    wc.hIconSm = hIconSm;

    if (wc.hIcon == nullptr)
        MessageBox(nullptr, L"Icon failed to load", L"Debug", MB_OK);
    RegisterClassExW(&wc);


    HWND hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, L"MediaPlayerClass", L"Media Player", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 900, 650, nullptr, nullptr, hInstance, nullptr
    );
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg = {};
    while (GetMessage(&msg, nullptr, 0, 0)) {

        if (msg.message == WM_KEYDOWN) {
            WPARAM key = msg.wParam;
            if (key == VK_SPACE || key == VK_LEFT || key == VK_RIGHT || key == VK_ESCAPE || key == VK_UP || key == VK_DOWN) {
                SendMessage(hwnd, WM_KEYDOWN, key, msg.lParam);
                continue;
            }
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return (int)msg.wParam;
}

HWND g_hwndSeek = nullptr;
HWND g_hwndVolume = nullptr;
HWND g_hwndTime = nullptr;

HWND g_hwndMain = nullptr;
HWND g_hwndVideo = nullptr;

enum class PlayerState { CLOSED, OPEN_PENDING, STARTED, PAUSED, STOPPED};

IMFMediaSession* g_pSession = nullptr;
IMFMediaSource* g_pSource = nullptr;
IMFVideoDisplayControl* g_pVideoControl = nullptr;
IMFSimpleAudioVolume* g_pVolume = nullptr;
PlayerState g_state = PlayerState::CLOSED;
MFTIME g_duration = 0;
std::wstring   g_lastFolder;

class MediaSessionCallback;
static MediaSessionCallback* g_pCallback = nullptr;

static bool g_bSeeking = false;
static MFTIME g_currentPos = 0;

template<class T> void SafeRelease(T** pp) {
    if (*pp) {
        (*pp)->Release();
        *pp = nullptr;
    }
}

static std::wstring FormatTime(MFTIME t) {
    LONGLONG secs = t / 10000000LL;
    wchar_t buf[32];
    swprintf_s(buf, 32, L"%d:%02d", secs / 60, secs % 60);
    return buf;
}

static void UpdateButtons() {
    bool hasSession = (g_pSession != nullptr && g_state != PlayerState::CLOSED
        && g_state != PlayerState::OPEN_PENDING);

    EnableWindow(GetDlgItem(g_hwndMain, IDM_PLAY),
        hasSession && g_state != PlayerState::STARTED);
    EnableWindow(GetDlgItem(g_hwndMain, IDM_PAUSE),
        hasSession && g_state == PlayerState::STARTED);
    EnableWindow(GetDlgItem(g_hwndMain, IDM_STOP),
        hasSession && (g_state == PlayerState::STARTED || g_state == PlayerState::PAUSED));
    EnableWindow(g_hwndSeek, hasSession);
    EnableWindow(g_hwndVolume, hasSession);
}

static void UpdateProgress() {
    if (!g_pSession || g_state == PlayerState::CLOSED || g_duration == 0)
        return;

    if (g_bSeeking) return;

    IMFClock* pClock = nullptr;
    if (FAILED(g_pSession->GetClock(&pClock))) return;

    IMFPresentationClock* pPClock = nullptr;
    if (SUCCEEDED(pClock->QueryInterface(IID_PPV_ARGS(&pPClock)))) {
        MFTIME pos = 0;
        if (SUCCEEDED(pPClock->GetTime(&pos))) {
            g_currentPos = pos;
            int val = (int)(pos * 1000 / g_duration);
            SendMessage(g_hwndSeek, TBM_SETPOS, TRUE, val);

            std::wstring txt = FormatTime(pos) + L" / " + FormatTime(g_duration);
            SetWindowText(g_hwndTime, txt.c_str());
        }
        pPClock->Release();
    }
    pClock->Release();
}

#define WM_APP_SESSION_EVENT (WM_APP + 1)

class MediaSessionCallback : public IMFAsyncCallback {
public:
    MediaSessionCallback(HWND hwnd) : m_hwnd(hwnd), m_cRef(1) {}
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IMFAsyncCallback) {
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
        ULONG n = _InterlockedDecrement(&m_cRef);
        if (n == 0) delete this;
        return n;
    }

    STDMETHODIMP GetParameters(DWORD*, DWORD*) override {
        return E_NOTIMPL;
    }

    STDMETHODIMP Invoke(IMFAsyncResult* pResult) override {
        IMFMediaEvent* pEvent = nullptr;
        if (g_pSession && SUCCEEDED(g_pSession->EndGetEvent(pResult, &pEvent))) {
            pEvent->AddRef();
            PostMessage(m_hwnd, WM_APP_SESSION_EVENT, 0, (LPARAM)pEvent);

            g_pSession->BeginGetEvent(this, nullptr);
        }
        return S_OK;
    }
private:
    HWND m_hwnd;
    LONG m_cRef;
};

HRESULT AddBranchToTopology(IMFTopology* pTopology, IMFPresentationDescriptor* pPD, IMFStreamDescriptor* pSD, HWND videoHwnd) {
    IMFTopologyNode* pSrcNode = nullptr;
    IMFTopologyNode* pSinkNode = nullptr;
    IMFActivate* pSinkActivate = nullptr;
    IMFMediaTypeHandler* pHandler = nullptr;
    HRESULT hr = S_OK;

    hr = pSD->GetMediaTypeHandler(&pHandler);
    if (FAILED(hr)) goto done;

    GUID majorType;
    hr = pHandler->GetMajorType(&majorType);
    if (FAILED(hr)) goto done;

    if (majorType == MFMediaType_Audio)
        hr = MFCreateAudioRendererActivate(&pSinkActivate);
    else if (majorType == MFMediaType_Video)
        hr = MFCreateVideoRendererActivate(videoHwnd, &pSinkActivate);
    else
        goto done;

    if (FAILED(hr)) goto done;

    hr = MFCreateTopologyNode(MF_TOPOLOGY_SOURCESTREAM_NODE, &pSrcNode);
    if (FAILED(hr)) goto done;

    pSrcNode->SetUnknown(MF_TOPONODE_SOURCE, g_pSource);
    pSrcNode->SetUnknown(MF_TOPONODE_PRESENTATION_DESCRIPTOR, pPD);
    pSrcNode->SetUnknown(MF_TOPONODE_STREAM_DESCRIPTOR, pSD);

    hr = MFCreateTopologyNode(MF_TOPOLOGY_OUTPUT_NODE, &pSinkNode);
    if (FAILED(hr)) goto done;

    pSinkNode->SetObject(pSinkActivate);

    pTopology->AddNode(pSrcNode);
    pTopology->AddNode(pSinkNode);
    pSrcNode->ConnectOutput(0, pSinkNode, 0);

done:
    SafeRelease(&pHandler);
    SafeRelease(&pSrcNode);
    SafeRelease(&pSinkNode);
    SafeRelease(&pSinkActivate);
    return hr;
}

HRESULT CreateTopology(IMFMediaSource* pSource, IMFTopology** ppTopology, HWND videoHwnd) {
    IMFTopology* pTopology = nullptr;
    IMFPresentationDescriptor* pPD = nullptr;

    HRESULT hr = MFCreateTopology(&pTopology);
    if (FAILED(hr)) goto done;

    hr = pSource->CreatePresentationDescriptor(&pPD);
    if (FAILED(hr)) goto done; 
    {
        DWORD streamCount = 0;
        pPD->GetStreamDescriptorCount(&streamCount);
        for (DWORD i = 0; i < streamCount; i++) {
            BOOL selected = FALSE;
            IMFStreamDescriptor* pSD = nullptr;
            if (SUCCEEDED(pPD->GetStreamDescriptorByIndex(i, &selected, &pSD)) && selected)
            {
                AddBranchToTopology(pTopology, pPD, pSD, videoHwnd);
                pSD->Release();
            }
        }
    }
    *ppTopology = pTopology;
    (*ppTopology)->AddRef();

done:
    SafeRelease(&pTopology);
    SafeRelease(&pPD);
    return hr;
}

static void CloseSession() {
    KillTimer(g_hwndMain, 1);

    SafeRelease(&g_pVideoControl);
    SafeRelease(&g_pVolume);
    g_duration = 0;

    if (g_pSession) {
        g_pSession->Close();
        Sleep(100);
        g_pSession->Shutdown();
        SafeRelease(&g_pSession);
    }

    if (g_pSource) {
        g_pSource->Shutdown();
        SafeRelease(&g_pSource);
    }

    SafeRelease(&g_pCallback);

    g_state = PlayerState::CLOSED;
    SetWindowText(g_hwndTime, L"0:00 / 0:00");
    SendMessage(g_hwndSeek, TBM_SETPOS, TRUE, 0);
    SetWindowText(g_hwndMain, L"Media Player");
    UpdateButtons();
}

static HRESULT OpenURL(const std::wstring& url) {
    CloseSession();

    HRESULT hr = S_OK;
    IMFTopology* pTopology = nullptr;

    hr = MFCreateMediaSession(nullptr, &g_pSession);
    if (FAILED(hr)) goto done;
    g_pCallback = new MediaSessionCallback(g_hwndMain);
    hr = g_pSession->BeginGetEvent(g_pCallback, nullptr);
    if (FAILED(hr)) goto done;

    {
        IMFSourceResolver* pResolver = nullptr;
        IUnknown* pSourceUnk = nullptr;
        MF_OBJECT_TYPE objType = MF_OBJECT_INVALID;

        hr = MFCreateSourceResolver(&pResolver);
        if (FAILED(hr)) goto done;

        hr = pResolver->CreateObjectFromURL(url.c_str(), MF_RESOLUTION_MEDIASOURCE, nullptr, &objType, &pSourceUnk);
        pResolver->Release();
        if (FAILED(hr)) goto done;

        hr = pSourceUnk->QueryInterface(IID_PPV_ARGS(&g_pSource));
        pSourceUnk->Release();
        if (FAILED(hr)) goto done;
    }

    hr = CreateTopology(g_pSource, &pTopology, g_hwndVideo);
    if (FAILED(hr)) goto done;

    hr = g_pSession->SetTopology(0, pTopology);
    if (FAILED(hr)) goto done;

    g_state = PlayerState::OPEN_PENDING;

    {
        auto slash = url.find_last_of(L"\\/");
        std::wstring name = (slash != std::wstring::npos) ? url.substr(slash + 1) : url;
        SetWindowText(g_hwndMain, (L"Media Player - " + name).c_str());
    }
done:
    SafeRelease(&pTopology);
    if (FAILED(hr)) {
        CloseSession();
        MessageBox(g_hwndMain, L"Failed to open file.", L"Error", MB_OK | MB_ICONERROR);
    }
    return hr;
}

static std::wstring OpenMediaFile(HWND hwndOwner) {
    wchar_t path[MAX_PATH] = {};
    OPENFILENAME ofn = {};

    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwndOwner;
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"Medi Files\0*.mp4;*.mp3;*.wav;*.avi;*.mkv\0" L"All Files\0*.*\0";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;

    if (!g_lastFolder.empty())
        ofn.lpstrInitialDir = g_lastFolder.c_str();

    if (!GetOpenFileName(&ofn))
        return L"";

    std::wstring result(path);
    auto slash = result.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
        g_lastFolder = result.substr(0, slash);

    return result;
}

static void HandleSessionEvent(IMFMediaEvent* pEvent) {
    MediaEventType evType = MEUnknown;
    pEvent->GetType(&evType);

    switch (evType) {
    case MESessionTopologyStatus: {
        UINT32 status = 0;
        pEvent->GetUINT32(MF_EVENT_TOPOLOGY_STATUS, &status);

        if (status == MF_TOPOSTATUS_READY) {
            g_currentPos = 0;
            IMFGetService* pGetService = nullptr;
            if (SUCCEEDED(g_pSession->QueryInterface(IID_PPV_ARGS(&pGetService)))) {
                pGetService->GetService(MR_VIDEO_RENDER_SERVICE, IID_PPV_ARGS(&g_pVideoControl));
                pGetService->GetService(MR_POLICY_VOLUME_SERVICE, IID_PPV_ARGS(&g_pVolume));
                pGetService->Release();
            }

            IMFPresentationDescriptor* pPD = nullptr;
            if (SUCCEEDED(g_pSource->CreatePresentationDescriptor(&pPD))) {
                pPD->GetUINT64(MF_PD_DURATION, (UINT64*)&g_duration);
                pPD->Release();
            }

            if (g_pVolume) {
                int vol = (int)SendMessage(g_hwndVolume, TBM_GETPOS, 0, 0);
                g_pVolume->SetMasterVolume(vol / 100.0f);
            }

            PROPVARIANT varStart;
            PropVariantInit(&varStart);
            varStart.vt = VT_EMPTY;
            g_pSession->Start(&GUID_NULL, &varStart);
            PropVariantClear(&varStart);

            g_state = PlayerState::STARTED;
            SetTimer(g_hwndMain, 1, 500, nullptr);
            UpdateButtons();
        }
        break;
    }

    case MESessionEnded:
        g_state = PlayerState::STOPPED;
        break;

    case MEEndOfPresentation: {
        KillTimer(g_hwndMain, 1);

        PROPVARIANT var = {};
        var.vt = VT_I8;
        var.hVal.QuadPart = 0;
        g_pSession->Start(&GUID_NULL, &var);
        PropVariantClear(&var);
        g_pSession->Pause();

        g_state = PlayerState::PAUSED;
        SendMessage(g_hwndSeek, TBM_SETPOS, TRUE, 0);
        SetWindowText(g_hwndTime, (FormatTime(0) + L" / " + FormatTime(g_duration)).c_str());
        UpdateButtons();
        break;
    }

    case MEError: {
        HRESULT hrStatus = S_OK;
        pEvent->GetStatus(&hrStatus);
        wchar_t msg[128];
        swprintf_s(msg, L"Playback error: 0x%08X", hrStatus);
        MessageBox(g_hwndMain, msg, L"Error", MB_OK | MB_ICONERROR);
        CloseSession();
        break;
    }
    default:
        break;
    }
    pEvent->Release();
}

static void DrawThemedButton(LPDRAWITEMSTRUCT dis) {
    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;
    bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    bool hot = (dis->itemState & ODS_HOTLIGHT) != 0;

    COLORREF bgColor = disabled ? RGB(35, 35, 35) : pressed ? CLR_ACCENT : hot ? CLR_BUTTON_HOT : CLR_BUTTON;

    HBRUSH hbr = CreateSolidBrush(bgColor);
    FillRect(hdc, &rc, hbr);
    DeleteObject(hbr);

    HPEN hpen = CreatePen(PS_SOLID, 1, RGB(70, 70, 70));
    HPEN hold = (HPEN)SelectObject(hdc, hpen);
    SelectObject(hdc, GetStockObject(NULL_BRUSH));
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 6, 6);
    SelectObject(hdc, hold);
    DeleteObject(hpen);

    wchar_t text[64] = {};
    GetWindowText(dis->hwndItem, text, 64);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, disabled ? RGB(100, 100, 100) : CLR_BUTTON_TXT);

    HFONT hFont = CreateFont(14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT holdFont = (HFONT)SelectObject(hdc, hFont);

    DrawText(hdc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    SelectObject(hdc, holdFont);
    DeleteObject(hFont);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg)
    {
    case WM_CREATE: {
        CreateWindowW(L"BUTTON", L"OPEN", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            10, 10, 70, 30, hwnd, (HMENU)IDM_OPEN, nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"PLAY", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            90, 10, 70, 30, hwnd, (HMENU)IDM_PLAY, nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"PAUSE", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            170, 10, 70, 30, hwnd, (HMENU)IDM_PAUSE, nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"STOP", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            250, 10, 70, 30, hwnd, (HMENU)IDM_STOP, nullptr, nullptr);

        g_hwndSeek = CreateWindowW(TRACKBAR_CLASS, nullptr, WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
            10, 50, 650, 28, hwnd, (HMENU)105, nullptr, nullptr);
        SendMessage(g_hwndSeek, TBM_SETRANGE, TRUE, MAKELPARAM(0, 1000));
        SendMessage(g_hwndSeek, TBM_SETPOS, TRUE, 0);

        g_hwndTime = CreateWindowW(L"STATIC", L"0:00 / 0:00", WS_CHILD | WS_VISIBLE | SS_CENTER,
            655, 55, 120, 20, hwnd, nullptr, nullptr, nullptr);

        CreateWindowW(L"STATIC", L"Vol:", WS_CHILD | WS_VISIBLE | SS_LEFT,
            10, 85, 30, 20, hwnd, nullptr, nullptr, nullptr);

        g_hwndVolume = CreateWindowW(TRACKBAR_CLASS, nullptr, WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
            45, 82, 200, 28, hwnd, (HMENU)106, nullptr, nullptr);
        SendMessage(g_hwndVolume, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
        SendMessage(g_hwndVolume, TBM_SETPOS, TRUE, 50);

        g_hwndMain = hwnd;
        g_hwndVideo = CreateWindowExW(0, L"STATIC", nullptr, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
            0, TOOLBAR_H, 900, 560, hwnd, nullptr, nullptr, nullptr);

        HFONT hFont = CreateFont(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

        SendMessage(g_hwndTime, WM_SETFONT, (WPARAM)hFont, TRUE);

        DragAcceptFiles(hwnd, TRUE);

        UpdateButtons();

        return 0;
    }

    case WM_SIZE: {
        int w = LOWORD(lParam);
        int h = HIWORD(lParam);

        if (g_hwndSeek)
            MoveWindow(g_hwndSeek, 10, 50, w - 145, 28, TRUE);

        if (g_hwndTime)
            MoveWindow(g_hwndTime, w - 130, 55, 120, 20, TRUE);

        if (g_hwndVideo)
            MoveWindow(g_hwndVideo, 0, TOOLBAR_H, w, h - TOOLBAR_H, TRUE);

        if (g_pVideoControl) {
            RECT rc = { 0, 0, w, h - TOOLBAR_H };
            g_pVideoControl->SetVideoPosition(nullptr, &rc);
        }

        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT rcClient;
        GetClientRect(hwnd, &rcClient);
        //rc.top = TOOLBAR_H;
        //FillRect(hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));

        RECT rcToolbar = rcClient;
        //GetClientRect(hwnd, &rcToolbar);
        rcToolbar.bottom = TOOLBAR_H;
        HBRUSH hbrToolbar = CreateSolidBrush(CLR_TOOLBAR);
        FillRect(hdc, &rcToolbar, hbrToolbar);
        DeleteObject(hbrToolbar);

        RECT rcLine = { 0, TOOLBAR_H - 1, rcClient.right, TOOLBAR_H };
        HBRUSH hbrLine = CreateSolidBrush(CLR_ACCENT);
        FillRect(hdc, &rcLine, hbrLine);
        DeleteObject(hbrLine);

        RECT rcVideo = rcClient;
        rcToolbar.top = TOOLBAR_H;
        HBRUSH hbrVideo = CreateSolidBrush(CLR_BG);
        FillRect(hdc, &rcVideo, hbrVideo);
        DeleteObject(hbrVideo);

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_TIMER:
        if (wParam == 1)
            UpdateProgress();
        return 0;

    case WM_DRAWITEM: {
        DrawThemedButton((LPDRAWITEMSTRUCT)lParam);
        return TRUE;
    }

    case WM_NOTIFY: {
        NMHDR* pnmh = (NMHDR*)lParam;
        if (pnmh->code == NM_CUSTOMDRAW && (pnmh->hwndFrom == g_hwndSeek || pnmh->hwndFrom == g_hwndVolume)) {
            NMCUSTOMDRAW* pcd = (NMCUSTOMDRAW*)lParam;
            switch (pcd->dwDrawStage) {
            case CDDS_PREPAINT:
                return CDRF_NOTIFYITEMDRAW;

            case CDDS_ITEMPREPAINT:
                switch (pcd->dwItemSpec) {
                case TBCD_CHANNEL: {
                    HBRUSH hbr = CreateSolidBrush(RGB(60, 60, 60));
                    FillRect(pcd->hdc, &pcd->rc, hbr);
                    DeleteObject(hbr);

                    if (g_duration > 0 && pnmh->hwndFrom == g_hwndSeek) {
                        int pos = (int)SendMessage(g_hwndSeek, TBM_GETPOS, 0, 0);
                        int filled = (pcd->rc.right - pcd->rc.left) * pos / 1000;
                        RECT rcFilled = pcd->rc;
                        rcFilled.right = rcFilled.left + filled;
                        HBRUSH hbrFill = CreateSolidBrush(CLR_ACCENT);
                        FillRect(pcd->hdc, &rcFilled, hbrFill);
                        DeleteObject(hbrFill);
                    }
                    return CDRF_SKIPDEFAULT;
                }
                case TBCD_THUMB: {
                    HBRUSH hbr = CreateSolidBrush(CLR_ACCENT);
                    FillRect(pcd->hdc, &pcd->rc, hbr);
                    DeleteObject(hbr);
                    return CDRF_SKIPDEFAULT;
                }
                }
            }
        }
        return CDRF_DODEFAULT;
    }

    case WM_COMMAND: {
        switch (LOWORD(wParam)) {
        case IDM_OPEN: {
            std::wstring file = OpenMediaFile(hwnd);
            if (!file.empty())
                OpenURL(file);
            break;
        }
        case IDM_PLAY:
            if (g_pSession && g_state != PlayerState::CLOSED && g_state != PlayerState::OPEN_PENDING) {
                PROPVARIANT var;
                PropVariantInit(&var);
                var.vt = VT_EMPTY;
                g_pSession->Start(&GUID_NULL, &var);
                PropVariantClear(&var);
                g_state = PlayerState::STARTED;
                SetTimer(g_hwndMain, 1, 500, nullptr);
                UpdateButtons();
            }
            break;

        case IDM_PAUSE:
            if (g_pSession && g_state == PlayerState::STARTED) {
                g_pSession->Pause();
                g_state = PlayerState::PAUSED;
                KillTimer(g_hwndMain, 1);
                UpdateButtons();
            }
            break;

        case IDM_STOP:
            if (g_pSession && (g_state == PlayerState::STARTED || g_state == PlayerState::PAUSED)) {
               /* g_pSession->Stop();
                g_state = PlayerState::STOPPED;
                KillTimer(g_hwndMain, 1);
                SendMessage(g_hwndSeek, TBM_SETPOS, TRUE, 0);
                SetWindowText(g_hwndTime, (FormatTime(0) + L" / " + FormatTime(g_duration)).c_str());
                UpdateButtons();*/
                CloseSession();
            }
            break;
        }
        return 0;
    }

    case WM_HSCROLL:
    {
        if ((HWND)lParam == g_hwndSeek && g_pSession && g_duration > 0) {
            WORD code = LOWORD(wParam);

            if (code == TB_THUMBTRACK) {
                g_bSeeking = true;
            }

            if (code == TB_THUMBPOSITION || code == TB_ENDTRACK || code == TB_PAGEUP || code == TB_PAGEDOWN) {

                g_bSeeking = false;
                int pos = (int)SendMessage(g_hwndSeek, TBM_GETPOS, 0, 0);
                MFTIME seekTo = (MFTIME)((LONGLONG)pos * g_duration / 1000LL);

                g_currentPos = seekTo;

                PROPVARIANT var = {};
                var.vt = VT_I8;
                var.hVal.QuadPart = seekTo;
                g_pSession->Start(&GUID_NULL, &var);
                PropVariantClear(&var);

                if (g_state == PlayerState::PAUSED)
                    g_pSession->Pause();

                else if (g_state == PlayerState::STOPPED) {
                    g_state = PlayerState::STARTED;
                    SetTimer(g_hwndMain, 1, 500, nullptr);
                    UpdateButtons();
                }
            }
        }

        if ((HWND)lParam == g_hwndVolume && g_pVolume) {
            int vol = (int)SendMessage(g_hwndVolume, TBM_GETPOS, 0, 0);
            g_pVolume->SetMasterVolume(vol / 100.0f);
        }
        return 0;
    }

    case WM_KEYDOWN:
        switch (wParam) {
        case VK_SPACE:
            if (g_state == PlayerState::STARTED)
                SendMessage(hwnd, WM_COMMAND, IDM_PAUSE, 0);
            else if (g_state == PlayerState::PAUSED || g_state == PlayerState::STOPPED)
                SendMessage(hwnd, WM_COMMAND, IDM_PLAY, 0);
            break;

        case VK_ESCAPE:
            SendMessage(hwnd, WM_COMMAND, IDM_STOP, 0);
            break;

        case VK_UP:
            if (g_pVolume) {
                float vol = 0;
                g_pVolume->GetMasterVolume(&vol);
                vol += 0.05f;
                if (vol > 1.0f) vol = 1.0f;
                g_pVolume->SetMasterVolume(vol);
                SendMessage(g_hwndVolume, TBM_SETPOS, TRUE, (LPARAM)(vol * 100));
            }
            break;

        case VK_DOWN:
            if (g_pVolume) {
                float vol = 0;
                g_pVolume->GetMasterVolume(&vol);
                vol -= 0.05f;
                if (vol < 0.0f) vol = 0.0f;
                g_pVolume->SetMasterVolume(vol);
                SendMessage(g_hwndVolume, TBM_SETPOS, TRUE, (LPARAM)(vol * 100));
            }
            break;

        case VK_RIGHT:
        case VK_LEFT:
            if (g_pSession && g_duration > 0) {
                MFTIME pos = g_currentPos;
                pos += (wParam == VK_RIGHT ? 1 : -1) * 50000000LL;

                if (pos < 0) pos = 0;
                if (pos > g_duration) pos - g_duration;

                g_currentPos = pos;

                PROPVARIANT var = {};

                if (g_state == PlayerState::PAUSED)
                    g_pSession->Pause();

                int sliderVal = (int)(pos * 1000 / g_duration);
                SendMessage(g_hwndSeek, TBM_SETPOS, TRUE, sliderVal);
                SetWindowText(g_hwndTime, (FormatTime(pos) + L" / " + FormatTime(g_duration)).c_str());
                /*
                IMFClock* pClock = nullptr;
                if (SUCCEEDED(g_pSession->GetClock(&pClock))) {
                    IMFPresentationClock* pPC = nullptr;
                    if (SUCCEEDED(pClock->QueryInterface(IID_PPV_ARGS(&pPC)))) {
                        MFTIME pos = 0;
                        pPC->GetTime(&pos);

                        pos += (wParam == VK_RIGHT ? 1 : -1) * 50000000LL;

                        if (pos < 0)  pos = 0;
                        if (pos > g_duration) pos = g_duration;

                        PROPVARIANT var = {};
                        var.vt = VT_I8;
                        var.hVal.QuadPart = pos;
                        g_pSession->Start(&GUID_NULL, &var);
                        PropVariantClear(&var);

                        if (g_state == PlayerState::PAUSED)
                            g_pSession->Pause();

                        pPC->Release();
                    }
                    pClock->Release();
                }*/
            }
            break;
        }
        return 0;
    

    case WM_DROPFILES:
    {
        HDROP hDrop = (HDROP)wParam;
        wchar_t path[MAX_PATH] = {};
        if (DragQueryFile(hDrop, 0, path, MAX_PATH))
            OpenURL(path);
        DragFinish(hDrop);
        return 0;
    }

    case WM_APP_SESSION_EVENT:
    {
        HandleSessionEvent(reinterpret_cast<IMFMediaEvent*>(lParam));
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_CTLCOLORSTATIC: {
        HDC hdcStatic = (HDC)wParam;
        SetTextColor(hdcStatic, CLR_TIME_TXT);
        SetBkColor(hdcStatic, CLR_TOOLBAR);
        SetBkMode(hdcStatic, OPAQUE);
        return (LRESULT)CreateSolidBrush(CLR_TOOLBAR);
    }
      
    case WM_DESTROY:
        CloseSession();
        MFShutdown();
        CoUninitialize();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}


