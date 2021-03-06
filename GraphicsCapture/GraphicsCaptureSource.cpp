/********************************************************************************
 Copyright (C) 2012 Hugh Bailey <obs.jim@gmail.com>

 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; either version 2 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307, USA.
********************************************************************************/


#include "GraphicsCapture.h"


typedef HANDLE (WINAPI *OPPROC) (DWORD, BOOL, DWORD);

bool GraphicsCaptureSource::Init(XElement *data)
{
    this->data = data;
    capture = NULL;
    injectHelperProcess = NULL;

    Log(TEXT("Using graphics capture"));
    return true;
}

GraphicsCaptureSource::~GraphicsCaptureSource()
{
    if (injectHelperProcess)
        CloseHandle(injectHelperProcess);

    if (warningID)
    {
        API->RemoveStreamInfo(warningID);
        warningID = 0;
    }

    EndScene(); //should never actually need to be called, but doing it anyway just to be safe
}

static bool GetCaptureInfo(CaptureInfo &ci, DWORD processID)
{
    HANDLE hFileMap = OpenFileMapping(FILE_MAP_ALL_ACCESS, FALSE, String() << INFO_MEMORY << UINT(processID));
    if(hFileMap == NULL)
    {
        AppWarning(TEXT("GetCaptureInfo: Could not open file mapping"));
        return false;
    }

    CaptureInfo *infoIn;
    infoIn = (CaptureInfo*)MapViewOfFile(hFileMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(CaptureInfo));
    if(!infoIn)
    {
        AppWarning(TEXT("GetCaptureInfo: Could not map view of file"));
        CloseHandle(hFileMap);
        return false;
    }

    mcpy(&ci, infoIn, sizeof(CaptureInfo));

    if(infoIn)
        UnmapViewOfFile(infoIn);

    if(hFileMap)
        CloseHandle(hFileMap);

    return true;
}

void GraphicsCaptureSource::NewCapture()
{
    if(capture)
    {
        Log(TEXT("GraphicsCaptureSource::NewCapture:  eliminating old capture"));
        capture->Destroy();
        delete capture;
        capture = NULL;
    }

    if(!hSignalRestart)
    {
        hSignalRestart = GetEvent(String() << RESTART_CAPTURE_EVENT << UINT(targetProcessID));
        if(!hSignalRestart)
        {
            RUNONCE AppWarning(TEXT("GraphicsCaptureSource::NewCapture:  Could not create restart event"));
            return;
        }
    }

    hSignalEnd = GetEvent(String() << END_CAPTURE_EVENT << UINT(targetProcessID));
    if(!hSignalEnd)
    {
        RUNONCE AppWarning(TEXT("GraphicsCaptureSource::NewCapture:  Could not create end event"));
        return;
    }

    hSignalReady = GetEvent(String() << CAPTURE_READY_EVENT << UINT(targetProcessID));
    if(!hSignalReady)
    {
        RUNONCE AppWarning(TEXT("GraphicsCaptureSource::NewCapture:  Could not create ready event"));
        return;
    }

    hSignalExit = GetEvent(String() << APP_EXIT_EVENT << UINT(targetProcessID));
    if(!hSignalExit)
    {
        RUNONCE AppWarning(TEXT("GraphicsCaptureSource::NewCapture:  Could not create exit event"));
        return;
    }

    CaptureInfo info;
    if(!GetCaptureInfo(info, targetProcessID))
        return;

    bFlip = info.bFlip != 0;

    hwndCapture = (HWND)info.hwndCapture;

    if(info.captureType == CAPTURETYPE_MEMORY)
        capture = new MemoryCapture;
    else if(info.captureType == CAPTURETYPE_SHAREDTEX)
        capture = new SharedTexCapture;
    else
    {
        API->LeaveSceneMutex();

        RUNONCE AppWarning(TEXT("GraphicsCaptureSource::NewCapture: wtf, bad data from the target process"));
        return;
    }

    if(!capture->Init(info))
    {
        capture->Destroy();
        delete capture;
        capture = NULL;
    }
}

