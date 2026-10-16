// ============================================================================
// Akai S5000 / S6000 Windows 11 64-Bit WinUSB Shim (v10.1)
// File: akS56USB_WinUSB_Shim.cpp -> Compiles to devices\\version.dll (x86)
//
// Full 64KB Buffer & Multi-Packet Framebuffer Read for Remote Panel (9600B LCD)
// + Strict WDM Overlapped Read Semantics & Orphaned IRP Cleanup
// ============================================================================

#define _CRT_SECURE_NO_WARNINGS
#define _WIN32_WINNT 0x0501
#include <windows.h>
#include <winioctl.h>
#include <setupapi.h>
#include <winusb.h>
#include <usb.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <tlhelp32.h>
#include <intrin.h>
#include <stdio.h>
#include <stdarg.h>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "winusb.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((ULONG_PTR)0x00000000L)
#endif
#ifndef STATUS_UNSUCCESSFUL
#define STATUS_UNSUCCESSFUL ((ULONG_PTR)0xC0000001L)
#endif
#ifndef STATUS_PENDING
#define STATUS_PENDING ((ULONG_PTR)0x00000103L)
#endif

#ifndef USB_ENDPOINT_DIRECTION_IN
#define USB_ENDPOINT_DIRECTION_IN(addr) (((addr) & 0x80) == 0x80)
#endif
#ifndef USB_ENDPOINT_DIRECTION_OUT
#define USB_ENDPOINT_DIRECTION_OUT(addr) (((addr) & 0x80) == 0x00)
#endif

DEFINE_GUID(GUID_DEVINTERFACE_AKAI_S56, 0x9D4B6529L, 0xD4A6, 0x11D3, 0x8E, 0xAE, 0x00, 0x50, 0xDA, 0x2E, 0x2D, 0x53);
DEFINE_GUID(GUID_DEVINTERFACE_USB_DEVICE_DEF, 0xA5DCBF10L, 0x6530, 0x11D2, 0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED);
DEFINE_GUID(GUID_DEVINTERFACE_WINUSB_DEF, 0x88BAE032L, 0x5A81, 0x49F0, 0xBC, 0x3D, 0xA4, 0xFF, 0x13, 0x82, 0x16, 0xD6);

static CRITICAL_SECTION g_LogLock;
static bool g_LogInitialized = false;

static void LogMsg(const char* fmt, ...) {
    if (!g_LogInitialized) return;
    EnterCriticalSection(&g_LogLock);
    FILE* fp = fopen("C:\\Users\\Public\\aksys_usb_log.txt", "a");
    if (fp) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(fp, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        va_list args;
        va_start(args, fmt);
        vfprintf(fp, fmt, args);
        va_end(args);
        fprintf(fp, "\n");
        fflush(fp);
        fclose(fp);
    }
    LeaveCriticalSection(&g_LogLock);
}

static void LogHex(const char* prefix, const void* data, DWORD len) {
    if (!data || len == 0) return;
    const unsigned char* bytes = (const unsigned char*)data;
    char buf[512];
    int pos = snprintf(buf, sizeof(buf), "%s (%lu bytes): ", prefix, len);
    DWORD dumpLen = (len > 32) ? 32 : len;
    for (DWORD i = 0; i < dumpLen && pos < (int)sizeof(buf) - 8; i++) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%02X ", bytes[i]);
    }
    if (len > 32) snprintf(buf + pos, sizeof(buf) - pos, "...");
    LogMsg("%s", buf);
}

#define MAX_AKAI_HANDLES 64
#define AKAI_HANDLE_BASE 0x414B5300
#define AKAI_READ_BUFFER_SIZE 65536

enum AkaiHandleType {
    HANDLE_TYPE_UNUSED = 0,
    HANDLE_TYPE_ROOT,
    HANDLE_TYPE_PIPE0, // IN
    HANDLE_TYPE_PIPE1  // OUT
};

struct AkaiHandleContext {
    AkaiHandleType type;
    char path[256];
    BYTE readBuffer[AKAI_READ_BUFFER_SIZE];
    DWORD readLen;
    OVERLAPPED winusbOv;
    HANDLE hEvent;
    volatile bool readPending;
    LPVOID pUserDest;
    DWORD userReqLen;
    LPOVERLAPPED pUserOv;
};

static CRITICAL_SECTION g_UsbLock;
static HANDLE g_hUsbDevice = INVALID_HANDLE_VALUE;
static WINUSB_INTERFACE_HANDLE g_hWinUsb = NULL;
static char g_WinUsbDevicePath[MAX_PATH] = {0};
static UCHAR g_BulkInPipe = 0x82;
static UCHAR g_BulkOutPipe = 0x02;
static DWORD g_LastBytesTransferred = 0;
static volatile bool g_S56Hooked = false;
static AkaiHandleContext g_Handles[MAX_AKAI_HANDLES];
static LONG g_HandleCounter = 0;

static const char* FAKE_AKAI_PATH = "\\\\?\\usb#vid_09e8&pid_005e#aks56usb#{9d4b6529-d4a6-11d3-8eae-0050da2e2d53}";

static bool IsAkaiHandle(HANDLE h) {
    ULONG_PTR val = (ULONG_PTR)h;
    return (val >= AKAI_HANDLE_BASE && val < (AKAI_HANDLE_BASE + MAX_AKAI_HANDLES));
}

static int GetAkaiHandleIndex(HANDLE h) {
    if (!IsAkaiHandle(h)) return -1;
    return (int)((ULONG_PTR)h - AKAI_HANDLE_BASE);
}

