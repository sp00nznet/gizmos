/*
 * Audio bridges.
 *
 * Two paths, and the game uses both:
 *
 *   waveOut*         digital speech and effects, straight out of the NE sound
 *                    DLLs it reads by hand.
 *   mciSendCommandA  everything else. There is no midiOutOpen anywhere in the
 *                    import table -- the MIDI in SOUNDS\*.MID and the CD audio
 *                    are both driven through MCI.
 *
 * Neither can be passed through untouched, because every structure involved
 * grew when Windows went 64-bit. WAVEHDR is 32 bytes in the game and 48 here;
 * the MCI parameter blocks all lead with a DWORD_PTR callback. So each one is
 * copied field by field into a host-side twin, and the twin is what the driver
 * sees.
 *
 * The awkward part is that the driver writes back. It sets WHDR_DONE on the
 * host's WAVEHDR, and the game polls its own copy.
 *
 * Re-syncing the two whenever the game pumped its message queue looked like
 * enough and was not: the narration in the opening is played by a loop that
 * spins on WHDR_DONE and never calls PeekMessage at all, so the intro stopped
 * dead on the first letter of "RED ALERT" and stayed there.
 *
 * So the driver tells us instead. Whatever the game asks for, the device is
 * opened with CALLBACK_FUNCTION pointing at wave_proc, which writes the flags
 * straight into the game's own WAVEHDR the moment a buffer finishes -- and then
 * posts the window message the game was expecting, if it asked for one.
 */

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>
/* Core Audio, for gg_set_own_volume() at the end of this file. */
#include <objbase.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#endif

#define RECOMP_GENERATED_CODE
#include "recomp_runtime.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Presence turns the log off; an explicit "0" turns it back on. */
static int audio_quiet(void) {
    static int q = -1;
    if (q < 0) {
        char buf[8];
        DWORD n = GetEnvironmentVariableA("GG_QUIET_AUDIO", buf, sizeof(buf));
        q = n ? !(buf[0] == '0' && buf[1] == 0) : 0;
    }
    return q;
}
#define ALOG if (!audio_quiet()) fprintf

/* ===================================================================
 * WAVEHDR: 32 bytes in the game, 48 here
 * =================================================================== */

/* Field offsets in the game's WAVEHDR. */
#define WH_LPDATA      0x00
#define WH_BUFLEN      0x04
#define WH_RECORDED    0x08
#define WH_USER        0x0C
#define WH_FLAGS       0x10
#define WH_LOOPS       0x14
#define WH_NEXT        0x18
#define WH_RESERVED    0x1C

#define MAX_WAVEHDRS 32

static struct {
    u32     va;        /* the game's WAVEHDR                    */
    WAVEHDR h;         /* the one the driver sees               */
    int     used;
} g_wh[MAX_WAVEHDRS];

static int wh_find(u32 va) {
    int i;
    for (i = 0; i < MAX_WAVEHDRS; i++)
        if (g_wh[i].used && g_wh[i].va == va) return i;
    return -1;
}

static int wh_claim(u32 va) {
    int i = wh_find(va);
    if (i >= 0) return i;
    for (i = 0; i < MAX_WAVEHDRS; i++) {
        if (!g_wh[i].used) {
            memset(&g_wh[i], 0, sizeof(g_wh[i]));
            g_wh[i].used = 1;
            g_wh[i].va = va;
            return i;
        }
    }
    fprintf(stderr, "    waveOut: header table full\n");
    return -1;
}

/* game -> host. lpData is a VA, and the image is mapped 1:1, so it is already
 * a valid host pointer. */
static void wh_push(int i) {
    u32 va = g_wh[i].va;
    g_wh[i].h.lpData          = (LPSTR)(uintptr_t)ADDR(MEM32(va + WH_LPDATA));
    g_wh[i].h.dwBufferLength  = MEM32(va + WH_BUFLEN);
    g_wh[i].h.dwBytesRecorded = MEM32(va + WH_RECORDED);
    g_wh[i].h.dwUser          = MEM32(va + WH_USER);
    g_wh[i].h.dwFlags         = MEM32(va + WH_FLAGS);
    g_wh[i].h.dwLoops         = MEM32(va + WH_LOOPS);
    g_wh[i].h.lpNext          = NULL;   /* only used for loops we do not chain */
    g_wh[i].h.reserved        = 0;
}