void GraphicsCaptureSource::EndCapture()
{
    if(capture)
    {
        capture->Destroy();
        delete capture;
        capture = NULL;
    }

    if(hOBSIsAlive)
        CloseHandle(hOBSIsAlive);
    if(hSignalRestart)
        CloseHandle(hSignalRestart);
    if(hSignalEnd)
        CloseHandle(hSignalEnd);
    if(hSignalReady)
        CloseHandle(hSignalReady);
    if(hSignalExit)
        CloseHandle(hSignalExit);

    hSignalRestart = hSignalEnd = hSignalReady = hSignalExit = hOBSIsAlive = NULL;

    bErrorAcquiring = false;

    bCapturing = false;
    captureCheckInterval = -1.0f;
    hwndCapture = NULL;
    targetProcessID = 0;
    foregroundPID = 0;
    foregroundCheckCount = 0;

    if(warningID)
    {
        API->RemoveStreamInfo(warningID);
        warningID = 0;
    }
}

void GraphicsCaptureSource::Preprocess()
{
}

void GraphicsCaptureSource::BeginScene()
{
    if(bCapturing)
        return;

    strWindowClass = data->GetString(TEXT("windowClass"));
    if(strWindowClass.IsEmpty())
        return;

    bStretch = data->GetInt(TEXT("stretchImage")) != 0;
    bIgnoreAspect = data->GetInt(TEXT("ignoreAspect")) != 0;
    bCaptureMouse = data->GetInt(TEXT("captureMouse"), 1) != 0;

    bool safeHookVal = data->GetInt(TEXT("safeHook")) != 0;

    if (safeHookVal != useSafeHook && safeHookVal)
        Log(L"Using anti-cheat hooking for game capture");

    useSafeHook = safeHookVal;

    bUseHotkey = data->GetInt(TEXT("useHotkey"), 0) != 0;
    hotkey = data->GetInt(TEXT("hotkey"), VK_F12);

    if (bUseHotkey)
        hotkeyID = OBSCreateHotkey(hotkey, (OBSHOTKEYPROC)GraphicsCaptureSource::CaptureHotkey, (UPARAM)this);

    gamma = data->GetInt(TEXT("gamma"), 100);

    if(bCaptureMouse && data->GetInt(TEXT("invertMouse")))
        invertShader = CreatePixelShaderFromFile(TEXT("shaders\\InvertTexture.pShader"));

    drawShader = CreatePixelShaderFromFile(TEXT("shaders\\DrawTexture_ColorAdjust.pShader"));

    AttemptCapture();
}

