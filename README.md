# DeskTop Media Player

A fully functional desktop media player built from scratch in C++ with the Win32 API and
 Windows Media Foundation. It has a custom dark-themed UI drawn with GDI+,and renders video through a Direct3D 11 flip-model swap chain so playback stays smooth and tear free.

 ---

## Features

- **Multi-forma support** - MP4, MP3, AVI, MKV, WAV and WMV files
- **Playback controls** - Play, Pause, Stop, Previous next
- **PlayList** - open several files at once and play then in sequence
- **Seek bar** - click or drag to scrub, with live position tracking
- **Volume and mute** - slider with a percentage readout
- **Playback speed** - 0.5x to 2x
- **Recent files menu** - remembers your last opened files and volume between runs
- **Fullscreen mode** - the control bar auto-hides and reappears on mouse movement
- **Drag and drop** - drop media files onto the window to play them
- **Keyboard shortcuts** - space(play/pause), escape(stop / leave full screen), left/right arrows(seek)
- **Time display** - live current position and total duration
- **File Dialog** - browse and open files with a standard Open dialog
- **Custom dark UI**- owner drawn control bar, icons and empty-screen artwork (GDI+)
- **Auto rewind** - automatically rewinds to start when media finishes

---

## Built with
- **C++** - core application language
- **Win32 API** - windows management, controls, message loop, input
- **Windows Media Foundation** - Audio and video decoding and playback
- **Direct3D 11 / DXGI**- hardware video frame transfer and flip-model presentation
- **GDI+** - Custom UI rendering and owner drawn controls
- **Visual Studio 2022** - IDE and build system

---

## Project Structure

```
- MediaPlayer/
- -MediaPlayer.cpp #entire application code here
- -resource.h  #resource definitions for the Icon
- -resource.rc #win32 resource file
- -icon.ico  #the application icon
- MediaPlayer.sln #vs solution file
```

---

## Architecture Overview

- **wWinMain** - starts COM, Media Foundation, GDI+ and Direct3D, creates the windows, and runs the message and frame loop
- **WndProc / VideoProc / ControlsProc** - main window, video surface and custom controls
- **InitGraphics / CreateVideoSwapChain** - create the D3D11 device, the DXGI device manager for the Media Engine, and a flip-model swap chain on Video window
- EngineCallback - receives Media Engine events on its own threads and posts them safely on the UI thread
- **EngineOpen / HandleEngineEvent** - load a file into the engine 
- **EngineRenderFrame** - each new video frame is transferred to the swap chain and presented in sync with the monitor refresh
- **UpdateProgress / SeekTo / Play / Pause / Stop** - playback control and position tracking
- **CloseSession** - cleanly shuts down the engine and resets states

---

## Supported Formats

| Extension | Type |
| ----- | ------ |
| .mp4 | video |
| .mkv | video |
| .avi | video |
| .wmv | video |
| .mp3 | audio |
| .wav | audio |

---

## Planned improvements
- Subtitle support
- Mini player mode


## Installation and Setup

### Clone the repository
```bash
git clone https://github.com/Diniso-G/Media_Player.git
cd Media_Player 
```

1. Open in Visual Studio
2. Build the project
3. Run the project

---


Built as a personal project to demonstrate systems programming, Win32 API integration Window Media
Foundation using C++

Diniso Gwabeni