# Akai S5000 / S6000 (IB-S56USB) — 64-bit Windows WinUSB Proxy Shim (Debug / Logging Build)

[![Build Status](https://img.shields.io/badge/build-passing-brightgreen.svg)](https://github.com/connectivezone/Aksys-shim-with-logging)
[![Platform](https://img.shields.io/badge/platform-Windows%2011%20%7C%2010%20%7C%207%20(64--bit)-blue.svg)](https://github.com/connectivezone/Aksys-shim-with-logging)
[![Target](https://img.shields.io/badge/target-Akai%20ak.Sys%20v1.53%20(x86)-orange.svg)](https://github.com/connectivezone/Aksys-shim-with-logging)
[![Driver](https://img.shields.io/badge/driver-Microsoft%20WinUSB-purple.svg)](https://github.com/connectivezone/Aksys-shim-with-logging)
[![License](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)

A lightweight 32-bit user-mode WinUSB proxy shim (`version.dll`) with **comprehensive diagnostic logging** enabled. This shim allows **Akai ak.Sys (v1.53)** to run on **64-bit Windows 11, Windows 10, and Windows 7** without legacy signed kernel drivers or disabling Windows Driver Signature Enforcement.

> 💡 **Looking for the high-speed Release build without file logging?**  
> If you do not need diagnostic trace logs and want maximum USB throughput with zero disk I/O overhead, download the release build from [Aksys-shim-no-logging](https://github.com/connectivezone/Aksys-shim-no-logging).

---

## 🔍 Purpose of This Build

This **Debug / Logging** build captures low-level communication between `ak.Sys` (`S56Device.dll`), modern Windows, and your Akai sampler hardware. All intercepted Win32 I/O calls, SetupAPI queries, USB bulk packets (with hex dumps), and completion timing events are written in real time to:

```text
C:\Users\Public\aksys_usb_log.txt
```

### Ideal For:
* **Troubleshooting Connection Issues**: Diagnose handshake drops, USB Error 15, USB Error 16 ("has s56 been unplugged?"), or unrecognized hardware.
* **Hardware Verification**: Verify whether the IB-S56USB board and host PC USB controller successfully complete endpoint enumeration and packet exchange.
* **Protocol Analysis & Development**: Inspect raw Akai SysEx inquiry/response headers, multi-chunk bulk packet boundaries, and Remote Panel LCD bitmap streams.

---

## ⚡ The Problem Solved

The original 1999–2002 Akai kernel driver (`akS56USB.sys`) is a legacy 32-bit driver written for Windows 98/2000. 64-bit editions of modern Windows strictly require digitally signed 64-bit kernel drivers and refuse to load 32-bit `.sys` files.

Instead of running unsigned third-party kernel drivers or forcing Windows into test-signing mode, this project pairs **Microsoft's built-in, WHQL-signed `WinUSB` (`winusb.sys`) driver** with a 32-bit user-mode proxy DLL named `version.dll`.

When `aksys.exe` loads `devices\S56Device.dll`, Windows loads our proxy shim first. The shim hooks the Import Address Table (IAT) of `S56Device.dll`, transparently converting obsolete device discovery and WDM asynchronous read/write requests into standard WinUSB API calls.

---

## 📦 Compatibility Matrix

| Hardware / Environment | Supported | Configuration Method |
| :--- | :---: | :--- |
| **Akai S5000 / S6000** | ✅ | Fitted with the **IB-S56USB** expansion card |
| **Akai ak.Sys** | ✅ | Version **1.53** (32-bit x86 application) |
| **Windows 11 (64-bit)** | ✅ | Native "WinUsb Device" selection in Device Manager |
| **Windows 10 (64-bit)** | ✅ | Native "WinUsb Device" selection in Device Manager |
| **Windows 7 (64-bit)** | ✅ | Assign WinUSB using [Zadig](https://zadig.akeo.ie/) |

---

## 🚀 Quick Start (Pre-Built Binary)

If you downloaded `version.dll` from the [Releases](https://github.com/connectivezone/Aksys-shim-with-logging/releases) tab:

### Step 1: Assign Microsoft WinUSB in Device Manager

1. Power on your Akai S5000/S6000 and connect the USB cable to your PC.
2. Press `Win + X` and select **Device Manager**.
3. Under **Other devices**, locate the entry for your sampler (usually shows as `AKAI S5000/S6000 USB` or `Unknown USB Device` with a yellow warning icon).
4. Right-click the device and choose **Update driver** → **Browse my computer for drivers** → **Let me pick from a list of available drivers on my computer**.
5. Scroll down to **Universal Serial Bus devices** (near the bottom) and click **Next**.
6. Select:
   * **Manufacturer**: `WinUsb Device`
   * **Model**: `WinUsb Device`
7. Click **Next**, click **Yes** to accept the prompt, and finish the installation.

*(Windows 7 64-bit users: Open [Zadig](https://zadig.akeo.ie/), choose Options → List All Devices, select the Akai sampler, set the target driver to **WinUSB**, and click "Install Driver".)*

### Step 2: Copy `version.dll`

1. Make sure `aksys.exe` is closed.
2. Copy `version.dll` into the `devices` subfolder of your ak.Sys installation directory (default path):
   ```text
   C:\Program Files (x86)\AKAIPRO\aksys\devices\version.dll
   ```
3. *(Recommended)* Also copy `version.dll` into the root directory alongside `aksys.exe`:
   ```text
   C:\Program Files (x86)\AKAIPRO\aksys\version.dll
   ```

#### Directory Layout Verification

```text
C:\Program Files (x86)\AKAIPRO\aksys\
├── aksys.exe
├── AKComCtrls.dll
├── version.dll                  <-- Recommended copy
└── devices\
    ├── S56Device.dll            <-- Original Akai module (do not modify!)
    └── version.dll              <-- REQUIRED: Drop logging shim here
```

### Step 3: Launch `ak.Sys` and Inspect the Log

1. Launch `aksys.exe`.
2. Open File Explorer and navigate to `C:\Users\Public\`.
3. Open `aksys_usb_log.txt` in Notepad or your preferred text editor to watch the real-time handshake, device enumeration, and data transfers.

---

## 📋 Understanding the Diagnostic Log

The log file is created at `C:\Users\Public\aksys_usb_log.txt` without requiring administrator privileges.

### Sample Normal Startup Sequence

```text
[18:18:16.922] ==============================================================
[18:18:16.922] Akai S5000/S6000 Win11 WinUSB Proxy Shim Initialized
[18:18:16.922] Target VID_09E8 PID_005E | Log: C:\Users\Public\aksys_usb_log.txt
[18:18:16.922] [*] Watcher thread active - monitoring process modules
[18:18:16.923] [*] Scanning system for Akai WinUSB Device (VID_09E8)...
[18:18:16.926] [+] Universal Scan Found Akai Node: 'WinUsb Device' (ID: usb\vid_09e8&pid_005e&rev_0100)
[18:18:16.927]   [+] Node Instance ID: USB\VID_09E8&PID_005E\6&3AF0F9CE&0&7
[18:18:16.928]       Endpoint[0] -> 0x82 (IN) MaxPacket=64 Type=2
[18:18:16.928]       Endpoint[1] -> 0x02 (OUT) MaxPacket=64 Type=2
[18:18:16.928] [+] Locked WinUSB Bulk Pipes: IN=0x82 (64B), OUT=0x02 (64B) [FIFOs Flushed]
[18:18:16.930] [+] Installed Safe SetupAPI Trampolines
[18:18:16.930]   [+] [IAT] S56Device.dll -> Hooked 10 functions!
[18:18:16.994] [+] Opened S56 Pipe 0 -> Handle 0x414B5302 [Type=PIPE0, Ep=0x82]
[18:18:16.995] [+] Opened S56 Pipe 1 -> Handle 0x414B5304 [Type=PIPE1, Ep=0x02]
[18:18:16.996] WriteFile[OUT] (14 bytes): 10 0B 00 F0 47 5E 20 00 00 00 01 00 01 F7
[18:18:17.001] ReadFile[RAW_IN] (10 bytes): F0 47 5E 20 00 00 44 00 01 F7
[18:18:17.263] ReadFile[RAW_IN] (21 bytes): F0 47 5E 20 00 00 52 02 03 41 4B 41 49 20 53 36 30 30 30 00 F7
```

* Notice the identification string returned by the sampler: ASCII `AKAI S6000` (or `AKAI S5000`).
* Notice the pipe handle isolation preventing endpoint collisions.
* Notice full frame assembly for multi-chunk transfers.

---

## 🔬 How It Works (Technical Overview)

```text
[aksys.exe / S56Device.dll]
          │
          ▼ (Loads local version.dll proxy)
   ┌─────────────────────────────────────────────────────────────┐
   │                  version.dll Shim (Logging)                 │
   │  - Export forwarding to C:\Windows\SysWOW64\version.dll     │
   │  - Real-time logging engine -> C:\Users\Public\aksys_usb... │
   │  - IAT Hooking: CreateFileA, ReadFile, WriteFile,           │
   │                 DeviceIoControl, CancelIo, SetupAPI         │
   │  - SetupAPI Enumeration Spoofing                            │
   │  - WDM Overlapped Completion Lifecycle Emulation           │
   │  - 64 KB Read Buffer + Multi-Packet LCD Frame Assembly      │
   └──────────────────────────┬──────────────────────────────────┘
                              │
                              ▼ (Translates to WinUSB calls)
                      [winusb.sys (Kernel)]
                              │
                              ▼ (Bulk IN: 0x82 / Bulk OUT: 0x02)
                   [Akai IB-S56USB Hardware]
```

1. **DLL Proxy & Redirection**: `ak.Sys` and `S56Device.dll` link Windows versioning APIs (`GetFileVersionInfoA`, `VerQueryValueA`, etc.). Dropping our proxy in the folder causes Windows to load the shim first. Non-intercepted exports are forwarded to `C:\Windows\SysWOW64\version.dll`.
2. **SetupAPI Emulation**: `S56Device.dll` searches for the legacy Akai interface GUID `{9D4B6529-D4A6-11D3-8EAE-0050DA2E2D53}`. The shim intercepts `SetupDiGetClassDevsA`, `SetupDiEnumDeviceInterfaces`, and `SetupDiGetDeviceInterfaceDetailA`, redirecting discovery to the active WinUSB device node.
3. **Overlapped Completion Emulation**: The original Akai communication module expects kernel-style asynchronous overlapped behavior (`IoMarkIrpPending` / immediate return of `FALSE` with `ERROR_IO_PENDING` on bulk reads). The shim properly manages caller event handles and asynchronous monitor routines to prevent handshake dropouts.
4. **Framebuffer Stream Assembly**: The Remote Panel LCD stream transmits 9,600 bytes across successive 64-byte bulk packets. The shim provisions a 64 KB transfer buffer and aggregates incoming chunks until packet completion, preventing endpoint desynchronization.

---

## 🛠️ Building from Source

The shim compiles into a 32-bit x86 DLL using Microsoft Visual C++ (`cl.exe`).

### Prerequisites
* Microsoft Visual Studio (2017, 2019, 2022, or 2026 Build Tools) with Desktop C++ components.
* Windows SDK (providing `winusb.lib`, `setupapi.lib`).

### Build Steps

1. Open an **x86 Native Tools Command Prompt for VS** (or Developer PowerShell).
2. Clone the repository:
   ```cmd
   git clone https://github.com/connectivezone/Aksys-shim-with-logging.git
   cd Aksys-shim-with-logging
   ```
3. Run the automated build script:
   ```cmd
   .\build_akai_win11_shim.bat
   ```
   Or invoke the MSVC compiler directly:
   ```cmd
   cl.exe /nologo /O2 /LD /EHsc akS56USB_WinUSB_Shim.cpp /link /OUT:version.dll /MACHINE:X86 setupapi.lib winusb.lib user32.lib advapi32.lib ole32.lib
   ```
4. Collect the generated `version.dll`.

---

## ❓ Troubleshooting

* **Log file is not created:**
  * Ensure `version.dll` is placed in `C:\Program Files (x86)\AKAIPRO\aksys\devices\`.
  * Verify `C:\Users\Public\` exists and is accessible.
* **"USB Error 16 on S56" on startup:**
  * Open Device Manager and verify the hardware is bound to `WinUsb Device` (Hardware ID `USB\VID_09E8&PID_005E`).
  * If the sampler experienced a timeout from an earlier aborted session, clear the popup on the sampler's physical screen using its soft keys or power-cycle the unit before relaunching `ak.Sys`.
* **Sampler not detected in device tree:**
  * Check `C:\Users\Public\aksys_usb_log.txt` to confirm that the SetupAPI queries returned `FOUND` and that `WriteFile` sent the 14-byte inquiry packet.

---

## 📄 License & Disclaimer

This project is open-source software distributed under the [MIT License](LICENSE).

*Akai, Akai Professional, S5000, S6000, and ak.Sys are trademarks or registered trademarks of their respective owners. This project is an independent community compatibility initiative and is not affiliated with, endorsed by, or associated with inMusic Brands or Akai Professional.*