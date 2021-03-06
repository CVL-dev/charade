/* The code in here is mostly lifted from Simon Tatham's putty code, in
 * particular windows/winpgntc.c.
 *
 * Copyright (c) 2009, Wesley Darlington. All Rights Reserved.
 *
 * Large swathes of the pageant interface taken from the putty source
 * code...
 *
 * Copyright 1997-2007 Simon Tatham.
 *
 * Portions copyright Robert de Bath, Joris van Rantwijk, Delian
 * Delchev, Andreas Schultz, Jeroen Massar, Wez Furlong, Nicolas Barry,
 * Justin Bradford, Ben Harris, Malcolm Smith, Ahmad Khalifa, Markus
 * Kuhn, and CORE SDI S.A.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE
 * FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
 * CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <stdio.h>
#include <windows.h>

#include "eprintf.h"
#include "pageant.h"

#ifndef NO_SECURITY
#include <aclapi.h>
#endif

// TODO: is there actually something magical about this number?
#define AGENT_COPYDATA_ID 0x804e50ba

#define AGENT_MAX_MSGLEN 8192

/*
 * Dynamically load advapi32.dll for SID manipulation. In its absence,
 * we degrade gracefully.
 */
#ifndef NO_SECURITY
int advapi_initialised = FALSE;
static HMODULE advapi;
DECL_WINDOWS_FUNCTION(static, BOOL, OpenProcessToken,
          (HANDLE, DWORD, PHANDLE));
DECL_WINDOWS_FUNCTION(static, BOOL, GetTokenInformation,
          (HANDLE, TOKEN_INFORMATION_CLASS,
                       LPVOID, DWORD, PDWORD));
DECL_WINDOWS_FUNCTION(static, BOOL, InitializeSecurityDescriptor,
          (PSECURITY_DESCRIPTOR, DWORD));
DECL_WINDOWS_FUNCTION(static, BOOL, SetSecurityDescriptorOwner,
          (PSECURITY_DESCRIPTOR, PSID, BOOL));
static int init_advapi(void)
{
    advapi = load_system32_dll("advapi32.dll");
    return advapi &&
        GET_WINDOWS_FUNCTION(advapi, OpenProcessToken) &&
  GET_WINDOWS_FUNCTION(advapi, GetTokenInformation) &&
  GET_WINDOWS_FUNCTION(advapi, InitializeSecurityDescriptor) &&
  GET_WINDOWS_FUNCTION(advapi, SetSecurityDescriptorOwner);
}
#endif

HMODULE load_system32_dll(const char *libname)
{
    /*
     * Wrapper function to load a DLL out of c:\windows\system32
     * without going through the full DLL search path. (Hence no
     * attack is possible by placing a substitute DLL earlier on that
     * path.)
     */
    static char path[MAX_PATH * 2] = {0};
    unsigned int len = GetSystemDirectory(path, MAX_PATH);
    if (len == 0 || len > MAX_PATH) return NULL;

    strcat(path, "\\");
    strcat(path, libname);
    HMODULE ret = LoadLibrary(path);
    return ret;
}

void
print_buf(int level, byte *buf, int numbytes)
{
    int i;
    for (i = 0; i < numbytes;) {
        EPRINTF_RAW(level, "%02x ", buf[i]);
        ++i;
        if (!(i%8)) EPRINTF_RAW(level, " ");
        if (!(i%16) || i == numbytes) EPRINTF_RAW(level, "\n");
    }
}

BOOL CALLBACK
wnd_enum_proc(HWND hwnd, LPARAM lparam)
{
    char window_name[512] = "\0";
    GetWindowText(hwnd, window_name, 512);
    EPRINTF(0, "Window %p: title '%s'.\n", hwnd, window_name);
    return TRUE;
}

void
enum_windows(void)
{
    BOOL retval = EnumWindows(wnd_enum_proc, (LPARAM) 0);

    if (!retval) {
        EPRINTF(0, "EnumWindows failed; GetLastError() => %d.\n",
                (int) GetLastError());
    }
}