/* host -> game: only what the driver is allowed to change. */
static void wh_pull(int i) {
    u32 va = g_wh[i].va;
    MEM32(va + WH_FLAGS)    = (u32)g_wh[i].h.dwFlags;
    MEM32(va + WH_RECORDED) = (u32)g_wh[i].h.dwBytesRecorded;
}

/* MM_WOM_DONE hands the window the host's WAVEHDR; the game only knows its
 * own. */
u32 wave_lparam_to_game(uintptr_t lparam) {
    int i;
    for (i = 0; i < MAX_WAVEHDRS; i++)
        if (g_wh[i].used && (uintptr_t)&g_wh[i].h == lparam) return g_wh[i].va;
    return (u32)lparam;
}

/* What the game asked to be told on, if it asked for a window. */
static HWND g_wave_hwnd;

/*
 * Runs on a driver thread. Only a handful of calls are legal in here -- a few
 * stores and PostMessage are both on the list.
 */
static void CALLBACK wave_proc(HWAVEOUT hwo, UINT msg, DWORD_PTR inst,
                               DWORD_PTR p1, DWORD_PTR p2) {
    int i;
    (void)inst; (void)p2;
    if (msg != WOM_DONE) return;
    for (i = 0; i < MAX_WAVEHDRS; i++) {
        if (g_wh[i].used && (uintptr_t)&g_wh[i].h == (uintptr_t)p1) {
            wh_pull(i);          /* the flags the spin loop is waiting on */
            if (g_wave_hwnd)
                PostMessageA(g_wave_hwnd, 0x3BD /* MM_WOM_DONE */,
                             (WPARAM)(uintptr_t)hwo, (LPARAM)g_wh[i].va);
            return;
        }
    }
}

void bridge_waveOutOpen(void) {
    HWAVEOUT hwo = NULL;
    u32 phwo = ARG(1), fmt = ARG(3), cb = ARG(4), inst = ARG(5), flags = ARG(6);
    MMRESULT r;

    /*
     * CALLBACK_FUNCTION would be a VA with no machine code behind it. The game
     * asks for CALLBACK_WINDOW, which needs no thunk -- but say so rather than
     * hand mmsystem an address it will call.
     */
    if ((flags & CALLBACK_TYPEMASK) == CALLBACK_FUNCTION) {
        fprintf(stderr, "    waveOutOpen: CALLBACK_FUNCTION 0x%X dropped\n", cb);
        flags = (flags & ~CALLBACK_TYPEMASK) | CALLBACK_NULL;
        cb = 0;
    }

    r = waveOutOpen(&hwo, (UINT)ARG(2),
                    (LPCWAVEFORMATEX)(uintptr_t)ADDR(fmt),
                    (DWORD_PTR)cb, (DWORD_PTR)inst, flags);
    if (r == MMSYSERR_NOERROR && phwo) MEM32(phwo) = (u32)(uintptr_t)hwo;

    ALOG(stderr, "    waveOutOpen(fmt=%uHz %ubit %uch) -> %u\n",
         (unsigned)MEM32(fmt + 4), (unsigned)MEM16(fmt + 14),
         (unsigned)MEM16(fmt + 2), r);
    eax = r;
    esp += 4 + 24;
}

void bridge_waveOutPrepareHeader(void) {
    int i = wh_claim(ARG(2));
    if (i < 0) { eax = MMSYSERR_NOMEM; esp += 4 + 12; return; }
    wh_push(i);
    eax = waveOutPrepareHeader((HWAVEOUT)(uintptr_t)ARG(1), &g_wh[i].h, sizeof(WAVEHDR));
    wh_pull(i);
    esp += 4 + 12;
}

void bridge_waveOutWrite(void) {
    int i = wh_claim(ARG(2));
    if (i < 0) { eax = MMSYSERR_NOMEM; esp += 4 + 12; return; }
    /* Re-read: the game fills in lpData and the length between preparing the
     * header and writing it, and reuses one header for many sounds. */
    g_wh[i].h.lpData         = (LPSTR)(uintptr_t)ADDR(MEM32(g_wh[i].va + WH_LPDATA));
    g_wh[i].h.dwBufferLength = MEM32(g_wh[i].va + WH_BUFLEN);
    g_wh[i].h.dwLoops        = MEM32(g_wh[i].va + WH_LOOPS);
    eax = waveOutWrite((HWAVEOUT)(uintptr_t)ARG(1), &g_wh[i].h, sizeof(WAVEHDR));
    wh_pull(i);
    ALOG(stderr, "    waveOutWrite(%u bytes) -> %u\n", g_wh[i].h.dwBufferLength, eax);
    esp += 4 + 12;
}