void GraphicsCaptureSource::AttemptCapture()
{
    //Log(TEXT("attempting to capture.."));

    if (!bUseHotkey)
        hwndTarget = FindWindow(strWindowClass, NULL);
    else
    {
        hwndTarget = hwndNextTarget;
        hwndNextTarget = NULL;
    }

    if (hwndTarget)
    {
        targetThreadID = GetWindowThreadProcessId(hwndTarget, &targetProcessID);
        if (!targetThreadID || !targetProcessID)
        {
            AppWarning(TEXT("GraphicsCaptureSource::BeginScene: GetWindowThreadProcessId failed, GetLastError = %u"), GetLastError());
            bErrorAcquiring = true;
            return;
        }
    }
    else
    {
        if (!bUseHotkey && !warningID)
            warningID = API->AddStreamInfo(Str("Sources.SoftwareCaptureSource.WindowNotFound"), StreamInfoPriority_High);

        bCapturing = false;

        return;
    }

    if (injectHelperProcess && WaitForSingleObject(injectHelperProcess, 0) == WAIT_TIMEOUT)
        return;

    if(warningID)
    {
        API->RemoveStreamInfo(warningID);
        warningID = 0;
    }

    //-------------------------------------------
    // see if we already hooked the process.  if not, inject DLL

    char pOPStr[12];
    mcpy(pOPStr, "NpflUvhel{x", 12); //OpenProcess obfuscated
    for (int i=0; i<11; i++) pOPStr[i] ^= i^1;

    OPPROC pOpenProcess = (OPPROC)GetProcAddress(GetModuleHandle(TEXT("KERNEL32")), pOPStr);

    HANDLE hProcess = (*pOpenProcess)(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ, FALSE, targetProcessID);
    if(hProcess)
    {
        DWORD dwSize = MAX_PATH;
        wchar_t processName[MAX_PATH];
        memset(processName, 0, sizeof(processName));

        QueryFullProcessImageName(hProcess, 0, processName, &dwSize);

        if (dwSize != 0 && scmpi(processName, lastProcessName) != 0)
        {
            if (processName[0])
            {
                wchar_t *fileName = srchr(processName, '\\');
                Log(L"Trying to hook process: %s", (fileName ? fileName+1 : processName));
            }
            scpy_n(lastProcessName, processName, MAX_PATH-1);
        }

        //-------------------------------------------
        // load keepalive event

        hOBSIsAlive = CreateEvent(NULL, FALSE, FALSE, String() << OBS_KEEPALIVE_EVENT << UINT(targetProcessID));

        //-------------------------------------------

        hwndCapture = hwndTarget;

        hSignalRestart = OpenEvent(EVENT_ALL_ACCESS, FALSE, String() << RESTART_CAPTURE_EVENT << UINT(targetProcessID));
        if(hSignalRestart)
        {
            SetEvent(hSignalRestart);
            bCapturing = true;
            captureWaitCount = 0;
        }
        else
        {
            BOOL b32bit = TRUE;

            if(Is64BitWindows())
                IsWow64Process(hProcess, &b32bit);

            String strDLLPath;
            DWORD dwDirSize = GetCurrentDirectory(0, NULL);
            strDLLPath.SetLength(dwDirSize);
            GetCurrentDirectory(dwDirSize, strDLLPath);

            strDLLPath << TEXT("\\plugins\\GraphicsCapture");

            String strHelper = strDLLPath;
            strHelper << ((b32bit) ? TEXT("\\injectHelper.exe") : TEXT("\\injectHelper64.exe"));

            String strCommandLine;
            strCommandLine << TEXT("\"") << strHelper << TEXT("\" ");
            if (useSafeHook)
                strCommandLine << UIntString(targetThreadID) << " 1";
            else
                strCommandLine << UIntString(targetProcessID) << " 0";

            //---------------------------------------

            PROCESS_INFORMATION pi;
            STARTUPINFO si;

            zero(&pi, sizeof(pi));
            zero(&si, sizeof(si));
            si.cb = sizeof(si);

            if(CreateProcess(strHelper, strCommandLine, NULL, NULL, FALSE, 0, NULL, strDLLPath, &si, &pi))
            {
                int exitCode = 0;

                CloseHandle(pi.hThread);

                if (!useSafeHook)
                {
                    WaitForSingleObject(pi.hProcess, INFINITE);
                    GetExitCodeProcess(pi.hProcess, (DWORD*)&exitCode);
                    CloseHandle(pi.hProcess);
                }
                else
                {
                    injectHelperProcess = pi.hProcess;
                }

                if(exitCode == 0)
                {
                    captureWaitCount = 0;
                    bCapturing = true;
                }
                else
                {
                    AppWarning(TEXT("GraphicsCaptureSource::BeginScene: Failed to inject library, error code = %d"), exitCode);
                    bErrorAcquiring = true;
                }
            }
            else
            {
                AppWarning(TEXT("GraphicsCaptureSource::BeginScene: Could not create inject helper, GetLastError = %u"), GetLastError());
                bErrorAcquiring = true;
            }
        }

        CloseHandle(hProcess);
        hProcess = NULL;

        if (!bCapturing)
        {
            CloseHandle(hOBSIsAlive);
            hOBSIsAlive = NULL;
        }
    }
    else
    {
        AppWarning(TEXT("GraphicsCaptureSource::BeginScene: OpenProcess failed, GetLastError = %u"), GetLastError());
        bErrorAcquiring = true;
    }
}

void GraphicsCaptureSource::EndScene()
{
    if(capture)
    {
        capture->Destroy();
        delete capture;
        capture = NULL;
    }

    delete invertShader;
    invertShader = NULL;

    delete drawShader;
    drawShader = NULL;

    delete cursorTexture;
    cursorTexture = NULL;

    if (hotkeyID)
    {
        OBSDeleteHotkey(hotkeyID);
        hotkeyID = 0;
    }

    if(!bCapturing)
        return;

    bCapturing = false;

    SetEvent(hSignalEnd);
    EndCapture();
}

