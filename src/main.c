/*
 * Legends Nudge Bridge
 * --------------------
 * Future Pinball's "Digital Nudge Left/Right/Up" fields only accept keyboard
 * keys -- you cannot bind a gamepad button to them. This tray app watches the
 * physical nudge buttons on an AtGames Legends cabinet (in OTG / controller
 * mode) and injects the matching keyboard scancodes, so FP sees exactly what
 * it wants to see.
 *
 * Buttons are read with the winmm joystick API and keys are injected as
 * hardware scancodes via SendInput, which is what DirectInput-era games like
 * Future Pinball read.
 *
 * Nothing is hardcoded: the Setup window learns the button numbers from the
 * cabinet you actually own, and stores them keyed by USB VID/PID.
 */

#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <shellapi.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#define APP_NAME     "Legends Nudge Bridge"
#define APP_VERSION  "1.0.0"
#define APP_MUTEX    "LegendsNudgeBridge_SingleInstance"
#define INI_NAME     "nudge.ini"
#define LOG_NAME     "nudge-log.txt"
#define RUN_KEY      "Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define RUN_VALUE    "LegendsNudgeBridge"

#define MAX_JOY      16
#define MAX_BIND     8
#define WM_TRAYICON  (WM_APP + 1)
#define WM_BOUND     (WM_APP + 2)   /* wParam = joy id, lParam = button (1-based) */
#define WM_RESCANNED (WM_APP + 3)

#define IDM_ENABLE   2001
#define IDM_SETUP    2002
#define IDM_DIAG     2003
#define IDM_OPENINI  2004
#define IDM_RELOAD   2005
#define IDM_AUTORUN  2006
#define IDM_ABOUT    2007
#define IDM_EXIT     2008

#define IDC_BIND_BASE   1000
#define IDC_CLEAR_BASE  1050
#define IDC_KEY_BASE    1100
#define IDC_STAT_BASE   1150
#define IDC_SAVE        1300
#define IDC_CANCEL      1301
#define IDC_HINT        1302
#define IDC_DIAGTEXT    1400

/* ------------------------------------------------------------------ */
/* Scancode table (US key positions -- these are the DIK_* codes that  */
/* DirectInput games read, so they are positional, not layout aware).  */
/* ------------------------------------------------------------------ */

typedef struct { const char *name; WORD scan; int ext; } KeyEntry;

static const KeyEntry KEYTAB[] = {
    {"ESC",0x01,0},{"1",0x02,0},{"2",0x03,0},{"3",0x04,0},{"4",0x05,0},
    {"5",0x06,0},{"6",0x07,0},{"7",0x08,0},{"8",0x09,0},{"9",0x0A,0},
    {"0",0x0B,0},{"MINUS",0x0C,0},{"EQUALS",0x0D,0},{"BACKSPACE",0x0E,0},
    {"TAB",0x0F,0},
    {"Q",0x10,0},{"W",0x11,0},{"E",0x12,0},{"R",0x13,0},{"T",0x14,0},
    {"Y",0x15,0},{"U",0x16,0},{"I",0x17,0},{"O",0x18,0},{"P",0x19,0},
    {"LBRACKET",0x1A,0},{"RBRACKET",0x1B,0},{"ENTER",0x1C,0},{"LCTRL",0x1D,0},
    {"A",0x1E,0},{"S",0x1F,0},{"D",0x20,0},{"F",0x21,0},{"G",0x22,0},
    {"H",0x23,0},{"J",0x24,0},{"K",0x25,0},{"L",0x26,0},
    {"SEMICOLON",0x27,0},{"APOSTROPHE",0x28,0},{"GRAVE",0x29,0},
    {"LSHIFT",0x2A,0},{"BACKSLASH",0x2B,0},
    {"Z",0x2C,0},{"X",0x2D,0},{"C",0x2E,0},{"V",0x2F,0},{"B",0x30,0},
    {"N",0x31,0},{"M",0x32,0},{"COMMA",0x33,0},{"PERIOD",0x34,0},
    {"SLASH",0x35,0},{"RSHIFT",0x36,0},{"LALT",0x38,0},{"SPACE",0x39,0},
    {"CAPSLOCK",0x3A,0},
    {"F1",0x3B,0},{"F2",0x3C,0},{"F3",0x3D,0},{"F4",0x3E,0},{"F5",0x3F,0},
    {"F6",0x40,0},{"F7",0x41,0},{"F8",0x42,0},{"F9",0x43,0},{"F10",0x44,0},
    {"F11",0x57,0},{"F12",0x58,0},
    {"NUMPAD0",0x52,0},{"NUMPAD1",0x4F,0},{"NUMPAD2",0x50,0},{"NUMPAD3",0x51,0},
    {"NUMPAD4",0x4B,0},{"NUMPAD5",0x4C,0},{"NUMPAD6",0x4D,0},{"NUMPAD7",0x47,0},
    {"NUMPAD8",0x48,0},{"NUMPAD9",0x49,0},{"NUMPADSTAR",0x37,0},
    {"NUMPADMINUS",0x4A,0},{"NUMPADPLUS",0x4E,0},{"NUMPADPERIOD",0x53,0},
    {"NUMPADSLASH",0x35,1},{"NUMPADENTER",0x1C,1},
    {"RCTRL",0x1D,1},{"RALT",0x38,1},
    {"UP",0x48,1},{"DOWN",0x50,1},{"LEFT",0x4B,1},{"RIGHT",0x4D,1},
    {"HOME",0x47,1},{"END",0x4F,1},{"PGUP",0x49,1},{"PGDN",0x51,1},
    {"INSERT",0x52,1},{"DELETE",0x53,1},
    {NULL,0,0}
};