void bridge_waveOutUnprepareHeader(void) {
    int i = wh_find(ARG(2));
    if (i < 0) { eax = MMSYSERR_NOERROR; esp += 4 + 12; return; }
    eax = waveOutUnprepareHeader((HWAVEOUT)(uintptr_t)ARG(1), &g_wh[i].h, sizeof(WAVEHDR));
    wh_pull(i);
    if (eax == MMSYSERR_NOERROR) g_wh[i].used = 0;
    esp += 4 + 12;
}

/* ===================================================================
 * MCI: the music and the CD
 *
 * Every parameter block starts with a DWORD_PTR callback, which is 4 bytes in
 * the game and 8 here, so nothing after it lines up. Each command gets its
 * fields copied across explicitly.
 * =================================================================== */

static const char *mci_name(u32 cmd) {
    switch (cmd) {
    case MCI_OPEN:   return "OPEN";
    case MCI_CLOSE:  return "CLOSE";
    case MCI_PLAY:   return "PLAY";
    case MCI_STOP:   return "STOP";
    case MCI_PAUSE:  return "PAUSE";
    case MCI_RESUME: return "RESUME";
    case MCI_SEEK:   return "SEEK";
    case MCI_STATUS: return "STATUS";
    case MCI_SET:    return "SET";
    case MCI_INFO:   return "INFO";
    case MCI_LOAD:   return "LOAD";
    case MCI_SAVE:   return "SAVE";
    default:         return "?";
    }
}

void bridge_mciSendCommandA(void) {
    u32 dev = ARG(1), cmd = ARG(2), fl = ARG(3), pva = ARG(4);
    MCIERROR r = MCIERR_UNSUPPORTED_FUNCTION;

    switch (cmd) {
    case MCI_OPEN: {
        /* game: dwCallback, wDeviceID, lpstrDeviceType, lpstrElementName, lpstrAlias */
        MCI_OPEN_PARMSA p;
        memset(&p, 0, sizeof(p));
        p.lpstrDeviceType  = MEM32(pva +  8) ? (LPCSTR)(uintptr_t)ADDR(MEM32(pva +  8)) : NULL;
        p.lpstrElementName = MEM32(pva + 12) ? (LPCSTR)(uintptr_t)ADDR(MEM32(pva + 12)) : NULL;
        p.lpstrAlias       = MEM32(pva + 16) ? (LPCSTR)(uintptr_t)ADDR(MEM32(pva + 16)) : NULL;
        r = mciSendCommandA(dev, cmd, fl, (DWORD_PTR)&p);
        MEM32(pva + 4) = p.wDeviceID;
        /* Only the fields the flags actually select hold anything. */
        ALOG(stderr, "    MCI OPEN type='%s' element='%s' -> %lu (id=%u)\n",
             (fl & MCI_OPEN_TYPE) && p.lpstrDeviceType ? p.lpstrDeviceType : "-",
             (fl & MCI_OPEN_ELEMENT) && p.lpstrElementName ? p.lpstrElementName : "-",
             r, p.wDeviceID);
        break;
    }
    case MCI_PLAY: {
        MCI_PLAY_PARMS p;
        memset(&p, 0, sizeof(p));
        p.dwFrom = MEM32(pva + 4);
        p.dwTo   = MEM32(pva + 8);
        r = mciSendCommandA(dev, cmd, fl, (DWORD_PTR)&p);
        ALOG(stderr, "    MCI PLAY dev=%u from=%d to=%d -> %lu\n", dev,
             (fl & MCI_FROM) ? (int)p.dwFrom : -1,
             (fl & MCI_TO)   ? (int)p.dwTo   : -1, r);
        break;
    }
    case MCI_SEEK: {
        MCI_SEEK_PARMS p;
        memset(&p, 0, sizeof(p));
        p.dwTo = MEM32(pva + 4);
        r = mciSendCommandA(dev, cmd, fl, (DWORD_PTR)&p);
        break;
    }
    case MCI_STATUS: {
        MCI_STATUS_PARMS p;
        memset(&p, 0, sizeof(p));
        p.dwItem  = MEM32(pva + 8);
        p.dwTrack = MEM32(pva + 12);
        r = mciSendCommandA(dev, cmd, fl, (DWORD_PTR)&p);
        MEM32(pva + 4) = (u32)p.dwReturn;
        break;
    }
    case MCI_SET: {
        MCI_SET_PARMS p;
        memset(&p, 0, sizeof(p));
        p.dwTimeFormat = MEM32(pva + 4);
        p.dwAudio      = MEM32(pva + 8);
        r = mciSendCommandA(dev, cmd, fl, (DWORD_PTR)&p);
        break;
    }
    case MCI_CLOSE:
    case MCI_STOP:
    case MCI_PAUSE:
    case MCI_RESUME: {
        MCI_GENERIC_PARMS p;
        memset(&p, 0, sizeof(p));
        r = mciSendCommandA(dev, cmd, fl, (DWORD_PTR)&p);
        ALOG(stderr, "    MCI %s dev=%u -> %lu\n", mci_name(cmd), dev, r);
        break;
    }
    default:
        fprintf(stderr, "    MCI %s (0x%X) not translated\n", mci_name(cmd), cmd);
        break;
    }

    eax = r;
    esp += 4 + 16;
}