void GraphicsCaptureSource::Tick(float fSeconds)
{
    if(hSignalExit && WaitForSingleObject(hSignalExit, 0) == WAIT_OBJECT_0) {
        Log(TEXT("Exit signal received, terminating capture"));
        EndCapture();
    }

    if(bCapturing && !hSignalReady && targetProcessID)
        hSignalReady = GetEvent(String() << CAPTURE_READY_EVENT << UINT(targetProcessID));

    if (injectHelperProcess && WaitForSingleObject(injectHelperProcess, 0) == WAIT_OBJECT_0)
    {
        DWORD exitCode;
        GetExitCodeProcess(injectHelperProcess, (DWORD*)&exitCode);
        CloseHandle(injectHelperProcess);
        injectHelperProcess = nullptr;

        if (exitCode != 0)
        {
            AppWarning(TEXT("safe inject Helper: Failed, error code = %d"), exitCode);
            bErrorAcquiring = true;
        }
    }

    if (hSignalReady) {

        DWORD val = WaitForSingleObject(hSignalReady, 0);
        if (val == WAIT_OBJECT_0)
            NewCapture();
        /*else if (val != WAIT_TIMEOUT)
            Log(TEXT("what the heck?  val is 0x%08lX"), val);*/
    }

    /*    static int floong = 0;

        if (floong++ == 30) {
            Log(TEXT("valid, bCapturing = %s"), bCapturing ? TEXT("true") : TEXT("false"));
            floong = 0;
        }
    } else {
        static int floong = 0;

        if (floong++ == 30) {
            Log(TEXT("not valid, bCapturing = %s"), bCapturing ? TEXT("true") : TEXT("false"));
            floong = 0;
        }
    }*/

    if(bCapturing && !capture)
    {
        if(++captureWaitCount >= API->GetMaxFPS())
        {
            bCapturing = false;
        }
    }

    if(!bCapturing && !bErrorAcquiring)
    {
        captureCheckInterval += fSeconds;
        if ((!bUseHotkey && captureCheckInterval >= 3.0f) ||
            (bUseHotkey && hwndNextTarget != NULL))
        {
            AttemptCapture();
            captureCheckInterval = 0.0f;
        }
    }
    else
    {
        if(!IsWindow(hwndCapture)) {
            Log(TEXT("Capture window 0x%08lX invalid or changing, terminating capture"), DWORD(hwndCapture));
            EndCapture();
        } else if (bUseHotkey && hwndNextTarget && hwndNextTarget != hwndTarget) {
            Log(TEXT("Capture hotkey triggered for new window, terminating capture"));
            EndCapture();
        } else {
            hwndNextTarget = NULL;
        }
    }
}

inline double round(double val)
{
    if(!_isnan(val) || !_finite(val))
        return val;

    if(val > 0.0f)
        return floor(val+0.5);
    else
        return floor(val-0.5);
}

void RoundVect2(Vect2 &v)
{
    v.x = float(round(v.x));
    v.y = float(round(v.y));
}