static HANDLE AllocateAkaiHandle(AkaiHandleType type, const char* path) {
    EnterCriticalSection(&g_UsbLock);
    for (int step = 0; step < MAX_AKAI_HANDLES; step++) {
        int i = (InterlockedIncrement(&g_HandleCounter) & 0x7FFFFFFF) % MAX_AKAI_HANDLES;
        if (g_Handles[i].type == HANDLE_TYPE_UNUSED) {
            g_Handles[i].type = type;
            if (path) strncpy(g_Handles[i].path, path, sizeof(g_Handles[i].path) - 1);
            else g_Handles[i].path[0] = 0;
            g_Handles[i].readLen = 0;
            g_Handles[i].readPending = false;
            g_Handles[i].pUserDest = NULL;
            g_Handles[i].userReqLen = 0;
            g_Handles[i].pUserOv = NULL;
            if (!g_Handles[i].hEvent) {
                g_Handles[i].hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
                memset(&g_Handles[i].winusbOv, 0, sizeof(OVERLAPPED));
                g_Handles[i].winusbOv.hEvent = g_Handles[i].hEvent;
            } else {
                ResetEvent(g_Handles[i].hEvent);
            }
            HANDLE hResult = (HANDLE)(ULONG_PTR)(AKAI_HANDLE_BASE + i);
            LeaveCriticalSection(&g_UsbLock);
            return hResult;
        }
    }
    LeaveCriticalSection(&g_UsbLock);
    return INVALID_HANDLE_VALUE;
}

static void FreeAkaiHandle(HANDLE h) {
    int idx = GetAkaiHandleIndex(h);
    if (idx < 0) return;
    EnterCriticalSection(&g_UsbLock);
    if (g_Handles[idx].readPending && g_hWinUsb) {
        g_Handles[idx].readPending = false;
        WinUsb_AbortPipe(g_hWinUsb, g_BulkInPipe);
    }
    g_Handles[idx].type = HANDLE_TYPE_UNUSED;
    g_Handles[idx].readPending = false;
    g_Handles[idx].readLen = 0;
    g_Handles[idx].pUserDest = NULL;
    g_Handles[idx].pUserOv = NULL;
    LeaveCriticalSection(&g_UsbLock);
}

static bool ConfigureWinUsbInterface(HANDLE hDev, const char* devPath) {
    WINUSB_INTERFACE_HANDLE hWinUsb = NULL;
    if (!WinUsb_Initialize(hDev, &hWinUsb)) {
        LogMsg("[!] WinUsb_Initialize failed: err=%lu", GetLastError());
        return false;
    }

    USB_INTERFACE_DESCRIPTOR ifaceDesc;
    if (WinUsb_QueryInterfaceSettings(hWinUsb, 0, &ifaceDesc)) {
        for (UCHAR p = 0; p < ifaceDesc.bNumEndpoints; p++) {
            WINUSB_PIPE_INFORMATION pipeInfo;
            if (WinUsb_QueryPipe(hWinUsb, 0, p, &pipeInfo)) {
                bool isIn = USB_ENDPOINT_DIRECTION_IN(pipeInfo.PipeId);
                LogMsg("      Endpoint[%u] -> 0x%02X (%s) MaxPacket=%u Type=%u", 
                       p, pipeInfo.PipeId, isIn ? "IN" : "OUT", pipeInfo.MaximumPacketSize, pipeInfo.PipeType);

                ULONG timeout = 0;
                UCHAR autoClear = 1;
                UCHAR rawIo = 0;
                UCHAR shortPacket = 1;
                WinUsb_SetPipePolicy(hWinUsb, pipeInfo.PipeId, PIPE_TRANSFER_TIMEOUT, sizeof(ULONG), &timeout);
                WinUsb_SetPipePolicy(hWinUsb, pipeInfo.PipeId, AUTO_CLEAR_STALL, sizeof(UCHAR), &autoClear);
                WinUsb_SetPipePolicy(hWinUsb, pipeInfo.PipeId, RAW_IO, sizeof(UCHAR), &rawIo);
                WinUsb_SetPipePolicy(hWinUsb, pipeInfo.PipeId, SHORT_PACKET_TERMINATE, sizeof(UCHAR), &shortPacket);
            }
        }
    }

    g_BulkInPipe = 0x82;
    g_BulkOutPipe = 0x02;

    WinUsb_FlushPipe(hWinUsb, g_BulkInPipe);

    g_hWinUsb = hWinUsb;
    g_hUsbDevice = hDev;
    strncpy(g_WinUsbDevicePath, devPath, MAX_PATH - 1);
    LogMsg("[+] Locked WinUSB Bulk Pipes: IN=0x%02X (64B), OUT=0x%02X (64B) [FIFOs Flushed]", g_BulkInPipe, g_BulkOutPipe);
    return true;
}