// "in" means "to pageant", "out" means "from pageant". Sorry.
int
send_request_to_pageant(byte *inbuf, int inbytes, byte *outbuf, int outbuflen)
{
    EPRINTF(3, "Sending %d bytes to pageant.\n", inbytes);

    if (inbytes < 4) {
        EPRINTF(0, "Pageant-bound message too short (%d bytes).\n", inbytes);
        return 0;
    }
    int claimed_inbytes = GET_32BIT(inbuf);
    if (inbytes != claimed_inbytes + 4) {
        EPRINTF(0, "Pageant-bound message is %d bytes long, but it "
                "*says* it has %d=%d+4 bytes in it.\n",
                inbytes, claimed_inbytes+4, claimed_inbytes);
        return 0;
    }

    EPRINTF(5, "Message to pageant (%d bytes):\n", inbytes);
    print_buf(5, inbuf, inbytes);

    HWND hwnd;
    hwnd = FindWindow("Pageant", "Pageant");
    if (!hwnd) {
        EPRINTF(0, "Can't FindWindow(\"Pageant\"...) - "
                   "is pageant running?. (GetLastError is %x)\n",
                   (unsigned) GetLastError());
        return 0;
    }

    char mapname[512];
    sprintf(mapname, "PageantRequest%08x", (unsigned)GetCurrentThreadId());

    PSECURITY_ATTRIBUTES psa = NULL;
#ifndef NO_SECURITY
    SECURITY_ATTRIBUTES sa = {0};
    if (advapi_initialised || init_advapi()) {
        /*
         * Set the security info for the file mapping to the same as Pageant's
         * process, to make sure Pageant's SID check will pass.
         */

        DWORD dwProcId = 0;
        GetWindowThreadProcessId(hwnd, &dwProcId);
        HANDLE proc = OpenProcess(MAXIMUM_ALLOWED, FALSE, dwProcId);
        if (proc != NULL) {
			sa.nLength = sizeof(sa);
			sa.bInheritHandle = TRUE;
			sa.lpSecurityDescriptor = NULL;
			GetSecurityInfo(proc, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION,
					NULL, NULL, NULL, NULL, (PSECURITY_DESCRIPTOR*)&sa.lpSecurityDescriptor);
			if (sa.lpSecurityDescriptor) {
				psa = &sa;
			}
        }
		CloseHandle(proc);
    } else {
        EPRINTF(0, "Couldn't initialize advapi.\n");
    }
#endif /* NO_SECURITY */

    HANDLE filemap = CreateFileMapping(INVALID_HANDLE_VALUE, psa, 
                                       PAGE_READWRITE, 0, 
                                       AGENT_MAX_MSGLEN, mapname);
    if (filemap == NULL || filemap == INVALID_HANDLE_VALUE) {
        EPRINTF(0, "Can't CreateFileMapping.\n");
        return 0;
    }

    byte *shmem = MapViewOfFile(filemap, FILE_MAP_WRITE, 0, 0, 0);
    memcpy(shmem, inbuf, inbytes);
    COPYDATASTRUCT cds;
    cds.dwData = AGENT_COPYDATA_ID;
    cds.cbData = 1 + strlen(mapname);
    cds.lpData = mapname;

    int id = SendMessage(hwnd, WM_COPYDATA, (WPARAM) NULL, (LPARAM) &cds);
    int retlen = 0;
    if (id > 0) {
        retlen = 4 + GET_32BIT(shmem);
        if (retlen > outbuflen) {
            EPRINTF(0, "Buffer too small to contain reply from pageant.\n");
            return 0;
        }

        memcpy(outbuf, shmem, retlen);

        EPRINTF(5, "Reply from pageant (%d bytes):\n", retlen);
        print_buf(5, outbuf, retlen);
    } else {
        EPRINTF(0, "Couldn't SendMessage().\n");
        return 0;
    }

    // enum_windows();


    UnmapViewOfFile(shmem);
    CloseHandle(filemap);
#ifndef NO_SECURITY
    if (sa.lpSecurityDescriptor)
        LocalFree(sa.lpSecurityDescriptor);
#endif

    EPRINTF(3, "Got %d bytes back from pageant.\n", retlen);

    return retlen;
}