void GraphicsCaptureSource::Render(const Vect2 &pos, const Vect2 &size)
{
    if(capture)
    {
        Shader *lastShader = GetCurrentPixelShader();

        float fGamma = float(-(gamma-100) + 100) * 0.01f;

        LoadPixelShader(drawShader);
        HANDLE hGamma = drawShader->GetParameterByName(TEXT("gamma"));
        if(hGamma)
            drawShader->SetFloat(hGamma, fGamma);

        //----------------------------------------------------------
        // capture mouse

        bMouseCaptured = false;
        if(bCaptureMouse)
        {
            CURSORINFO ci;
            zero(&ci, sizeof(ci));
            ci.cbSize = sizeof(ci);

            if(GetCursorInfo(&ci) && hwndCapture)
            {
                mcpy(&cursorPos, &ci.ptScreenPos, sizeof(cursorPos));

                ScreenToClient(hwndCapture, &cursorPos);

                if(ci.flags & CURSOR_SHOWING)
                {
                    if(ci.hCursor == hCurrentCursor)
                        bMouseCaptured = true;
                    else
                    {
                        HICON hIcon = CopyIcon(ci.hCursor);
                        hCurrentCursor = ci.hCursor;

                        delete cursorTexture;
                        cursorTexture = NULL;

                        if(hIcon)
                        {
                            ICONINFO ii;
                            if(GetIconInfo(hIcon, &ii))
                            {
                                xHotspot = int(ii.xHotspot);
                                yHotspot = int(ii.yHotspot);

                                UINT width, height;
                                LPBYTE lpData = GetCursorData(hIcon, ii, width, height);
                                if(lpData && width && height)
                                {
                                    cursorTexture = CreateTexture(width, height, GS_BGRA, lpData, FALSE);
                                    if(cursorTexture)
                                        bMouseCaptured = true;

                                    Free(lpData);
                                }

                                DeleteObject(ii.hbmColor);
                                DeleteObject(ii.hbmMask);
                            }

                            DestroyIcon(hIcon);
                        }
                    }
                }
            }
        }

        //----------------------------------------------------------
        // game texture

        Texture *tex = capture->LockTexture();

        Vect2 texPos = Vect2(0.0f, 0.0f);
        Vect2 texStretch = Vect2(1.0f, 1.0f);

        if(tex)
        {
            Vect2 texSize = Vect2(float(tex->Width()), float(tex->Height()));
            Vect2 totalSize = API->GetBaseSize();

            Vect2 center = totalSize*0.5f;

            BlendFunction(GS_BLEND_ONE, GS_BLEND_ZERO);

            if(bStretch)
            {
                if(bIgnoreAspect)
                    texStretch *= totalSize;
                else
                {
                    float xAspect = totalSize.x / texSize.x;
                    float yAspect = totalSize.y / texSize.y;
                    float multiplyVal = ((texSize.y * xAspect) > totalSize.y) ? yAspect : xAspect;

                    texStretch *= texSize*multiplyVal;
                    texPos = center-(texStretch*0.5f);
                }
            }
            else
            {
                texStretch *= texSize;
                texPos = center-(texStretch*0.5f);
            }

            Vect2 sizeAdjust = size/totalSize;
            texPos *= sizeAdjust;
            texPos += pos;
            texStretch *= sizeAdjust;

            RoundVect2(texPos);
            RoundVect2(texSize);

            if(bFlip)
                DrawSprite(tex, 0xFFFFFFFF, texPos.x, texPos.y+texStretch.y, texPos.x+texStretch.x, texPos.y);
            else
                DrawSprite(tex, 0xFFFFFFFF, texPos.x, texPos.y, texPos.x+texStretch.x, texPos.y+texStretch.y);

            capture->UnlockTexture();

            BlendFunction(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);

            //----------------------------------------------------------
            // draw mouse

            if (!foregroundCheckCount)
            {
                //only check for foreground window every 10 frames since this involves two syscalls
                GetWindowThreadProcessId(GetForegroundWindow(), &foregroundPID);
                foregroundCheckCount = 10;
            }

            if(bMouseCaptured && cursorTexture && foregroundPID == targetProcessID)
            {
                Vect2 newCursorPos  = Vect2(float(cursorPos.x-xHotspot), float(cursorPos.y-xHotspot));
                Vect2 newCursorSize = Vect2(float(cursorTexture->Width()), float(cursorTexture->Height()));

                newCursorPos  /= texSize;
                newCursorSize /= texSize;

                newCursorPos *= texStretch;
                newCursorPos += texPos;

                newCursorSize *= texStretch;

                bool bInvertCursor = false;
                if(invertShader)
                {
                    if(bInvertCursor = ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 || (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0))
                        LoadPixelShader(invertShader);
                }

                DrawSprite(cursorTexture, 0xFFFFFFFF, newCursorPos.x, newCursorPos.y, newCursorPos.x+newCursorSize.x, newCursorPos.y+newCursorSize.y);
            }

            foregroundCheckCount--;
        }

        if(lastShader)
            LoadPixelShader(lastShader);
    }
}

Vect2 GraphicsCaptureSource::GetSize() const
{
    return API->GetBaseSize();
}

void GraphicsCaptureSource::UpdateSettings()
{
    EndScene();
    BeginScene();
}

void GraphicsCaptureSource::SetInt(CTSTR lpName, int iVal)
{
    if(scmpi(lpName, TEXT("gamma")) == 0)
    {
        gamma = iVal;
        if(gamma < 50)        gamma = 50;
        else if(gamma > 175)  gamma = 175;
    }
}

void STDCALL GraphicsCaptureSource::CaptureHotkey(DWORD hotkey, GraphicsCaptureSource *capture, bool bDown)
{
    if (bDown)
        capture->hwndNextTarget = GetForegroundWindow();
}