static bool TryDirectPathOpen(const char* candidatePath) {
    LogMsg("    Attempting Direct Open: %s", candidatePath);
    HANDLE hDev = CreateFileA(candidatePath, GENERIC_WRITE | GENERIC_READ,
                              FILE_SHARE_WRITE | FILE_SHARE_READ, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
    if (hDev != INVALID_HANDLE_VALUE) {
        if (ConfigureWinUsbInterface(hDev, candidatePath)) {
            LogMsg("[+] DIRECT OPEN SUCCEEDED on: %s", candidatePath);
            return true;
        }
        CloseHandle(hDev);
    } else {
        LogMsg("    Direct Open failed: err=%lu", GetLastError());
    }
    return false;
}

static bool ScanAllUsbDevicesUniversal() {
    HDEVINFO hDevInfo = SetupDiGetClassDevsA(NULL, "USB", NULL, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (hDevInfo == INVALID_HANDLE_VALUE) return false;

    SP_DEVINFO_DATA devData;
    devData.cbSize = sizeof(SP_DEVINFO_DATA);

    for (DWORD i = 0; SetupDiEnumDeviceInfo(hDevInfo, i, &devData); i++) {
        char hwId[512] = {0};
        if (SetupDiGetDeviceRegistryPropertyA(hDevInfo, &devData, SPDRP_HARDWAREID, NULL, (PBYTE)hwId, sizeof(hwId) - 1, NULL)) {
            _strlwr(hwId);
            if (strstr(hwId, "vid_09e8") != NULL) {
                char devDesc[256] = {0};
                SetupDiGetDeviceRegistryPropertyA(hDevInfo, &devData, SPDRP_DEVICEDESC, NULL, (PBYTE)devDesc, sizeof(devDesc) - 1, NULL);
                LogMsg("[+] Universal Scan Found Akai Node: '%s' (ID: %s)", devDesc, hwId);

                char instanceId[MAX_PATH] = {0};
                if (SetupDiGetDeviceInstanceIdA(hDevInfo, &devData, instanceId, sizeof(instanceId) - 1, NULL)) {
                    LogMsg("  [+] Node Instance ID: %s", instanceId);

                    char symInstance[MAX_PATH] = {0};
                    strncpy(symInstance, instanceId, sizeof(symInstance) - 1);
                    for (int c = 0; symInstance[c]; c++) {
                        if (symInstance[c] == '\\') symInstance[c] = '#';
                    }

                    const char* guidStrings[] = {
                        "{a5dcbf10-6530-11d2-901f-00c04fb951ed}",
                        "{88bae032-5a81-49f0-bc3d-a4ff138216d6}",
                        "{9d4b6529-d4a6-11d3-8eae-0050da2e2d53}"
                    };

                    for (int g = 0; g < 3; g++) {
                        char candidatePath[MAX_PATH];
                        snprintf(candidatePath, sizeof(candidatePath), "\\\\?\\%s#%s", symInstance, guidStrings[g]);
                        if (TryDirectPathOpen(candidatePath)) {
                            SetupDiDestroyDeviceInfoList(hDevInfo);
                            return true;
                        }
                    }
                }
            }
        }
    }
    SetupDiDestroyDeviceInfoList(hDevInfo);
    return false;
}

static bool OpenAkaiWinUsbDevice() {
    EnterCriticalSection(&g_UsbLock);
    if (g_hWinUsb != NULL) {
        LeaveCriticalSection(&g_UsbLock);
        return true;
    }

    LogMsg("[*] Scanning system for Akai WinUSB Device (VID_09E8)...");
    if (ScanAllUsbDevicesUniversal()) {
        LeaveCriticalSection(&g_UsbLock);
        return true;
    }

    LeaveCriticalSection(&g_UsbLock);
    return false;
}

static BOOL CompleteIoOperation(LPOVERLAPPED lpOverlapped, DWORD bytesTransferred, BOOL ok) {
    g_LastBytesTransferred = bytesTransferred;
    if (lpOverlapped) {
        lpOverlapped->Internal = ok ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
        lpOverlapped->InternalHigh = bytesTransferred;
        if (lpOverlapped->hEvent) {
            SetEvent(lpOverlapped->hEvent);
        }
    }
    SetLastError(ok ? ERROR_SUCCESS : ERROR_GEN_FAILURE);
    return ok;
}

#define FAKE_HDEVINFO ((HDEVINFO)(ULONG_PTR)0x414B0001)

typedef HDEVINFO (WINAPI *PFN_SetupDiGetClassDevsA)(const GUID*, PCSTR, HWND, DWORD);
typedef BOOL (WINAPI *PFN_SetupDiEnumDeviceInterfaces)(HDEVINFO, PSP_DEVINFO_DATA, const GUID*, DWORD, PSP_DEVICE_INTERFACE_DATA);
typedef BOOL (WINAPI *PFN_SetupDiGetDeviceInterfaceDetailA)(HDEVINFO, PSP_DEVICE_INTERFACE_DATA, PSP_DEVICE_INTERFACE_DETAIL_DATA_A, DWORD, PDWORD, PSP_DEVINFO_DATA);

static PFN_SetupDiGetClassDevsA g_realSetupDiGetClassDevsA = NULL;
static PFN_SetupDiEnumDeviceInterfaces g_realSetupDiEnumDeviceInterfaces = NULL;
static PFN_SetupDiGetDeviceInterfaceDetailA g_realSetupDiGetDeviceInterfaceDetailA = NULL;

static bool InstallTrampolineHook(void* targetFunc, void* hookFunc, void** origTrampoline) {
    if (!targetFunc || !hookFunc) return false;
    BYTE* pTarget = (BYTE*)targetFunc;

    BYTE* pTrampoline = (BYTE*)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!pTrampoline) return false;

    memcpy(pTrampoline, pTarget, 5);
    pTrampoline[5] = 0xE9;
    *(DWORD*)(pTrampoline + 6) = (DWORD)((pTarget + 5) - (pTrampoline + 10));

    DWORD oldProt;
    if (!VirtualProtect(pTarget, 5, PAGE_EXECUTE_READWRITE, &oldProt)) {
        VirtualFree(pTrampoline, 0, MEM_RELEASE);
        return false;
    }

    pTarget[0] = 0xE9;
    *(DWORD*)(pTarget + 1) = (DWORD)((BYTE*)hookFunc - (pTarget + 5));
    VirtualProtect(pTarget, 5, oldProt, &oldProt);

    if (origTrampoline) *origTrampoline = pTrampoline;
    return true;
}

static HDEVINFO WINAPI Hook_SetupDiGetClassDevsA(const GUID* g, PCSTR e, HWND h, DWORD f) {
    if (g && (g->Data1 == 0x9D4B6529L || memcmp(g, &GUID_DEVINTERFACE_AKAI_S56, sizeof(GUID)) == 0)) {
        LogMsg("[+] Intercepted Akai SetupDiGetClassDevsA query");
        OpenAkaiWinUsbDevice();
        return FAKE_HDEVINFO;
    }
    if (g_realSetupDiGetClassDevsA) return g_realSetupDiGetClassDevsA(g, e, h, f);
    return SetupDiGetClassDevsA(g, e, h, f);
}

static BOOL WINAPI Hook_SetupDiEnumDeviceInterfaces(HDEVINFO s, PSP_DEVINFO_DATA d, const GUID* i, DWORD m, PSP_DEVICE_INTERFACE_DATA id) {
    if (s == FAKE_HDEVINFO) {
        if (m == 0) {
            if (id) {
                id->InterfaceClassGuid = GUID_DEVINTERFACE_AKAI_S56;
                id->Flags = SPINT_ACTIVE;
                id->Reserved = 0x414B;
            }
            LogMsg("[+] SetupDiEnumDeviceInterfaces(idx=0) -> FOUND");
            return TRUE;
        }
        SetLastError(ERROR_NO_MORE_ITEMS);
        return FALSE;
    }
    if (g_realSetupDiEnumDeviceInterfaces) return g_realSetupDiEnumDeviceInterfaces(s, d, i, m, id);
    return SetupDiEnumDeviceInterfaces(s, d, i, m, id);
}

static BOOL WINAPI Hook_SetupDiGetDeviceInterfaceDetailA(HDEVINFO s, PSP_DEVICE_INTERFACE_DATA id, 
                                                        PSP_DEVICE_INTERFACE_DETAIL_DATA_A det, 
                                                        DWORD sz, PDWORD req, PSP_DEVINFO_DATA d) {
    if (s == FAKE_HDEVINFO) {
        DWORD needed = (DWORD)(FIELD_OFFSET(SP_DEVICE_INTERFACE_DETAIL_DATA_A, DevicePath) + strlen(FAKE_AKAI_PATH) + 1);
        if (req) *req = needed;
        if (!det || sz < needed) {
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return FALSE;
        }
        strcpy(det->DevicePath, FAKE_AKAI_PATH);
        LogMsg("[+] SetupDiGetDeviceInterfaceDetailA -> Returned Spoofed Path: %s", det->DevicePath);
        return TRUE;
    }
    if (g_realSetupDiGetDeviceInterfaceDetailA) return g_realSetupDiGetDeviceInterfaceDetailA(s, id, det, sz, req, d);
    return SetupDiGetDeviceInterfaceDetailA(s, id, det, sz, req, d);
}

static BOOL WINAPI Hook_SetupDiDestroyDeviceInfoList(HDEVINFO s) {
    if (s == FAKE_HDEVINFO) return TRUE;
    return SetupDiDestroyDeviceInfoList(s);
}

static HANDLE WINAPI Hook_CreateFileA(LPCSTR name, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sec, 
                                     DWORD disp, DWORD flags, HANDLE templ) {
    if (name && (strstr(name, "aks56") || strstr(name, "vid_09e8"))) {
        OpenAkaiWinUsbDevice();
        if (strstr(name, "PIPE00") || strstr(name, "pipe00")) {
            HANDLE h = AllocateAkaiHandle(HANDLE_TYPE_PIPE0, name);
            LogMsg("[+] Opened S56 Pipe 0 ('%s') -> Handle 0x%p [Type=PIPE0, Ep=0x%02X]", name, h, g_BulkInPipe);
            return h;
        }
        if (strstr(name, "PIPE01") || strstr(name, "pipe01")) {
            HANDLE h = AllocateAkaiHandle(HANDLE_TYPE_PIPE1, name);
            LogMsg("[+] Opened S56 Pipe 1 ('%s') -> Handle 0x%p [Type=PIPE1, Ep=0x%02X]", name, h, g_BulkOutPipe);
            return h;
        }
        HANDLE h = AllocateAkaiHandle(HANDLE_TYPE_ROOT, name);
        LogMsg("[+] Opened S56 Root Handle ('%s') -> Handle 0x%p [Type=ROOT]", name, h);
        return h;
    }
    return CreateFileA(name, acc, share, sec, disp, flags, templ);
}

static void CompletePendingReadForContext(AkaiHandleContext* ctx, DWORD gotBytes, BOOL ok) {
    ctx->readPending = false;
    ResetEvent(ctx->hEvent);
    g_LastBytesTransferred = gotBytes;

    if (ok && gotBytes > 0) {
        LogHex("AsyncRead[RAW_IN]", ctx->readBuffer, gotBytes);
        if (ctx->pUserDest) {
            DWORD copyLen = (ctx->userReqLen < gotBytes) ? ctx->userReqLen : gotBytes;
            memcpy(ctx->pUserDest, ctx->readBuffer, copyLen);
            LogHex("  [+] Delivered Raw SysEx to S56Device", ctx->pUserDest, copyLen);
        }
    }

    LPOVERLAPPED uOv = ctx->pUserOv;
    ctx->pUserDest = NULL;
    ctx->pUserOv = NULL;

    if (uOv) {
        uOv->Internal = ok ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
        uOv->InternalHigh = ok ? gotBytes : 0;
        if (uOv->hEvent) {
            SetEvent(uOv->hEvent);
        }
    }
}

static DWORD WINAPI AsyncReadMonitorThread(LPVOID lpParam) {
    int idx = (int)(ULONG_PTR)lpParam;
    if (idx < 0 || idx >= MAX_AKAI_HANDLES) return 0;
    AkaiHandleContext* ctx = &g_Handles[idx];

    while (ctx->readPending && g_hWinUsb) {
        DWORD waitRes = WaitForSingleObject(ctx->hEvent, 100);
        if (!ctx->readPending) break;
        if (waitRes == WAIT_OBJECT_0) {
            ULONG got = 0;
            BOOL ok = WinUsb_GetOverlappedResult(g_hWinUsb, &ctx->winusbOv, &got, FALSE);
            CompletePendingReadForContext(ctx, got, ok);
            break;
        }
    }
    return 0;
}

static BOOL WINAPI Hook_WriteFile(HANDLE hFile, LPCVOID buf, DWORD len, LPDWORD written, LPOVERLAPPED ov) {
    if (!IsAkaiHandle(hFile)) return WriteFile(hFile, buf, len, written, ov);

    if (ov) {
        ov->Internal = STATUS_PENDING;
        ov->InternalHigh = 0;
        if (ov->hEvent) ResetEvent(ov->hEvent);
    }

    LogMsg("[*] WriteFile requested (h=0x%p, len=%lu, writtenPtr=0x%p, ov=0x%p)", hFile, len, written, ov);
    LogHex("WriteFile[OUT]", buf, len);

    ULONG transferred = 0;
    BOOL ok = FALSE;

    if (g_hWinUsb) {
        OVERLAPPED localOv;
        memset(&localOv, 0, sizeof(localOv));
        localOv.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

        ok = WinUsb_WritePipe(g_hWinUsb, g_BulkOutPipe, (PUCHAR)buf, len, &transferred, &localOv);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            ok = WinUsb_GetOverlappedResult(g_hWinUsb, &localOv, &transferred, TRUE);
        }
        CloseHandle(localOv.hEvent);
    }
    LogMsg("  -> WinUsb_WritePipe(pipe=0x%02X, req=%lu) = %s (sent=%lu, err=%lu)", 
           g_BulkOutPipe, len, ok ? "OK" : "FAIL", transferred, GetLastError());

    if (written) *written = len;
    return CompleteIoOperation(ov, len, ok);
}