/* Friendly aliases so the ini can say what people actually call the keys. */
static const struct { const char *alias; const char *real; } KEYALIAS[] = {
    {"/","SLASH"},{",","COMMA"},{".","PERIOD"},{";","SEMICOLON"},{"'","APOSTROPHE"},
    {"-","MINUS"},{"=","EQUALS"},{"[","LBRACKET"},{"]","RBRACKET"},{"\\","BACKSLASH"},
    {"`","GRAVE"},{"OEM_2","SLASH"},{"OEM_1","SEMICOLON"},{"SPACEBAR","SPACE"},
    {"RETURN","ENTER"},{"ESCAPE","ESC"},{"SHIFT","LSHIFT"},{"CTRL","LCTRL"},
    {"ALT","LALT"},{NULL,NULL}
};

/* ------------------------------------------------------------------ */
/* Config                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    char  id[16];        /* "left" / "right" / "forward" ... ini section suffix */
    char  label[40];     /* shown in the UI */
    UINT  vid, pid;      /* USB ids of the device this button lives on */
    int   button;        /* 1-based button number, 0 = unbound */
    char  keyname[24];
    WORD  scan;
    int   ext;
    /* runtime */
    int   joy;           /* resolved winmm joystick id, -1 = not present */
    int   was_pressed;
    int   key_down;
    DWORD pulse_until;
} Binding;

typedef struct {
    Binding bind[MAX_BIND];
    int     nbind;
    int     pulse_mode;      /* 0 = hold, 1 = pulse */
    int     pulse_ms;
    int     poll_ms;
    int     log;
    char    only_when[512];  /* comma separated exe names, or "always" */
} Config;

static Config          g_cfg;
static CRITICAL_SECTION g_lock;
static HINSTANCE       g_inst;
static HWND            g_main, g_setup, g_diag;
static NOTIFYICONDATAA g_nid;
static UINT            g_taskbar_created;
static volatile LONG   g_enabled = 1;
static volatile LONG   g_learn = -1;      /* binding index being learned */
static volatile LONG   g_learn_armed = 0;
static volatile LONG   g_quit = 0;
static HANDLE          g_thread;
static char            g_dir[MAX_PATH], g_ini[MAX_PATH + 32], g_log[MAX_PATH + 32], g_exe[MAX_PATH];

/* live state, written by the poll thread and read by the UI */
typedef struct {
    int   present;
    UINT  vid, pid;
    UINT  nbuttons;
    DWORD buttons;
    char  name[128];
} JoyState;

static JoyState g_joy[MAX_JOY];
static char     g_fg_exe[MAX_PATH] = "";
static int      g_fg_ok = 0;

/* ------------------------------------------------------------------ */
/* Small helpers                                                      */
/* ------------------------------------------------------------------ */

static void trim(char *s)
{
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n && (s[n-1]==' '||s[n-1]=='\t'||s[n-1]=='\r'||s[n-1]=='\n')) s[--n] = 0;
}

