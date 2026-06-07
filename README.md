# DeskTop Media Player
A fully functional desktop media player built from scratch using C++, the Win32 API, and
 Windows Media Foundation. Designed with a custom dark-themed UI nad support for multiple media formats.

 ---

## Features

- Multi-forma support - plays MP4, MP3, AVI, MKV, WAV and WMV files
- Full playback controls- Play, Pause, Stop
- Seek bar - scrub through media with real-time position tracking
- Volume control - adjustable slider with up/down arrow key support
- Keyboard shortcuts - space(play/pause), escape(stop), left/right arrows(seek 5s)
- Drag and drop - drag any media file directly on the window
- Time display - live current position and total duration
- File Dialog - browse and open files with a standard Open dialog
- Custom dark UI- owner drawn buttons, styled tract bar and GDI-rendered background
- Auto rewind - automatically rewinds to start when media finishes

---

## Built with
- C++ - core application language
- Win32 API - windows management, controls, message loop
- Windows Media Foundation - Audio and video decoding and playback
- GDI -Custom UI rendering and owner drawn controls
- Visual Studio 2022 - IDE and build system

---

## Project Structure

- MediaPlayer/
- -MediaPlayer.cpp #entire application code here
- -resource.h  #resource definitions for the Icon
- -resource.rc #win32 resource file
- -icon.ico  #the application icon
- MediaPlayer.sln #vs solution file

---

## Architecture Overview

- wWinMain - entry point, initializes COM, Media Foundation and the window
- WndProc - handles all the window message including input, resize and painting
- MediaSessionCallback - asynchronous MFA callback that posts session events back to the main thread safely
- OpenUrl - creates the MF source, builds the topology and starts the session
- CreateTopology / AddBranchToTopology - wires media source streams to audio and video renders
- HandleSessionEvent - processes playback events like topology ready, end of media and errors
- CloseSession - cleanly shuts down and release all COM objects

---

## Supported Formats

- .mp4 - video
- .mkv - video
- .avi - video
- .wmv - video
- .mp3 - audio
- .wav - audio

---

## Planned improvements
- Better UI
- Volume percentage track
- Accurate position on seek bar numbering
- Recent files menu
- Mini player mode
- Fullscreen mode toggle

## Installation and Setup

### Clone the repository
'''bash
git clone https://github.com/your-username/Media_Player_.git
cd MediaPlayer '''

- Open in Visual Studio
- Build the project
- Run the project


Built as a personal project to demonstrate systems programming, Win32 API integration Window Media
Foundation using C++