static BOOL WINAPI Hook_ReadFile(HANDLE hFile, LPVOID buf, DWORD len, LPDWORD readBytes, LPOVERLAPPED ov) {
    if (!IsAkaiHandle(hFile)) return ReadFile(hFile, buf, len, readBytes, ov);

    int idx = GetAkaiHandleIndex(hFile);
    if (idx < 0 || !g_hWinUsb) {
        if (readBytes) *readBytes = 0;
        return CompleteIoOperation(ov, 0, FALSE);
    }

    if (ov) {
        ov->Internal = STATUS_PENDING;
        ov->InternalHigh = 0;
        if (ov->hEvent) ResetEvent(ov->hEvent);
    }

    AkaiHandleContext* ctx = &g_Handles[idx];
    LogMsg("[*] ReadFile requested (h=0x%p, reqLen=%lu, readBytesPtr=0x%p, ov=0x%p, readPending=%d)", 
           hFile, len, readBytes, ov, ctx->readPending ? 1 : 0);

    ULONG transferred = 0;
    BOOL ok = FALSE;

    if (ctx->readPending) {
        DWORD waitRes = WaitForSingleObject(ctx->hEvent, 20);
        if (waitRes == WAIT_OBJECT_0) {
            ok = WinUsb_GetOverlappedResult(g_hWinUsb, &ctx->winusbOv, &transferred, FALSE);
            ctx->readPending = false;
            ResetEvent(ctx->hEvent);
        } else {
            ctx->pUserDest = buf;
            ctx->userReqLen = len;
            ctx->pUserOv = ov;
            if (readBytes) *readBytes = 0;
            SetLastError(ERROR_IO_PENDING);
            return FALSE;
        }
    }

    if (!ctx->readPending && !ok) {
        ULONG readReq = (len > AKAI_READ_BUFFER_SIZE) ? AKAI_READ_BUFFER_SIZE : len;

        ResetEvent(ctx->hEvent);
        memset(&ctx->winusbOv, 0, sizeof(OVERLAPPED));
        ctx->winusbOv.hEvent = ctx->hEvent;

        ok = WinUsb_ReadPipe(g_hWinUsb, g_BulkInPipe, ctx->readBuffer, readReq, &transferred, &ctx->winusbOv);
        DWORD err = GetLastError();

        if (!ok && err == ERROR_IO_PENDING) {
            DWORD initialWait = (len >= 512) ? 500 : 25;
            DWORD waitRes = WaitForSingleObject(ctx->hEvent, initialWait);
            if (waitRes == WAIT_OBJECT_0) {
                ok = WinUsb_GetOverlappedResult(g_hWinUsb, &ctx->winusbOv, &transferred, FALSE);
                ctx->readPending = false;
                ResetEvent(ctx->hEvent);
            } else {
                ctx->pUserDest = buf;
                ctx->userReqLen = len;
                ctx->pUserOv = ov;
                ctx->readPending = true;
                LogMsg("  -> Read queued as pending on endpoint 0x%02X (reqLen=%lu)", g_BulkInPipe, len);
                HANDLE hTh = CreateThread(NULL, 0, AsyncReadMonitorThread, (LPVOID)(ULONG_PTR)idx, 0, NULL);
                if (hTh) CloseHandle(hTh);
                if (readBytes) *readBytes = 0;
                SetLastError(ERROR_IO_PENDING);
                return FALSE;
            }
        }

        while (ok && transferred < readReq && readReq >= 512 && (transferred % 64) == 0) {
            ULONG chunkGot = 0;
            ResetEvent(ctx->hEvent);
            memset(&ctx->winusbOv, 0, sizeof(OVERLAPPED));
            ctx->winusbOv.hEvent = ctx->hEvent;

            BOOL chunkOk = WinUsb_ReadPipe(g_hWinUsb, g_BulkInPipe,
                                           ctx->readBuffer + transferred,
                                           readReq - transferred,
                                           &chunkGot, &ctx->winusbOv);
            if (!chunkOk && GetLastError() == ERROR_IO_PENDING) {
                if (WaitForSingleObject(ctx->hEvent, 500) == WAIT_OBJECT_0) {
                    chunkOk = WinUsb_GetOverlappedResult(g_hWinUsb, &ctx->winusbOv, &chunkGot, FALSE);
                } else {
                    WinUsb_AbortPipe(g_hWinUsb, g_BulkInPipe);
                    break;
                }
            }
            if (!chunkOk || chunkGot == 0) break;
            transferred += chunkGot;
            if ((chunkGot % 64) != 0) break;
        }
    }

    if (ok && transferred > 0) {
        LogHex("ReadFile[RAW_IN]", ctx->readBuffer, transferred);

        DWORD copyLen = (len < transferred) ? len : transferred;
        memcpy(buf, ctx->readBuffer, copyLen);
        LogHex("  [+] Delivered Raw SysEx to S56Device", buf, copyLen);

        if (readBytes) *readBytes = copyLen;
        CompleteIoOperation(ov, copyLen, TRUE);

        if (ov) {
            LogMsg("  -> Returning FALSE (ERROR_IO_PENDING) with ov->hEvent signaled so S56Device calls GetOverlappedResult");
            SetLastError(ERROR_IO_PENDING);
            return FALSE;
        }
        return TRUE;
    }

    if (readBytes) *readBytes = 0;
    if (ov) {
        SetLastError(ERROR_IO_PENDING);
        return FALSE;
    }

    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

static BOOL WINAPI Hook_GetOverlappedResult(HANDLE hFile, LPOVERLAPPED ov, LPDWORD trans, BOOL wait) {
    if (IsAkaiHandle(hFile)) {
        int idx = GetAkaiHandleIndex(hFile);
        LogMsg("[*] GetOverlappedResult(h=0x%p, ov=0x%p, wait=%d)", hFile, ov, wait ? 1 : 0);

        if (idx >= 0 && g_Handles[idx].readPending && g_Handles[idx].hEvent) {
            DWORD waitTime = wait ? 4000 : 0;
            DWORD waitRes = WaitForSingleObject(g_Handles[idx].hEvent, waitTime);
            if (waitRes == WAIT_OBJECT_0) {
                ULONG got = 0;
                if (WinUsb_GetOverlappedResult(g_hWinUsb, &g_Handles[idx].winusbOv, &got, FALSE)) {
                    CompletePendingReadForContext(&g_Handles[idx], got, TRUE);
                    if (trans) *trans = got;
                    SetLastError(ERROR_SUCCESS);
                    LogMsg("  -> Overlapped read completed in GetOverlappedResult (%lu bytes)", got);
                    return TRUE;
                }
            } else {
                if (trans) *trans = 0;
                SetLastError(ERROR_IO_INCOMPLETE);
                return FALSE;
            }
        }

        if (ov && ov->Internal == STATUS_PENDING) {
            if (trans) *trans = 0;
            SetLastError(ERROR_IO_INCOMPLETE);
            return FALSE;
        }

        DWORD bytes = ov ? (DWORD)ov->InternalHigh : g_LastBytesTransferred;
        BOOL ok = (ov == NULL) || (ov->Internal == STATUS_SUCCESS);
        if (trans) *trans = bytes;
        SetLastError(ok ? ERROR_SUCCESS : ERROR_GEN_FAILURE);
        LogMsg("  -> GetOverlappedResult returning ok=%d, bytes=%lu", ok ? 1 : 0, bytes);
        return ok;
    }
    return GetOverlappedResult(hFile, ov, trans, wait);
}

static BOOL WINAPI Hook_DeviceIoControl(HANDLE hDev, DWORD code, LPVOID inBuf, DWORD inSz, 
                                       LPVOID outBuf, DWORD outSz, LPDWORD ret, LPOVERLAPPED ov) {
    if (!IsAkaiHandle(hDev)) return DeviceIoControl(hDev, code, inBuf, inSz, outBuf, outSz, ret, ov);

    int idx = GetAkaiHandleIndex(hDev);
    const char* typeStr = "UNKNOWN";
    if (idx >= 0) {
        if (g_Handles[idx].type == HANDLE_TYPE_PIPE0) typeStr = "PIPE0";
        else if (g_Handles[idx].type == HANDLE_TYPE_PIPE1) typeStr = "PIPE1";
        else if (g_Handles[idx].type == HANDLE_TYPE_ROOT) typeStr = "ROOT";
    }

    LogMsg("DeviceIoControl(h=0x%p [%s], IOCTL=0x%08lX, in=%lu, out=%lu)", hDev, typeStr, code, inSz, outSz);
    if (inBuf && inSz > 0) LogHex("  -> IOCTL inBuf", inBuf, inSz);

    if (code == 0x00220008 || code == 0x220000 || code == 0x220004) {
        if (idx >= 0 && g_Handles[idx].type == HANDLE_TYPE_PIPE0 && g_hWinUsb) {
            g_Handles[idx].readPending = false;
            WinUsb_AbortPipe(g_hWinUsb, g_BulkInPipe);
        }
        if (outBuf && outSz > 0) {
            memset(outBuf, 0, outSz);
        }
        if (ret) *ret = 0;
        LogMsg("  -> IOCTL 0x%08lX: Performed Pipe Reset / Abort (STATUS_SUCCESS, retBytes=0)", code);
        return CompleteIoOperation(ov, 0, TRUE);
    }

    if (ret) *ret = 0;
    return CompleteIoOperation(ov, 0, TRUE);
}

static BOOL WINAPI Hook_CancelIo(HANDLE hFile) {
    if (IsAkaiHandle(hFile)) {
        int idx = GetAkaiHandleIndex(hFile);
        if (idx >= 0 && g_Handles[idx].readPending && g_hWinUsb) {
            g_Handles[idx].readPending = false;
            WinUsb_AbortPipe(g_hWinUsb, g_BulkInPipe);
        }
        return TRUE;
    }
    return CancelIo(hFile);
}

static BOOL WINAPI Hook_CloseHandle(HANDLE hObject) {
    if (IsAkaiHandle(hObject)) {
        FreeAkaiHandle(hObject);
        return TRUE;
    }
    return CloseHandle(hObject);
}

static void* ResolveHookFunction(const char* fn) {
    if (strcmp(fn, "CreateFileA") == 0) return (void*)Hook_CreateFileA;
    if (strcmp(fn, "ReadFile") == 0) return (void*)Hook_ReadFile;
    if (strcmp(fn, "WriteFile") == 0) return (void*)Hook_WriteFile;
    if (strcmp(fn, "DeviceIoControl") == 0) return (void*)Hook_DeviceIoControl;
    if (strcmp(fn, "GetOverlappedResult") == 0) return (void*)Hook_GetOverlappedResult;
    if (strcmp(fn, "CancelIo") == 0) return (void*)Hook_CancelIo;
    if (strcmp(fn, "CloseHandle") == 0) return (void*)Hook_CloseHandle;
    if (strcmp(fn, "SetupDiGetClassDevsA") == 0) return (void*)Hook_SetupDiGetClassDevsA;
    if (strcmp(fn, "SetupDiEnumDeviceInterfaces") == 0) return (void*)Hook_SetupDiEnumDeviceInterfaces;
    if (strcmp(fn, "SetupDiGetDeviceInterfaceDetailA") == 0) return (void*)Hook_SetupDiGetDeviceInterfaceDetailA;
    if (strcmp(fn, "SetupDiDestroyDeviceInfoList") == 0) return (void*)Hook_SetupDiDestroyDeviceInfoList;
    return NULL;
}

static void PatchModuleIAT(HMODULE hMod, const char* modName) {
    if (!hMod) return;

    __try {
        PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)hMod;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
        PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)((BYTE*)hMod + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return;

        DWORD impRva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
        if (!impRva) return;
        PIMAGE_IMPORT_DESCRIPTOR imp = (PIMAGE_IMPORT_DESCRIPTOR)((BYTE*)hMod + impRva);

        int count = 0;
        for (; imp->Name; imp++) {
            PIMAGE_THUNK_DATA thunk = (PIMAGE_THUNK_DATA)((BYTE*)hMod + imp->FirstThunk);
            PIMAGE_THUNK_DATA origThunk = imp->OriginalFirstThunk ? 
                                          (PIMAGE_THUNK_DATA)((BYTE*)hMod + imp->OriginalFirstThunk) : thunk;

            for (; thunk->u1.AddressOfData; thunk++, origThunk++) {
                if (IMAGE_SNAP_BY_ORDINAL(origThunk->u1.Ordinal)) continue;
                PIMAGE_IMPORT_BY_NAME name = (PIMAGE_IMPORT_BY_NAME)((BYTE*)hMod + origThunk->u1.AddressOfData);
                void* rep = ResolveHookFunction((const char*)name->Name);
                if (rep && thunk->u1.Function != (ULONG_PTR)rep) {
                    DWORD old;
                    if (VirtualProtect(&thunk->u1.Function, sizeof(ULONG_PTR), PAGE_EXECUTE_READWRITE, &old)) {
                        thunk->u1.Function = (ULONG_PTR)rep;
                        VirtualProtect(&thunk->u1.Function, sizeof(ULONG_PTR), old, &old);
                        count++;
                    }
                }
            }
        }
        if (count > 0) {
            LogMsg("  [+] [IAT] %s (0x%p) -> Hooked %d functions!", modName, hMod, count);
            if (strstr(modName, "S56") || strstr(modName, "s56")) {
                g_S56Hooked = true;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void ScanAndPatchAllLoadedModules() {
    const char* knownPaths[] = {
        "S56Device.dll",
        "devices\\S56Device.dll",
        "C:\\Program Files (x86)\\AKAIPRO\\aksys\\devices\\S56Device.dll",
        "AKComCtrls.dll",
        "aksys.exe"
    };
    for (int i = 0; i < 5; i++) {
        HMODULE h = GetModuleHandleA(knownPaths[i]);
        if (h) {
            PatchModuleIAT(h, knownPaths[i]);
        }
    }

    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (hSnap != INVALID_HANDLE_VALUE) {
        MODULEENTRY32 me;
        me.dwSize = sizeof(MODULEENTRY32);
        if (Module32First(hSnap, &me)) {
            do {
                char lowerName[MAX_PATH];
                strncpy(lowerName, me.szModule, MAX_PATH - 1);
                lowerName[MAX_PATH - 1] = 0;
                _strlwr(lowerName);

                char lowerPath[MAX_PATH];
                strncpy(lowerPath, me.szExePath, MAX_PATH - 1);
                lowerPath[MAX_PATH - 1] = 0;
                _strlwr(lowerPath);

                if (strstr(lowerName, "s56") || strstr(lowerName, "aksys") || strstr(lowerName, "akai") ||
                    strstr(lowerPath, "s56device")) {
                    PatchModuleIAT(me.hModule, me.szExePath);
                }
            } while (Module32Next(hSnap, &me));
        }
        CloseHandle(hSnap);
    }
}

static void InstallGlobalSetupApiTrampolines() {
    HMODULE hSetup = GetModuleHandleA("setupapi.dll");
    if (!hSetup) hSetup = LoadLibraryA("setupapi.dll");
    if (!hSetup) return;

    void* pG = (void*)GetProcAddress(hSetup, "SetupDiGetClassDevsA");
    void* pE = (void*)GetProcAddress(hSetup, "SetupDiEnumDeviceInterfaces");
    void* pD = (void*)GetProcAddress(hSetup, "SetupDiGetDeviceInterfaceDetailA");

    if (pG && !g_realSetupDiGetClassDevsA) {
        InstallTrampolineHook(pG, (void*)Hook_SetupDiGetClassDevsA, (void**)&g_realSetupDiGetClassDevsA);
    }
    if (pE && !g_realSetupDiEnumDeviceInterfaces) {
        InstallTrampolineHook(pE, (void*)Hook_SetupDiEnumDeviceInterfaces, (void**)&g_realSetupDiEnumDeviceInterfaces);
    }
    if (pD && !g_realSetupDiGetDeviceInterfaceDetailA) {
        InstallTrampolineHook(pD, (void*)Hook_SetupDiGetDeviceInterfaceDetailA, (void**)&g_realSetupDiGetDeviceInterfaceDetailA);
    }
    LogMsg("[+] Installed Safe SetupAPI Trampolines (v10.1)");
}

static DWORD WINAPI WatcherThread(LPVOID) {
    LogMsg("[*] Watcher thread active (v10.1) - monitoring process modules");
    OpenAkaiWinUsbDevice();
    InstallGlobalSetupApiTrampolines();

    for (int i = 0; i < 50; i++) {
        ScanAndPatchAllLoadedModules();
        if (g_S56Hooked && i > 5) {
            LogMsg("[*] S56Device.dll successfully hooked - watcher thread entering idle mode");
            break;
        }
        Sleep(50);
    }
    return 0;
}

static HMODULE g_hRealVersion = NULL;

static void LoadRealVersionDll() {
    if (g_hRealVersion) return;
    char sysDir[MAX_PATH];
    GetSystemDirectoryA(sysDir, MAX_PATH);
    strcat(sysDir, "\\version.dll");
    g_hRealVersion = LoadLibraryA(sysDir);
}

static void CheckAndHookCaller(void* returnAddr) {
    HMODULE hCaller = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)returnAddr, &hCaller)) {
        if (hCaller && hCaller != GetModuleHandleA(NULL)) {
            char modPath[MAX_PATH] = {0};
            GetModuleFileNameA(hCaller, modPath, MAX_PATH - 1);
            PatchModuleIAT(hCaller, modPath);
        }
    }
}

extern "C" {
    BOOL WINAPI Proxy_GetFileVersionInfoA(LPCSTR f, DWORD h, DWORD l, LPVOID d) {
        CheckAndHookCaller(_ReturnAddress());
        LoadRealVersionDll();
        FARPROC p = g_hRealVersion ? GetProcAddress(g_hRealVersion, "GetFileVersionInfoA") : NULL;
        typedef BOOL (WINAPI *PFN)(LPCSTR, DWORD, DWORD, LPVOID);
        return p ? ((PFN)p)(f, h, l, d) : FALSE;
    }
    BOOL WINAPI Proxy_GetFileVersionInfoW(LPCWSTR f, DWORD h, DWORD l, LPVOID d) {
        CheckAndHookCaller(_ReturnAddress());
        LoadRealVersionDll();
        FARPROC p = g_hRealVersion ? GetProcAddress(g_hRealVersion, "GetFileVersionInfoW") : NULL;
        typedef BOOL (WINAPI *PFN)(LPCWSTR, DWORD, DWORD, LPVOID);
        return p ? ((PFN)p)(f, h, l, d) : FALSE;
    }
    DWORD WINAPI Proxy_GetFileVersionInfoSizeA(LPCSTR f, LPDWORD h) {
        CheckAndHookCaller(_ReturnAddress());
        LoadRealVersionDll();
        FARPROC p = g_hRealVersion ? GetProcAddress(g_hRealVersion, "GetFileVersionInfoSizeA") : NULL;
        typedef DWORD (WINAPI *PFN)(LPCSTR, LPDWORD);
        return p ? ((PFN)p)(f, h) : 0;
    }
    DWORD WINAPI Proxy_GetFileVersionInfoSizeW(LPCWSTR f, LPDWORD h) {
        CheckAndHookCaller(_ReturnAddress());
        LoadRealVersionDll();
        FARPROC p = g_hRealVersion ? GetProcAddress(g_hRealVersion, "GetFileVersionInfoSizeW") : NULL;
        typedef DWORD (WINAPI *PFN)(LPCWSTR, LPDWORD);
        return p ? ((PFN)p)(f, h) : 0;
    }
    BOOL WINAPI Proxy_VerQueryValueA(LPCVOID b, LPCSTR s, LPVOID *pBlock, PUINT l) {
        CheckAndHookCaller(_ReturnAddress());
        LoadRealVersionDll();
        FARPROC p = g_hRealVersion ? GetProcAddress(g_hRealVersion, "VerQueryValueA") : NULL;
        typedef BOOL (WINAPI *PFN)(LPCVOID, LPCSTR, LPVOID*, PUINT);
        return p ? ((PFN)p)(b, s, pBlock, l) : FALSE;
    }
    BOOL WINAPI Proxy_VerQueryValueW(LPCVOID b, LPCWSTR s, LPVOID *pBlock, PUINT l) {
        CheckAndHookCaller(_ReturnAddress());
        LoadRealVersionDll();
        FARPROC p = g_hRealVersion ? GetProcAddress(g_hRealVersion, "VerQueryValueW") : NULL;
        typedef BOOL (WINAPI *PFN)(LPCVOID, LPCWSTR, LPVOID*, PUINT);
        return p ? ((PFN)p)(b, s, pBlock, l) : FALSE;
    }
}

#pragma comment(linker, "/EXPORT:GetFileVersionInfoA=_Proxy_GetFileVersionInfoA@16")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoW=_Proxy_GetFileVersionInfoW@16")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoSizeA=_Proxy_GetFileVersionInfoSizeA@8")
#pragma comment(linker, "/EXPORT:GetFileVersionInfoSizeW=_Proxy_GetFileVersionInfoSizeW@8")
#pragma comment(linker, "/EXPORT:VerQueryValueA=_Proxy_VerQueryValueA@16")
#pragma comment(linker, "/EXPORT:VerQueryValueW=_Proxy_VerQueryValueW@16")

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        memset(g_Handles, 0, sizeof(g_Handles));
        g_HandleCounter = 0;
        InitializeCriticalSection(&g_LogLock);
        InitializeCriticalSection(&g_UsbLock);
        g_LogInitialized = true;

        LogMsg("==============================================================");
        LogMsg("Akai S5000/S6000 Win11 WinUSB Proxy Shim v10.1 Initialized");
        LogMsg("Target VID_09E8 PID_005E | Log: C:\\Users\\Public\\aksys_usb_log.txt");

        CreateThread(NULL, 0, WatcherThread, NULL, 0, NULL);
    } else if (reason == DLL_PROCESS_DETACH) {
        for (int i = 0; i < MAX_AKAI_HANDLES; i++) {
            if (g_Handles[i].hEvent) CloseHandle(g_Handles[i].hEvent);
        }
        if (g_hWinUsb) WinUsb_Free(g_hWinUsb);
        if (g_hUsbDevice != INVALID_HANDLE_VALUE) CloseHandle(g_hUsbDevice);
    }
    return TRUE;
}