static void log_line(const char *fmt, ...)
{
    if (!g_cfg.log) return;
    FILE *f = fopen(g_log, "a");
    if (!f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "%02d:%02d:%02d.%03d  ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap; va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static int key_lookup(const char *name, WORD *scan, int *ext)
{
    char buf[32];
    int i;

    if (!name || !*name) return 0;
    strncpy(buf, name, sizeof(buf) - 1);
    buf[sizeof(buf)-1] = 0;
    trim(buf);

    /* raw scancode form: 0x35 or 0xE035 */
    if ((buf[0]=='0') && (buf[1]=='x' || buf[1]=='X')) {
        unsigned v = (unsigned)strtoul(buf + 2, NULL, 16);
        if (v == 0) return 0;
        *ext  = (v & 0xE000) == 0xE000;
        *scan = (WORD)(v & 0xFF);
        return 1;
    }

    for (i = 0; KEYALIAS[i].alias; i++)
        if (_stricmp(buf, KEYALIAS[i].alias) == 0) {
            strncpy(buf, KEYALIAS[i].real, sizeof(buf) - 1);
            break;
        }

    for (i = 0; KEYTAB[i].name; i++)
        if (_stricmp(buf, KEYTAB[i].name) == 0) {
            *scan = KEYTAB[i].scan;
            *ext  = KEYTAB[i].ext;
            return 1;
        }
    return 0;
}

static void send_scan(WORD scan, int ext, int down)
{
    INPUT in;
    memset(&in, 0, sizeof(in));
    in.type       = INPUT_KEYBOARD;
    in.ki.wVk     = 0;
    in.ki.wScan   = scan;
    in.ki.dwFlags = KEYEVENTF_SCANCODE
                  | (ext  ? KEYEVENTF_EXTENDEDKEY : 0)
                  | (down ? 0 : KEYEVENTF_KEYUP);
    SendInput(1, &in, sizeof(INPUT));
}

/* Release everything we are holding. Called on focus loss, disable, unplug,
 * shutdown and exit -- a stuck key is the worst thing this app could do. */
static void release_all_keys(void)
{
    int i;
    for (i = 0; i < g_cfg.nbind; i++) {
        Binding *b = &g_cfg.bind[i];
        if (b->key_down) {
            send_scan(b->scan, b->ext, 0);
            b->key_down = 0;
            b->pulse_until = 0;
            log_line("release %s (%s)", b->id, b->keyname);
        }
        b->was_pressed = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Device enumeration                                                 */
/* ------------------------------------------------------------------ */

/* Windows stores the real product name here; joyGetDevCaps only ever gives
 * back the generic driver string. */
static void oem_name(UINT vid, UINT pid, char *out, size_t outsz)
{
    static const char *base =
        "System\\CurrentControlSet\\Control\\MediaProperties\\PrivateProperties\\Joystick\\OEM";
    char sub[256];
    HKEY roots[2] = { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE };
    int i;

    out[0] = 0;
    sprintf(sub, "%s\\VID_%04X&PID_%04X", base, vid, pid);

    for (i = 0; i < 2; i++) {
        HKEY k;
        if (RegOpenKeyExA(roots[i], sub, 0, KEY_READ, &k) == ERROR_SUCCESS) {
            DWORD type = 0, cb = (DWORD)outsz;
            if (RegQueryValueExA(k, "OEMName", NULL, &type, (BYTE *)out, &cb) == ERROR_SUCCESS
                && type == REG_SZ) {
                out[outsz-1] = 0;
                RegCloseKey(k);
                return;
            }
            RegCloseKey(k);
        }
    }
    strncpy(out, "(unnamed device)", outsz - 1);
    out[outsz-1] = 0;
}

static void scan_devices(void)
{
    UINT id;
    for (id = 0; id < MAX_JOY; id++) {
        JOYINFOEX ji;
        JOYCAPSA  caps;

        memset(&ji, 0, sizeof(ji));
        ji.dwSize  = sizeof(ji);
        ji.dwFlags = JOY_RETURNALL;

        /* JOY_RETURNALL asks for six axes plus POV. A cabinet whose HID
         * descriptor winmm can't fully map fails the whole call and would
         * otherwise vanish, buttons and all -- so fall back to asking for
         * nothing but the buttons, which is all we actually need. */
        if (joyGetPosEx(id, &ji) != JOYERR_NOERROR) {
            memset(&ji, 0, sizeof(ji));
            ji.dwSize  = sizeof(ji);
            ji.dwFlags = JOY_RETURNBUTTONS;
            if (joyGetPosEx(id, &ji) != JOYERR_NOERROR) {
                if (g_joy[id].present) log_line("joystick %u disappeared", id);
                g_joy[id].present = 0;
                g_joy[id].buttons = 0;
                continue;
            }
            if (!g_joy[id].present)
                log_line("joystick %u needs the buttons-only fallback", id);
        }

        if (joyGetDevCapsA(id, &caps, sizeof(caps)) != JOYERR_NOERROR)
            memset(&caps, 0, sizeof(caps));

        if (!g_joy[id].present) {
            g_joy[id].vid = caps.wMid;
            g_joy[id].pid = caps.wPid;
            g_joy[id].nbuttons = caps.wNumButtons;
            oem_name(caps.wMid, caps.wPid, g_joy[id].name, sizeof(g_joy[id].name));
            log_line("joystick %u present: %s VID_%04X PID_%04X buttons=%u",
                     id, g_joy[id].name, caps.wMid, caps.wPid, caps.wNumButtons);
        }
        g_joy[id].present = 1;
        g_joy[id].buttons = ji.dwButtons;
    }
}

/* Map each binding's VID/PID onto whatever joystick id it landed on this
 * session -- winmm ids are not stable across replugs. */
static void resolve_bindings(void)
{
    int i;
    UINT id;
    for (i = 0; i < g_cfg.nbind; i++) {
        Binding *b = &g_cfg.bind[i];
        b->joy = -1;
        if (!b->button) continue;
        for (id = 0; id < MAX_JOY; id++) {
            if (g_joy[id].present && g_joy[id].vid == b->vid && g_joy[id].pid == b->pid) {
                b->joy = (int)id;
                break;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Foreground window gate                                             */
/* ------------------------------------------------------------------ */

static void foreground_exe(char *out, size_t outsz)
{
    HWND  h = GetForegroundWindow();
    DWORD pid = 0;
    HANDLE ph;

    out[0] = 0;
    if (!h) return;
    GetWindowThreadProcessId(h, &pid);
    if (!pid) return;

    ph = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!ph) return;

    {
        char path[MAX_PATH];
        DWORD cb = MAX_PATH;
        if (QueryFullProcessImageNameA(ph, 0, path, &cb)) {
            char *slash = strrchr(path, '\\');
            snprintf(out, outsz, "%s", slash ? slash + 1 : path);
        }
    }
    CloseHandle(ph);
}

static int foreground_allowed(const char *exe)
{
    char list[512], *tok;
    if (_stricmp(g_cfg.only_when, "always") == 0) return 1;
    if (!exe || !*exe) return 0;

    strncpy(list, g_cfg.only_when, sizeof(list) - 1);
    list[sizeof(list)-1] = 0;

    for (tok = strtok(list, ","); tok; tok = strtok(NULL, ",")) {
        trim(tok);
        if (*tok && _stricmp(tok, exe) == 0) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Config load / save                                                 */
/* ------------------------------------------------------------------ */

static void default_binding(Binding *b, const char *id, const char *label, const char *key)
{
    memset(b, 0, sizeof(*b));
    strncpy(b->id, id, sizeof(b->id) - 1);
    strncpy(b->label, label, sizeof(b->label) - 1);
    strncpy(b->keyname, key, sizeof(b->keyname) - 1);
    key_lookup(key, &b->scan, &b->ext);
    b->joy = -1;
}

static void config_defaults(void)
{
    memset(&g_cfg, 0, sizeof(g_cfg));
    default_binding(&g_cfg.bind[0], "left",    "Nudge Left",    "Z");
    default_binding(&g_cfg.bind[1], "right",   "Nudge Right",   "SLASH");
    default_binding(&g_cfg.bind[2], "forward", "Nudge Forward", "SPACE");
    g_cfg.nbind      = 3;
    g_cfg.pulse_mode = 0;
    g_cfg.pulse_ms   = 60;
    g_cfg.poll_ms    = 2;
    g_cfg.log        = 0;
    strcpy(g_cfg.only_when, "Future Pinball.exe, FPLoader.exe, BAM.exe");
}

static Binding *find_binding(const char *id)
{
    int i;
    for (i = 0; i < g_cfg.nbind; i++)
        if (_stricmp(g_cfg.bind[i].id, id) == 0) return &g_cfg.bind[i];
    return NULL;
}

static void config_load(void)
{
    FILE *f;
    char line[600], section[64] = "";
    Binding *cur = NULL;

    config_defaults();

    f = fopen(g_ini, "r");
    if (!f) { log_line("no %s yet, using defaults", g_ini); return; }

    while (fgets(line, sizeof(line), f)) {
        char *eq, *key, *val, *hash;

        /* strip comments (; and #) */
        hash = strpbrk(line, ";#");
        if (hash) *hash = 0;
        trim(line);
        if (!*line) continue;

        if (line[0] == '[') {
            char *end = strchr(line, ']');
            if (!end) continue;
            *end = 0;
            strncpy(section, line + 1, sizeof(section) - 1);
            section[sizeof(section)-1] = 0;
            trim(section);
            cur = NULL;
            if (_strnicmp(section, "binding.", 8) == 0) {
                cur = find_binding(section + 8);
                if (!cur && g_cfg.nbind < MAX_BIND) {
                    cur = &g_cfg.bind[g_cfg.nbind++];
                    default_binding(cur, section + 8, section + 8, "");
                }
            }
            continue;
        }

        eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        key = line; val = eq + 1;
        trim(key); trim(val);

        if (cur) {
            if      (_stricmp(key, "vid")    == 0) cur->vid = (UINT)strtoul(val, NULL, 0);
            else if (_stricmp(key, "pid")    == 0) cur->pid = (UINT)strtoul(val, NULL, 0);
            else if (_stricmp(key, "button") == 0) cur->button = atoi(val);
            else if (_stricmp(key, "label")  == 0) { strncpy(cur->label, val, sizeof(cur->label)-1); cur->label[sizeof(cur->label)-1]=0; }
            else if (_stricmp(key, "key")    == 0) {
                strncpy(cur->keyname, val, sizeof(cur->keyname) - 1);
                cur->keyname[sizeof(cur->keyname)-1] = 0;
                if (!key_lookup(cur->keyname, &cur->scan, &cur->ext)) {
                    cur->scan = 0;
                    log_line("unknown key name '%s' for binding %s", val, cur->id);
                }
            }
        } else if (_stricmp(section, "options") == 0) {
            if      (_stricmp(key, "mode")      == 0) g_cfg.pulse_mode = (_stricmp(val, "pulse") == 0);
            else if (_stricmp(key, "pulse_ms")  == 0) g_cfg.pulse_ms = atoi(val);
            else if (_stricmp(key, "poll_ms")   == 0) g_cfg.poll_ms = atoi(val);
            else if (_stricmp(key, "log")       == 0) g_cfg.log = atoi(val);
            else if (_stricmp(key, "only_when") == 0) { strncpy(g_cfg.only_when, val, sizeof(g_cfg.only_when)-1); g_cfg.only_when[sizeof(g_cfg.only_when)-1]=0; }
        }
    }
    fclose(f);

    if (g_cfg.pulse_ms < 10)  g_cfg.pulse_ms = 10;
    if (g_cfg.pulse_ms > 500) g_cfg.pulse_ms = 500;
    if (g_cfg.poll_ms  < 1)   g_cfg.poll_ms  = 1;
    if (g_cfg.poll_ms  > 50)  g_cfg.poll_ms  = 50;
}

static void config_save(void)
{
    FILE *f = fopen(g_ini, "w");
    int i;
    if (!f) {
        MessageBoxA(g_setup ? g_setup : g_main,
                    "Could not write nudge.ini.\n\nIf the app lives in Program Files, move it to a\nnormal folder such as Documents.",
                    APP_NAME, MB_OK | MB_ICONWARNING);
        return;
    }

    fprintf(f,
        "; %s %s -- configuration\n"
        ";\n"
        "; Button numbers below were learned from your own cabinet, and are stored\n"
        "; against the device's USB VID/PID so they survive a replug.\n"
        "; Re-run Setup from the tray menu to change them.\n\n",
        APP_NAME, APP_VERSION);

    for (i = 0; i < g_cfg.nbind; i++) {
        Binding *b = &g_cfg.bind[i];
        fprintf(f, "[binding.%s]\n", b->id);
        fprintf(f, "label  = %s\n", b->label);
        fprintf(f, "vid    = 0x%04X\n", b->vid);
        fprintf(f, "pid    = 0x%04X\n", b->pid);
        fprintf(f, "button = %d\n", b->button);
        fprintf(f, "key    = %s\n\n", b->keyname);
    }

    fprintf(f,
        "[options]\n"
        "; hold  = key is held exactly as long as the button is held\n"
        "; pulse = key is tapped for pulse_ms on each press (use this if holding\n"
        ";         a nudge button makes Future Pinball tilt too easily)\n"
        "mode      = %s\n"
        "pulse_ms  = %d\n\n"
        "; Keys are only injected while one of these programs is in the foreground.\n"
        "; Set to 'always' to send them regardless. The Diagnostics window shows the\n"
        "; current foreground process name if you need to add one.\n"
        "only_when = %s\n\n"
        "; Polling interval in ms (1-50). 2 is smooth and cheap.\n"
        "poll_ms   = %d\n\n"
        "; 1 writes %s next to the exe.\n"
        "log       = %d\n",
        g_cfg.pulse_mode ? "pulse" : "hold",
        g_cfg.pulse_ms,
        g_cfg.only_when,
        g_cfg.poll_ms,
        LOG_NAME,
        g_cfg.log);

    fclose(f);
}

/* ------------------------------------------------------------------ */
/* Autorun                                                            */
/* ------------------------------------------------------------------ */

static int autorun_enabled(void)
{
    HKEY k; int on = 0;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        if (RegQueryValueExA(k, RUN_VALUE, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) on = 1;
        RegCloseKey(k);
    }
    return on;
}

static void autorun_set(int on)
{
    HKEY k;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        char q[MAX_PATH + 4];
        sprintf(q, "\"%s\"", g_exe);
        RegSetValueExA(k, RUN_VALUE, 0, REG_SZ, (const BYTE *)q, (DWORD)strlen(q) + 1);
    } else {
        RegDeleteValueA(k, RUN_VALUE);
    }
    RegCloseKey(k);
}

/* ------------------------------------------------------------------ */
/* Poll thread                                                        */
/* ------------------------------------------------------------------ */

static DWORD WINAPI poll_thread(LPVOID arg)
{
    DWORD prev[MAX_JOY];
    DWORD last_fg = 0, last_scan = 0;
    int   i, first_pass = 1;

    (void)arg;
    memset(prev, 0, sizeof(prev));

    while (!g_quit) {
        DWORD now = GetTickCount();

        EnterCriticalSection(&g_lock);

        scan_devices();

        if (first_pass || now - last_scan > 1000) {
            resolve_bindings();
            last_scan = now;
        }

        if (first_pass || now - last_fg > 100) {
            foreground_exe(g_fg_exe, sizeof(g_fg_exe));
            g_fg_ok = foreground_allowed(g_fg_exe);
            last_fg = now;
        }
        first_pass = 0;

        /* Learn mode: first pass takes a baseline so a button that is already
         * held when you click Bind doesn't count. */
        if (g_learn >= 0) {
            release_all_keys();
            if (g_learn_armed) {
                for (i = 0; i < MAX_JOY; i++) prev[i] = g_joy[i].buttons;
                g_learn_armed = 0;
            } else {
                for (i = 0; i < MAX_JOY; i++) {
                    DWORD fresh;
                    if (!g_joy[i].present) continue;
                    fresh = g_joy[i].buttons & ~prev[i];
                    if (fresh) {
                        int bit;
                        for (bit = 0; bit < 32; bit++) {
                            if (fresh & (1u << bit)) {
                                HWND target = g_setup;
                                g_learn = -1;
                                if (target) PostMessageA(target, WM_BOUND, (WPARAM)i, (LPARAM)(bit + 1));
                                break;
                            }
                        }
                        break;
                    }
                }
            }
            for (i = 0; i < MAX_JOY; i++) prev[i] = g_joy[i].buttons;
            LeaveCriticalSection(&g_lock);
            Sleep(g_cfg.poll_ms);
            continue;
        }

        if (!g_enabled || !g_fg_ok) {
            release_all_keys();
        } else {
            for (i = 0; i < g_cfg.nbind; i++) {
                Binding *b = &g_cfg.bind[i];
                int pressed;

                if (!b->button || !b->scan) continue;

                if (b->joy < 0 || !g_joy[b->joy].present) {
                    /* device vanished mid-press */
                    if (b->key_down) { send_scan(b->scan, b->ext, 0); b->key_down = 0; }
                    b->was_pressed = 0;
                    continue;
                }

                pressed = (g_joy[b->joy].buttons & (1u << (b->button - 1))) ? 1 : 0;

                if (g_cfg.pulse_mode) {
                    /* expire first: a tap then always spans at least one poll
                     * interval, even if pulse_ms < poll_ms */
                    if (b->key_down && (LONG)(now - b->pulse_until) >= 0) {
                        send_scan(b->scan, b->ext, 0);
                        b->key_down = 0;
                        b->pulse_until = 0;
                        log_line("%s pulse up (%s)", b->id, b->keyname);
                    }
                    if (pressed && !b->was_pressed && !b->key_down) {
                        send_scan(b->scan, b->ext, 1);
                        b->key_down = 1;
                        b->pulse_until = now + (DWORD)g_cfg.pulse_ms;
                        log_line("%s pulse down (%s)", b->id, b->keyname);
                    }
                } else {
                    if (pressed && !b->key_down) {
                        send_scan(b->scan, b->ext, 1);
                        b->key_down = 1;
                        log_line("%s down (%s)", b->id, b->keyname);
                    } else if (!pressed && b->key_down) {
                        send_scan(b->scan, b->ext, 0);
                        b->key_down = 0;
                        log_line("%s up (%s)", b->id, b->keyname);
                    }
                }
                b->was_pressed = pressed;
            }
        }

        for (i = 0; i < MAX_JOY; i++) prev[i] = g_joy[i].buttons;

        LeaveCriticalSection(&g_lock);
        Sleep(g_cfg.poll_ms);
    }

    EnterCriticalSection(&g_lock);
    release_all_keys();
    LeaveCriticalSection(&g_lock);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Tray                                                               */
/* ------------------------------------------------------------------ */

static void tray_tip(void)
{
    int bound = 0, i;
    for (i = 0; i < g_cfg.nbind; i++) if (g_cfg.bind[i].button) bound++;
    sprintf(g_nid.szTip, "%s -- %s, %d/%d buttons bound",
            APP_NAME, g_enabled ? "on" : "off", bound, g_cfg.nbind);
    Shell_NotifyIconA(NIM_MODIFY, &g_nid);
}

static void tray_add(void)
{
    memset(&g_nid, 0, sizeof(g_nid));
    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = g_main;
    g_nid.uID              = 1;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon            = LoadIconA(g_inst, "APPICON");
    if (!g_nid.hIcon) g_nid.hIcon = LoadIconA(NULL, IDI_APPLICATION);
    strcpy(g_nid.szTip, APP_NAME);
    Shell_NotifyIconA(NIM_ADD, &g_nid);
    tray_tip();
}

static void tray_menu(void)
{
    HMENU m = CreatePopupMenu();
    POINT p;

    AppendMenuA(m, MF_STRING | (g_enabled ? MF_CHECKED : 0), IDM_ENABLE, "Enabled");
    AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    AppendMenuA(m, MF_STRING, IDM_SETUP,  "Setup (learn buttons)...");
    AppendMenuA(m, MF_STRING, IDM_DIAG,   "Diagnostics...");
    AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    AppendMenuA(m, MF_STRING, IDM_OPENINI, "Open nudge.ini");
    AppendMenuA(m, MF_STRING, IDM_RELOAD,  "Reload config");
    AppendMenuA(m, MF_STRING | (autorun_enabled() ? MF_CHECKED : 0), IDM_AUTORUN, "Start with Windows");
    AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    AppendMenuA(m, MF_STRING, IDM_ABOUT, "About");
    AppendMenuA(m, MF_STRING, IDM_EXIT,  "Exit");

    GetCursorPos(&p);
    SetForegroundWindow(g_main);   /* so the menu dismisses properly */
    TrackPopupMenu(m, TPM_RIGHTBUTTON, p.x, p.y, 0, g_main, NULL);
    PostMessageA(g_main, WM_NULL, 0, 0);
    DestroyMenu(m);
}

/* ------------------------------------------------------------------ */
/* Diagnostics window                                                 */
/* ------------------------------------------------------------------ */

static void diag_refresh(void)
{
    char buf[8192];
    int  n = 0, i, ndev;

    EnterCriticalSection(&g_lock);

    n += sprintf(buf + n, "STATUS\r\n");
    n += sprintf(buf + n, "  Bridge          : %s\r\n", g_enabled ? "enabled" : "disabled");
    n += sprintf(buf + n, "  Foreground app  : %s\r\n", g_fg_exe[0] ? g_fg_exe : "(unknown)");
    n += sprintf(buf + n, "  Keys allowed now: %s\r\n", g_fg_ok ? "YES" : "no - foreground app not in only_when list");
    n += sprintf(buf + n, "  only_when       : %s\r\n", g_cfg.only_when);
    if (g_cfg.pulse_mode)
        n += sprintf(buf + n, "  Mode            : pulse (%d ms)\r\n", g_cfg.pulse_ms);
    else
        n += sprintf(buf + n, "  Mode            : hold\r\n");
    n += sprintf(buf + n, "\r\nBINDINGS\r\n");

    for (i = 0; i < g_cfg.nbind; i++) {
        Binding *b = &g_cfg.bind[i];
        int live = (b->joy >= 0 && g_joy[b->joy].present && b->button
                    && (g_joy[b->joy].buttons & (1u << (b->button - 1)))) ? 1 : 0;
        if (!b->button)
            n += sprintf(buf + n, "  %-15s not bound\r\n", b->label);
        else
            n += sprintf(buf + n, "  %-15s VID_%04X/PID_%04X button %-2d -> %-12s  %s%s\r\n",
                         b->label, b->vid, b->pid, b->button, b->keyname,
                         b->joy < 0 ? "[device not found]" : (live ? "[PRESSED]" : ""),
                         b->scan ? "" : " [unknown key name!]");
    }

    n += sprintf(buf + n, "\r\nDEVICES  (press a button to see its number)\r\n");
    ndev = 0;
    for (i = 0; i < MAX_JOY; i++) {
        int bit, first = 1;
        if (!g_joy[i].present) continue;
        ndev++;
        n += sprintf(buf + n, "  joy %-2d  %s\r\n", i, g_joy[i].name);
        n += sprintf(buf + n, "          VID_%04X  PID_%04X  %u buttons\r\n",
                     g_joy[i].vid, g_joy[i].pid, g_joy[i].nbuttons);
        n += sprintf(buf + n, "          pressed: ");
        for (bit = 0; bit < 32; bit++) {
            if (g_joy[i].buttons & (1u << bit)) {
                n += sprintf(buf + n, "%s%d", first ? "" : ", ", bit + 1);
                first = 0;
            }
        }
        if (first) n += sprintf(buf + n, "(none)");
        n += sprintf(buf + n, "\r\n\r\n");
    }
    if (!ndev) sprintf(buf + n, "  no joysticks detected -- is the cabinet in OTG controller mode?\r\n");

    LeaveCriticalSection(&g_lock);

    SetDlgItemTextA(g_diag, IDC_DIAGTEXT, buf);
}

static LRESULT CALLBACK diag_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        HWND e = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            0, 0, 10, 10, h, (HMENU)IDC_DIAGTEXT, g_inst, NULL);
        HFONT f = CreateFontA(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                              FIXED_PITCH | FF_MODERN, "Consolas");
        SendMessageA(e, WM_SETFONT, (WPARAM)f, TRUE);
        SetTimer(h, 1, 60, NULL);
        return 0;
    }
    case WM_SIZE:
        MoveWindow(GetDlgItem(h, IDC_DIAGTEXT), 0, 0, LOWORD(lp), HIWORD(lp), TRUE);
        return 0;
    case WM_TIMER:
        diag_refresh();
        return 0;
    case WM_CLOSE:
        KillTimer(h, 1);
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        g_diag = NULL;
        return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

static void diag_open(void)
{
    if (g_diag) { SetForegroundWindow(g_diag); return; }
    g_diag = CreateWindowExA(0, "LNB_Diag", APP_NAME " - Diagnostics",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 620, 620,
        NULL, NULL, g_inst, NULL);
    ShowWindow(g_diag, SW_SHOW);
}

/* ------------------------------------------------------------------ */
/* Setup window                                                       */
/* ------------------------------------------------------------------ */

static void setup_status(int i)
{
    char s[128];
    Binding *b = &g_cfg.bind[i];
    if (!b->button) strcpy(s, "not bound");
    else sprintf(s, "VID_%04X/PID_%04X  button %d", b->vid, b->pid, b->button);
    SetDlgItemTextA(g_setup, IDC_STAT_BASE + i, s);
}

static LRESULT CALLBACK setup_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        HFONT f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        int i, y = 54;

        CreateWindowExA(0, "STATIC",
            "Click Bind, then press that nudge button on the cabinet.\r\n"
            "The key column is what Future Pinball has in Preferences > Game Keys and Controls.",
            WS_CHILD | WS_VISIBLE, 14, 10, 540, 36, h, (HMENU)IDC_HINT, g_inst, NULL);

        for (i = 0; i < g_cfg.nbind; i++) {
            HWND w;
            w = CreateWindowExA(0, "STATIC", g_cfg.bind[i].label, WS_CHILD | WS_VISIBLE,
                    14, y + 4, 110, 20, h, NULL, g_inst, NULL);
            SendMessageA(w, WM_SETFONT, (WPARAM)f, TRUE);

            w = CreateWindowExA(0, "BUTTON", "Bind", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                    128, y, 60, 26, h, (HMENU)(INT_PTR)(IDC_BIND_BASE + i), g_inst, NULL);
            SendMessageA(w, WM_SETFONT, (WPARAM)f, TRUE);

            w = CreateWindowExA(0, "BUTTON", "Clear", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                    192, y, 56, 26, h, (HMENU)(INT_PTR)(IDC_CLEAR_BASE + i), g_inst, NULL);
            SendMessageA(w, WM_SETFONT, (WPARAM)f, TRUE);

            w = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", g_cfg.bind[i].keyname,
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                    256, y + 1, 90, 24, h, (HMENU)(INT_PTR)(IDC_KEY_BASE + i), g_inst, NULL);
            SendMessageA(w, WM_SETFONT, (WPARAM)f, TRUE);

            w = CreateWindowExA(0, "STATIC", "", WS_CHILD | WS_VISIBLE,
                    356, y + 4, 220, 20, h, (HMENU)(INT_PTR)(IDC_STAT_BASE + i), g_inst, NULL);
            SendMessageA(w, WM_SETFONT, (WPARAM)f, TRUE);

            y += 34;
        }

        CreateWindowExA(0, "BUTTON", "Save", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            14, y + 12, 80, 28, h, (HMENU)IDC_SAVE, g_inst, NULL);
        CreateWindowExA(0, "BUTTON", "Close", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            102, y + 12, 80, 28, h, (HMENU)IDC_CANCEL, g_inst, NULL);

        {
            HWND c = GetWindow(h, GW_CHILD);
            while (c) { SendMessageA(c, WM_SETFONT, (WPARAM)f, TRUE); c = GetWindow(c, GW_HWNDNEXT); }
        }
        for (i = 0; i < g_cfg.nbind; i++) setup_status(i);
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wp);
        int i;

        if (id >= IDC_BIND_BASE && id < IDC_BIND_BASE + g_cfg.nbind) {
            i = id - IDC_BIND_BASE;
            EnterCriticalSection(&g_lock);
            g_learn_armed = 1;
            g_learn = i;
            LeaveCriticalSection(&g_lock);
            SetDlgItemTextA(h, IDC_STAT_BASE + i, "press a button now...");
            return 0;
        }
        if (id >= IDC_CLEAR_BASE && id < IDC_CLEAR_BASE + g_cfg.nbind) {
            i = id - IDC_CLEAR_BASE;
            EnterCriticalSection(&g_lock);
            g_cfg.bind[i].button = 0;
            g_cfg.bind[i].vid = g_cfg.bind[i].pid = 0;
            g_cfg.bind[i].joy = -1;
            LeaveCriticalSection(&g_lock);
            setup_status(i);
            return 0;
        }
        if (id == IDC_SAVE) {
            char kn[24];
            int bad = -1;
            EnterCriticalSection(&g_lock);
            for (i = 0; i < g_cfg.nbind; i++) {
                GetDlgItemTextA(h, IDC_KEY_BASE + i, kn, sizeof(kn));
                trim(kn);
                snprintf(g_cfg.bind[i].keyname, sizeof(g_cfg.bind[i].keyname), "%s", kn);
                if (!key_lookup(kn, &g_cfg.bind[i].scan, &g_cfg.bind[i].ext)) {
                    g_cfg.bind[i].scan = 0;
                    if (bad < 0) bad = i;
                }
            }
            config_save();
            resolve_bindings();
            LeaveCriticalSection(&g_lock);
            tray_tip();
            if (bad >= 0) {
                char m[256];
                sprintf(m, "'%s' is not a key name I recognise, so %s will do nothing.\n\n"
                           "Try names like Z, SLASH, SPACE, COMMA, LSHIFT, RSHIFT, F5\n"
                           "or a raw scancode such as 0x35.",
                        g_cfg.bind[bad].keyname, g_cfg.bind[bad].label);
                MessageBoxA(h, m, APP_NAME, MB_OK | MB_ICONWARNING);
            } else {
                MessageBoxA(h, "Saved to nudge.ini.", APP_NAME, MB_OK | MB_ICONINFORMATION);
            }
            return 0;
        }
        if (id == IDC_CANCEL) { PostMessageA(h, WM_CLOSE, 0, 0); return 0; }
        return 0;
    }

    case WM_CLOSE:
        g_learn = -1;
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        g_setup = NULL;
        return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

/* The learn result needs to know which row asked for it; keep it here so the
 * window proc stays simple. */
static int g_learn_row = -1;

static LRESULT CALLBACK setup_router(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_BOUND) {
        int row = g_learn_row;
        if (row >= 0 && row < g_cfg.nbind) {
            int joy = (int)wp, btn = (int)lp;
            EnterCriticalSection(&g_lock);
            g_cfg.bind[row].vid    = g_joy[joy].vid;
            g_cfg.bind[row].pid    = g_joy[joy].pid;
            g_cfg.bind[row].button = btn;
            g_cfg.bind[row].joy    = joy;
            LeaveCriticalSection(&g_lock);
            setup_status(row);
        }
        g_learn_row = -1;
        return 0;
    }
    if (msg == WM_COMMAND) {
        int id = LOWORD(wp);
        if (id >= IDC_BIND_BASE && id < IDC_BIND_BASE + g_cfg.nbind)
            g_learn_row = id - IDC_BIND_BASE;
    }
    return setup_proc(h, msg, wp, lp);
}

static void setup_open(void)
{
    if (g_setup) { SetForegroundWindow(g_setup); return; }
    g_setup = CreateWindowExA(0, "LNB_Setup", APP_NAME " - Setup",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 610, 260, NULL, NULL, g_inst, NULL);
    ShowWindow(g_setup, SW_SHOW);
    SetForegroundWindow(g_setup);
}

/* ------------------------------------------------------------------ */
/* Main window                                                        */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK main_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == g_taskbar_created) { tray_add(); return 0; }

    switch (msg) {
    case WM_TRAYICON:
        if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU) tray_menu();
        else if (lp == WM_LBUTTONDBLCLK) diag_open();
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_ENABLE:
            g_enabled = !g_enabled;
            if (!g_enabled) {
                EnterCriticalSection(&g_lock);
                release_all_keys();
                LeaveCriticalSection(&g_lock);
            }
            tray_tip();
            return 0;
        case IDM_SETUP:  setup_open(); return 0;
        case IDM_DIAG:   diag_open();  return 0;
        case IDM_OPENINI:
            if (GetFileAttributesA(g_ini) == INVALID_FILE_ATTRIBUTES) {
                EnterCriticalSection(&g_lock);
                config_save();
                LeaveCriticalSection(&g_lock);
            }
            ShellExecuteA(NULL, "open", "notepad.exe", g_ini, NULL, SW_SHOWNORMAL);
            return 0;
        case IDM_RELOAD:
            EnterCriticalSection(&g_lock);
            release_all_keys();
            config_load();
            resolve_bindings();
            LeaveCriticalSection(&g_lock);
            tray_tip();
            return 0;
        case IDM_AUTORUN:
            autorun_set(!autorun_enabled());
            return 0;
        case IDM_ABOUT:
            MessageBoxA(h,
                APP_NAME " " APP_VERSION "\n\n"
                "Bridges the physical nudge buttons on an AtGames Legends cabinet\n"
                "(OTG / controller mode) to the keyboard keys Future Pinball wants,\n"
                "because FP's Digital Nudge fields only accept keyboard input.\n\n"
                "Right-click the tray icon for Setup and Diagnostics.",
                APP_NAME, MB_OK | MB_ICONINFORMATION);
            return 0;
        case IDM_EXIT:
            PostMessageA(h, WM_CLOSE, 0, 0);
            return 0;
        }
        return 0;

    case WM_QUERYENDSESSION:
        EnterCriticalSection(&g_lock);
        release_all_keys();
        LeaveCriticalSection(&g_lock);
        return TRUE;

    case WM_ENDSESSION:
        g_quit = 1;
        EnterCriticalSection(&g_lock);
        release_all_keys();
        LeaveCriticalSection(&g_lock);
        return 0;

    case WM_CLOSE:
        DestroyWindow(h);
        return 0;

    case WM_DESTROY:
        Shell_NotifyIconA(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

/* ------------------------------------------------------------------ */

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    WNDCLASSA wc;
    MSG msg;
    HANDLE mtx;
    char *slash;
    int first_run;

    (void)prev; (void)cmd; (void)show;
    g_inst = inst;

    mtx = CreateMutexA(NULL, TRUE, APP_MUTEX);
    if (mtx && GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxA(NULL, APP_NAME " is already running (look in the system tray).",
                    APP_NAME, MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    GetModuleFileNameA(NULL, g_exe, MAX_PATH);
    strcpy(g_dir, g_exe);
    slash = strrchr(g_dir, '\\');
    if (slash) *(slash + 1) = 0;
    snprintf(g_ini, sizeof(g_ini), "%s%s", g_dir, INI_NAME);
    snprintf(g_log, sizeof(g_log), "%s%s", g_dir, LOG_NAME);

    InitializeCriticalSection(&g_lock);
    first_run = (GetFileAttributesA(g_ini) == INVALID_FILE_ATTRIBUTES);
    config_load();

    /* Without this our 2 ms poll sleep is really ~15 ms and nudge feels late. */
    timeBeginPeriod(1);

    g_taskbar_created = RegisterWindowMessageA("TaskbarCreated");

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = main_proc;
    wc.hInstance     = inst;
    wc.lpszClassName = "LNB_Main";
    RegisterClassA(&wc);

    wc.lpfnWndProc   = setup_router;
    wc.lpszClassName = "LNB_Setup";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
    wc.hIcon         = LoadIconA(inst, "APPICON");
    RegisterClassA(&wc);

    wc.lpfnWndProc   = diag_proc;
    wc.lpszClassName = "LNB_Diag";
    RegisterClassA(&wc);

    /* A hidden top-level window, not HWND_MESSAGE: the tray menu needs a window
     * it can foreground, and WM_QUERYENDSESSION only reaches top-level windows. */
    g_main = CreateWindowExA(WS_EX_TOOLWINDOW, "LNB_Main", APP_NAME,
                             WS_OVERLAPPED, 0, 0, 0, 0, NULL, NULL, inst, NULL);
    tray_add();

    g_thread = CreateThread(NULL, 0, poll_thread, NULL, 0, NULL);
    if (g_thread) SetThreadPriority(g_thread, THREAD_PRIORITY_ABOVE_NORMAL);

    if (first_run) {
        MessageBoxA(NULL,
            "First run -- nothing is bound yet.\n\n"
            "Put the cabinet in OTG controller mode and connect it, then use the\n"
            "Setup window that follows: click Bind and press each nudge button.\n\n"
            "The app lives in the system tray (right-click it any time).",
            APP_NAME, MB_OK | MB_ICONINFORMATION);
        setup_open();
    }

    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        if (!g_setup || !IsDialogMessageA(g_setup, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }

    g_quit = 1;
    if (g_thread) { WaitForSingleObject(g_thread, 1000); CloseHandle(g_thread); }
    EnterCriticalSection(&g_lock);
    release_all_keys();
    LeaveCriticalSection(&g_lock);
    timeEndPeriod(1);
    DeleteCriticalSection(&g_lock);
    if (mtx) CloseHandle(mtx);
    return 0;
}