/* ===================================================================
 * Volume, for when the game is being driven rather than played
 *
 * A scripted run is a few hundred frames of MIDI and narration at whatever the
 * machine's volume happens to be, which is nobody's idea of a good time in a
 * shared room. GG_VOLUME=0..100 sets this PROCESS's own audio session -- not the
 * system master, which is everyone else's -- so it covers waveOut and the MCI
 * sequencer at once rather than each of them separately.
 *
 * Doing it from outside, by pid, is what scripts/shot.ps1 used to try; it kept
 * missing, because the session does not exist until the game opens the device
 * and by then the first bar has already played. From in here it is set before
 * the game runs at all.
 * =================================================================== */

static const GUID GG_CLSID_MMDeviceEnumerator =
    {0xBCDE0395,0xE52F,0x467C,{0x8E,0x3D,0xC4,0x57,0x92,0x91,0x69,0x2E}};
static const GUID GG_IID_IMMDeviceEnumerator =
    {0xA95664D2,0x9614,0x4F35,{0xA7,0x46,0xDE,0x8D,0xB6,0x36,0x17,0xE6}};
static const GUID GG_IID_IAudioSessionManager =
    {0xBFA971F1,0x4D5E,0x40BB,{0x93,0x5E,0x96,0x70,0x39,0xBF,0xBE,0xE4}};

void gg_set_own_volume(void) {
    char buf[16];
    int pct;
    IMMDeviceEnumerator *en = NULL;
    IMMDevice *dev = NULL;
    IAudioSessionManager *mgr = NULL;
    ISimpleAudioVolume *vol = NULL;

    if (!GetEnvironmentVariableA("GG_VOLUME", buf, sizeof(buf))) return;
    pct = atoi(buf);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED))) return;
    if (SUCCEEDED(CoCreateInstance(&GG_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                   &GG_IID_IMMDeviceEnumerator, (void **)&en)) &&
        SUCCEEDED(en->lpVtbl->GetDefaultAudioEndpoint(en, eRender, eMultimedia, &dev)) &&
        SUCCEEDED(dev->lpVtbl->Activate(dev, &GG_IID_IAudioSessionManager, CLSCTX_ALL,
                                        NULL, (void **)&mgr)) &&
        /* NULL session guid + FALSE == this process's own default session. No
         * enumerating, no matching by pid: it is ours by construction. */
        SUCCEEDED(mgr->lpVtbl->GetSimpleAudioVolume(mgr, NULL, FALSE, &vol))) {
        vol->lpVtbl->SetMasterVolume(vol, (float)pct / 100.0f, NULL);
        vol->lpVtbl->SetMute(vol, pct == 0, NULL);
        fprintf(stderr, "  audio: this process at %d%%%s\n", pct, pct ? "" : " (muted)");
    } else {
        fprintf(stderr, "  audio: could not set this process's volume\n");
    }
    if (vol) vol->lpVtbl->Release(vol);
    if (mgr) mgr->lpVtbl->Release(mgr);
    if (dev) dev->lpVtbl->Release(dev);
    if (en)  en->lpVtbl->Release(en);
}
