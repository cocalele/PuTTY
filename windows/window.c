/*
 * window.c - the PuTTY(tel) main program, which runs a PuTTY terminal
 * emulator and backend in a window.
 */

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include <limits.h>
#include <assert.h>

#ifdef __WINE__
#define NO_MULTIMON                    /* winelib doesn't have this */
#endif

#ifndef NO_MULTIMON
#define COMPILE_MULTIMON_STUBS
#endif

#define PUTTY_DO_GLOBALS               /* actually _define_ globals */
#include "putty.h"
#include "terminal.h"
#include "storage.h"
#include "win_res.h"
#include "winsecur.h"
#include "tree234.h"

#ifndef NO_MULTIMON
#include <multimon.h>
#endif

#include <imm.h>
#include <commctrl.h>
#include <richedit.h>
#include <mmsystem.h>

/* From MSDN: In the WM_SYSCOMMAND message, the four low-order bits of
 * wParam are used by Windows, and should be masked off, so we shouldn't
 * attempt to store information in them. Hence all these identifiers have
 * the low 4 bits clear. Also, identifiers should < 0xF000. */

#define IDM_SHOWLOG   0x0010
#define IDM_NEWSESS   0x0020
#define IDM_DUPSESS   0x0030
#define IDM_RESTART   0x0040
#define IDM_RECONF    0x0050
#define IDM_CLRSB     0x0060
#define IDM_RESET     0x0070
#define IDM_HELP      0x0140
#define IDM_ABOUT     0x0150
#define IDM_SAVEDSESS 0x0160
#define IDM_COPYALL   0x0170
#define IDM_FULLSCREEN  0x0180
#define IDM_COPY      0x0190
#define IDM_PASTE     0x01A0
#define IDM_SPECIALSEP 0x0200

#define IDM_SPECIAL_MIN 0x0400
#define IDM_SPECIAL_MAX 0x0800

#define IDM_SAVED_MIN 0x1000
#define IDM_SAVED_MAX 0x5000
#define MENU_SAVED_STEP 16
/* Maximum number of sessions on saved-session submenu */
#define MENU_SAVED_MAX ((IDM_SAVED_MAX-IDM_SAVED_MIN) / MENU_SAVED_STEP)

#define WM_IGNORE_CLIP (WM_APP + 2)
#define WM_FULLSCR_ON_MAX (WM_APP + 3)
#define WM_AGENT_CALLBACK (WM_APP + 4)
#define WM_GOT_CLIPDATA (WM_APP + 6)

/* Needed for Chinese support and apparently not always defined. */
#ifndef VK_PROCESSKEY
#define VK_PROCESSKEY 0xE5
#endif

/* Mouse wheel support. */
#ifndef WM_MOUSEWHEEL
#define WM_MOUSEWHEEL 0x020A           /* not defined in earlier SDKs */
#endif
#ifndef WHEEL_DELTA
#define WHEEL_DELTA 120
#endif

/* VK_PACKET, used to send Unicode characters in WM_KEYDOWNs */
#ifndef VK_PACKET
#define VK_PACKET 0xE7
#endif

static Mouse_Button translate_button(Mouse_Button button);
static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
static int TranslateKey(UINT message, WPARAM wParam, LPARAM lParam,
                        unsigned char *output);
static void conftopalette(void);
static void systopalette(void);
static void init_palette(void);
static void init_fonts(int, int);
static void another_font(int);
static void deinit_fonts(void);
static void set_input_locale(HKL);
static void update_savedsess_menu(void);
static void init_winfuncs(void);

static bool is_full_screen(void);
static void make_full_screen(void);
static void clear_full_screen(void);
static void flip_full_screen(void);
static void process_clipdata(HGLOBAL clipdata, bool unicode);
static void setup_clipboards(Terminal *, Conf *);

/* Window layout information */
static void reset_window(int);
static int extra_width, extra_height;
static int font_width, font_height;
static bool font_dualwidth, font_varpitch;
static int offset_width, offset_height;
static bool was_zoomed = false;
static int prev_rows, prev_cols;

static void flash_window(int mode);
static void sys_cursor_update(void);
static bool get_fullscreen_rect(RECT * ss);

static int caret_x = -1, caret_y = -1;

static int kbd_codepage;

static Ldisc *ldisc;
static Backend *backend;

static struct unicode_data ucsdata;
static bool session_closed;
static bool reconfiguring = false;

static const SessionSpecial *specials = NULL;
static HMENU specials_menu = NULL;
static int n_specials = 0;

#define TIMING_TIMER_ID 1234
static long timing_next_time;

static struct {
    HMENU menu;
} popup_menus[2];
enum { SYSMENU, CTXMENU };
static HMENU savedsess_menu;

struct wm_netevent_params {
    /* Used to pass data to wm_netevent_callback */
    WPARAM wParam;
    LPARAM lParam;
};

static void conf_cache_data(void);
int cursor_type;
int vtmode;

static struct sesslist sesslist;       /* for saved-session menu */

struct agent_callback {
    void (*callback)(void *, void *, int);
    void *callback_ctx;
    void *data;
    int len;
};

#define FONT_NORMAL 0
#define FONT_BOLD 1
#define FONT_UNDERLINE 2
#define FONT_BOLDUND 3
#define FONT_WIDE       0x04
#define FONT_HIGH       0x08
#define FONT_NARROW     0x10

#define FONT_OEM        0x20
#define FONT_OEMBOLD    0x21
#define FONT_OEMUND     0x22
#define FONT_OEMBOLDUND 0x23

#define FONT_MAXNO      0x40
#define FONT_SHIFT      5
static HFONT fonts[FONT_MAXNO];
static LOGFONT lfont;
static bool fontflag[FONT_MAXNO];
static enum {
    BOLD_NONE, BOLD_SHADOW, BOLD_FONT
} bold_font_mode;
static bool bold_colours;
static enum {
    UND_LINE, UND_FONT
} und_mode;
static int descent;

#define NCFGCOLOURS 22
#define NEXTCOLOURS 240
#define NALLCOLOURS (NCFGCOLOURS + NEXTCOLOURS)
static COLORREF colours[NALLCOLOURS];
struct rgb {
    int r, g, b;
} colours_rgb[NALLCOLOURS];
static HPALETTE pal;
static LPLOGPALETTE logpal;
static RGBTRIPLE defpal[NALLCOLOURS];

static HBITMAP caretbm;

static int dbltime, lasttime, lastact;
static Mouse_Button lastbtn;

/* this allows xterm-style mouse handling. */
static bool send_raw_mouse = false;
static int wheel_accumulator = 0;

static BusyStatus busy_status = BUSY_NOT;

static char *window_name, *icon_name;

static int compose_state = 0;

static UINT wm_mousewheel = WM_MOUSEWHEEL;

#define IS_HIGH_VARSEL(wch1, wch2) \
    ((wch1) == 0xDB40 && ((wch2) >= 0xDD00 && (wch2) <= 0xDDEF))
#define IS_LOW_VARSEL(wch) \
    (((wch) >= 0x180B && (wch) <= 0x180D) || /* MONGOLIAN FREE VARIATION SELECTOR */ \
     ((wch) >= 0xFE00 && (wch) <= 0xFE0F)) /* VARIATION SELECTOR 1-16 */

static bool wintw_setup_draw_ctx(TermWin *);
static void wintw_draw_text(TermWin *, int x, int y, wchar_t *text, int len,
                            unsigned long attrs, int lattrs, truecolour tc);
static void wintw_draw_cursor(TermWin *, int x, int y, wchar_t *text, int len,
                              unsigned long attrs, int lattrs, truecolour tc);
static void wintw_draw_trust_sigil(TermWin *, int x, int y);
static int wintw_char_width(TermWin *, int uc);
static void wintw_free_draw_ctx(TermWin *);
static void wintw_set_cursor_pos(TermWin *, int x, int y);
static void wintw_set_raw_mouse_mode(TermWin *, bool enable);
static void wintw_set_scrollbar(TermWin *, int total, int start, int page);
static void wintw_bell(TermWin *, int mode);
static void wintw_clip_write(
    TermWin *, int clipboard, wchar_t *text, int *attrs,
    truecolour *colours, int len, bool must_deselect);
static void wintw_clip_request_paste(TermWin *, int clipboard);
static void wintw_refresh(TermWin *);
static void wintw_request_resize(TermWin *, int w, int h);
static void wintw_set_title(TermWin *, const char *title);
static void wintw_set_icon_title(TermWin *, const char *icontitle);
static void wintw_set_minimised(TermWin *, bool minimised);
static bool wintw_is_minimised(TermWin *);
static void wintw_set_maximised(TermWin *, bool maximised);
static void wintw_move(TermWin *, int x, int y);
static void wintw_set_zorder(TermWin *, bool top);
static bool wintw_palette_get(TermWin *, int n, int *r, int *g, int *b);
static void wintw_palette_set(TermWin *, int n, int r, int g, int b);
static void wintw_palette_reset(TermWin *);
static void wintw_get_pos(TermWin *, int *x, int *y);
static void wintw_get_pixels(TermWin *, int *x, int *y);
static const char *wintw_get_title(TermWin *, bool icon);
static bool wintw_is_utf8(TermWin *);

static const TermWinVtable windows_termwin_vt = {
    wintw_setup_draw_ctx,
    wintw_draw_text,
    wintw_draw_cursor,
    wintw_draw_trust_sigil,
    wintw_char_width,
    wintw_free_draw_ctx,
    wintw_set_cursor_pos,
    wintw_set_raw_mouse_mode,
    wintw_set_scrollbar,
    wintw_bell,
    wintw_clip_write,
    wintw_clip_request_paste,
    wintw_refresh,
    wintw_request_resize,
    wintw_set_title,
    wintw_set_icon_title,
    wintw_set_minimised,
    wintw_is_minimised,
    wintw_set_maximised,
    wintw_move,
    wintw_set_zorder,
    wintw_palette_get,
    wintw_palette_set,
    wintw_palette_reset,
    wintw_get_pos,
    wintw_get_pixels,
    wintw_get_title,
    wintw_is_utf8,
};

static TermWin wintw[1];
static HDC wintw_hdc;

static HICON trust_icon = INVALID_HANDLE_VALUE;

const bool share_can_be_downstream = true;
const bool share_can_be_upstream = true;

static bool is_utf8(void)
{
    return ucsdata.line_codepage == CP_UTF8;
}

static bool wintw_is_utf8(TermWin *tw)
{
    return is_utf8();
}

static bool win_seat_is_utf8(Seat *seat)
{
    return is_utf8();
}

char *win_seat_get_ttymode(Seat *seat, const char *mode)
{
    return term_get_ttymode(term, mode);
}

bool win_seat_get_window_pixel_size(Seat *seat, int *x, int *y)
{
    win_get_pixels(wintw, x, y);
    return true;
}

StripCtrlChars *win_seat_stripctrl_new(
    Seat *seat, BinarySink *bs_out, SeatInteractionContext sic)
{
    return stripctrl_new_term(bs_out, false, 0, term);
}

static size_t win_seat_output(
    Seat *seat, bool is_stderr, const void *, size_t);
static bool win_seat_eof(Seat *seat);
static int win_seat_get_userpass_input(
    Seat *seat, prompts_t *p, bufchain *input);
static void win_seat_notify_remote_exit(Seat *seat);
static void win_seat_connection_fatal(Seat *seat, const char *msg);
static void win_seat_update_specials_menu(Seat *seat);
static void win_seat_set_busy_status(Seat *seat, BusyStatus status);
static bool win_seat_set_trust_status(Seat *seat, bool trusted);

static const SeatVtable win_seat_vt = {
    win_seat_output,
    win_seat_eof,
    win_seat_get_userpass_input,
    win_seat_notify_remote_exit,
    win_seat_connection_fatal,
    win_seat_update_specials_menu,
    win_seat_get_ttymode,
    win_seat_set_busy_status,
    win_seat_verify_ssh_host_key,
    win_seat_confirm_weak_crypto_primitive,
    win_seat_confirm_weak_cached_hostkey,
    win_seat_is_utf8,
    nullseat_echoedit_update,
    nullseat_get_x_display,
    nullseat_get_windowid,
    win_seat_get_window_pixel_size,
    win_seat_stripctrl_new,
    win_seat_set_trust_status,
    nullseat_verbose_yes,
    nullseat_interactive_yes,
};
static Seat win_seat_impl = { &win_seat_vt };
Seat *const win_seat = &win_seat_impl;

static void start_backend(void)
{
    const struct BackendVtable *vt;
    const char *error;
    const char *title;
    char *realhost;
    int i;

    /*
     * Select protocol. This is farmed out into a table in a
     * separate file to enable an ssh-free variant.
     */
    vt = backend_vt_from_proto(conf_get_int(conf, CONF_protocol));
    if (!vt) {
        char *str = dupprintf("%s Internal Error", appname);
        MessageBox(NULL, "Unsupported protocol number found",
                   str, MB_OK | MB_ICONEXCLAMATION);
        sfree(str);
        cleanup_exit(1);
    }

    seat_set_trust_status(win_seat, true);
    error = backend_init(vt, win_seat, &backend, logctx, conf,
                         conf_get_str(conf, CONF_host),
                         conf_get_int(conf, CONF_port),
                         &realhost,
                         conf_get_bool(conf, CONF_tcp_nodelay),
                         conf_get_bool(conf, CONF_tcp_keepalives));
    if (error) {
        char *str = dupprintf("%s Error", appname);
        char *msg = dupprintf("Unable to open connection to\n%s\n%s",
                              conf_dest(conf), error);
        MessageBox(NULL, msg, str, MB_ICONERROR | MB_OK);
        sfree(str);
        sfree(msg);
        exit(0);
    }
    window_name = icon_name = NULL;
    char *title_to_free = NULL;
    title = conf_get_str(conf, CONF_wintitle);
    if (!*title) {
        title_to_free = dupprintf("%s - %s", realhost, appname);
        title = title_to_free;
    }
    sfree(realhost);
    win_set_title(wintw, title);
    win_set_icon_title(wintw, title);

    /*
     * Connect the terminal to the backend for resize purposes.
     */
    term_provide_backend(term, backend);

    /*
     * Set up a line discipline.
     */
    ldisc = ldisc_create(conf, term, backend, win_seat);

    /*
     * Destroy the Restart Session menu item. (This will return
     * failure if it's already absent, as it will be the very first
     * time we call this function. We ignore that, because as long
     * as the menu item ends up not being there, we don't care
     * whether it was us who removed it or not!)
     */
    for (i = 0; i < lenof(popup_menus); i++) {
        DeleteMenu(popup_menus[i].menu, IDM_RESTART, MF_BYCOMMAND);
    }

    session_closed = false;

    sfree(title_to_free);
}

static void close_session(void *ignored_context)
{
    char *newtitle;
    int i;

    session_closed = true;
    newtitle = dupprintf("%s (inactive)", appname);
    win_set_icon_title(wintw, newtitle);
    win_set_title(wintw, newtitle);
    sfree(newtitle);

    if (ldisc) {
        ldisc_free(ldisc);
        ldisc = NULL;
    }
    if (backend) {
        backend_free(backend);
        backend = NULL;
        term_provide_backend(term, NULL);
        seat_update_specials_menu(win_seat);
    }

    /*
     * Show the Restart Session menu item. Do a precautionary
     * delete first to ensure we never end up with more than one.
     */
    for (i = 0; i < lenof(popup_menus); i++) {
        DeleteMenu(popup_menus[i].menu, IDM_RESTART, MF_BYCOMMAND);
        InsertMenu(popup_menus[i].menu, IDM_DUPSESS, MF_BYCOMMAND | MF_ENABLED,
                   IDM_RESTART, "&Restart Session");
    }
}

extern LogPolicy win_gui_logpolicy[1]; /* defined in windlg.c */

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
    MSG msg;
    HRESULT hr;
    int guess_width, guess_height;

    dll_hijacking_protection();

    hinst = inst;
    hwnd = NULL;
    cmdline_tooltype |= TOOLTYPE_HOST_ARG | TOOLTYPE_PORT_ARG |
        TOOLTYPE_NO_VERBOSE_OPTION;

    sk_init();

    init_common_controls();

    /* Set Explicit App User Model Id so that jump lists don't cause
       PuTTY to hang on to removable media. */

    set_explicit_app_user_model_id();

    /* Ensure a Maximize setting in Explorer doesn't maximise the
     * config box. */
    defuse_showwindow();

    init_winver();

    /*
     * If we're running a version of Windows that doesn't support
     * WM_MOUSEWHEEL, find out what message number we should be
     * using instead.
     */
    if (osMajorVersion < 4 ||
        (osMajorVersion == 4 && osPlatformId != VER_PLATFORM_WIN32_NT))
        wm_mousewheel = RegisterWindowMessage("MSWHEEL_ROLLMSG");

    init_help();

    init_winfuncs();

    conf = conf_new();

    /*
     * Initialize COM.
     */
    hr = CoInitialize(NULL);
    if (hr != S_OK && hr != S_FALSE) {
        char *str = dupprintf("%s Fatal Error", appname);
        MessageBox(NULL, "Failed to initialize COM subsystem",
                   str, MB_OK | MB_ICONEXCLAMATION);
        sfree(str);
        return 1;
    }

    /*
     * Process the command line.
     */
    {
        char *p;
        bool special_launchable_argument = false;

        default_protocol = be_default_protocol;
        /* Find the appropriate default port. */
        {
            const struct BackendVtable *vt =
                backend_vt_from_proto(default_protocol);
            default_port = 0; /* illegal */
            if (vt)
                default_port = vt->default_port;
        }
        conf_set_int(conf, CONF_logtype, LGTYP_NONE);

        do_defaults(NULL, conf);

        p = cmdline;

        /*
         * Process a couple of command-line options which are more
         * easily dealt with before the line is broken up into words.
         * These are the old-fashioned but convenient @sessionname and
         * the internal-use-only &sharedmemoryhandle, plus the &R
         * prefix for -restrict-acl, all of which are used by PuTTYs
         * auto-launching each other via System-menu options.
         */
        while (*p && isspace(*p))
            p++;
        if (*p == '&' && p[1] == 'R' &&
            (!p[2] || p[2] == '@' || p[2] == '&')) {
            /* &R restrict-acl prefix */
            restrict_process_acl();
            restricted_acl = true;
            p += 2;
        }

        if (*p == '@') {
            /*
             * An initial @ means that the whole of the rest of the
             * command line should be treated as the name of a saved
             * session, with _no quoting or escaping_. This makes it a
             * very convenient means of automated saved-session
             * launching, via IDM_SAVEDSESS or Windows 7 jump lists.
             */
            int i = strlen(p);
            while (i > 1 && isspace(p[i - 1]))
                i--;
            p[i] = '\0';
            do_defaults(p + 1, conf);
            if (!conf_launchable(conf) && !do_config()) {
                cleanup_exit(0);
            }
            special_launchable_argument = true;
        } else if (*p == '&') {
            /*
             * An initial & means we've been given a command line
             * containing the hex value of a HANDLE for a file
             * mapping object, which we must then interpret as a
             * serialised Conf.
             */
            HANDLE filemap;
            void *cp;
            unsigned cpsize;
            if (sscanf(p + 1, "%p:%u", &filemap, &cpsize) == 2 &&
                (cp = MapViewOfFile(filemap, FILE_MAP_READ,
                                    0, 0, cpsize)) != NULL) {
                BinarySource src[1];
                BinarySource_BARE_INIT(src, cp, cpsize);
                if (!conf_deserialise(conf, src))
                    modalfatalbox("Serialised configuration data was invalid");
                UnmapViewOfFile(cp);
                CloseHandle(filemap);
            } else if (!do_config()) {
                cleanup_exit(0);
            }
            special_launchable_argument = true;
        } else if (!*p) {
            /* Do-nothing case for an empty command line - or rather,
             * for a command line that's empty _after_ we strip off
             * the &R prefix. */
        } else {
            /*
             * Otherwise, break up the command line and deal with
             * it sensibly.
             */
            int argc, i;
            char **argv;

            split_into_argv(cmdline, &argc, &argv, NULL);

            for (i = 0; i < argc; i++) {
                char *p = argv[i];
                int ret;

                ret = cmdline_process_param(p, i+1<argc?argv[i+1]:NULL,
                                            1, conf);
                if (ret == -2) {
                    cmdline_error("option \"%s\" requires an argument", p);
                } else if (ret == 2) {
                    i++;               /* skip next argument */
                } else if (ret == 1) {
                    continue;          /* nothing further needs doing */
                } else if (!strcmp(p, "-cleanup")) {
                    /*
                     * `putty -cleanup'. Remove all registry
                     * entries associated with PuTTY, and also find
                     * and delete the random seed file.
                     */
                    char *s1, *s2;
                    s1 = dupprintf("This procedure will remove ALL Registry entries\n"
                                   "associated with %s, and will also remove\n"
                                   "the random seed file. (This only affects the\n"
                                   "currently logged-in user.)\n"
                                   "\n"
                                   "THIS PROCESS WILL DESTROY YOUR SAVED SESSIONS.\n"
                                   "Are you really sure you want to continue?",
                                   appname);
                    s2 = dupprintf("%s Warning", appname);
                    if (message_box(s1, s2,
                                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2,
                                    HELPCTXID(option_cleanup)) == IDYES) {
                        cleanup_all();
                    }
                    sfree(s1);
                    sfree(s2);
                    exit(0);
                } else if (!strcmp(p, "-pgpfp")) {
                    pgp_fingerprints();
                    exit(1);
                } else if (*p != '-') {
                    cmdline_error("unexpected argument \"%s\"", p);
                } else {
                    cmdline_error("unknown option \"%s\"", p);
                }
            }
        }

        cmdline_run_saved(conf);

        /*
         * Bring up the config dialog if the command line hasn't
         * (explicitly) specified a launchable configuration.
         */
        if (!(special_launchable_argument || cmdline_host_ok(conf))) {
            if (!do_config())
                cleanup_exit(0);
        }

        prepare_session(conf);
    }

    if (!prev) {
        WNDCLASSW wndclass;

        wndclass.style = 0;
        wndclass.lpfnWndProc = WndProc;
        wndclass.cbClsExtra = 0;
        wndclass.cbWndExtra = 0;
        wndclass.hInstance = inst;
        wndclass.hIcon = LoadIcon(inst, MAKEINTRESOURCE(IDI_MAINICON));
        wndclass.hCursor = LoadCursor(NULL, IDC_IBEAM);
        wndclass.hbrBackground = NULL;
        wndclass.lpszMenuName = NULL;
        wndclass.lpszClassName = dup_mb_to_wc(DEFAULT_CODEPAGE, 0, appname);

        RegisterClassW(&wndclass);
    }

    memset(&ucsdata, 0, sizeof(ucsdata));

    conf_cache_data();

    conftopalette();

    /*
     * Guess some defaults for the window size. This all gets
     * updated later, so we don't really care too much. However, we
     * do want the font width/height guesses to correspond to a
     * large font rather than a small one...
     */

    font_width = 10;
    font_height = 20;
    extra_width = 25;
    extra_height = 28;
    guess_width = extra_width + font_width * conf_get_int(conf, CONF_width);
    guess_height = extra_height + font_height*conf_get_int(conf, CONF_height);
    {
        RECT r;
        get_fullscreen_rect(&r);
        if (guess_width > r.right - r.left)
            guess_width = r.right - r.left;
        if (guess_height > r.bottom - r.top)
            guess_height = r.bottom - r.top;
    }

    {
        int winmode = WS_OVERLAPPEDWINDOW | WS_VSCROLL;
        int exwinmode = 0;
        wchar_t *uappname = dup_mb_to_wc(DEFAULT_CODEPAGE, 0, appname);
        if (!conf_get_bool(conf, CONF_scrollbar))
            winmode &= ~(WS_VSCROLL);
        if (conf_get_int(conf, CONF_resize_action) == RESIZE_DISABLED)
            winmode &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
        if (conf_get_bool(conf, CONF_alwaysontop))
            exwinmode |= WS_EX_TOPMOST;
        if (conf_get_bool(conf, CONF_sunken_edge))
            exwinmode |= WS_EX_CLIENTEDGE;
        hwnd = CreateWindowExW(exwinmode, uappname, uappname,
                               winmode, CW_USEDEFAULT, CW_USEDEFAULT,
                               guess_width, guess_height,
                               NULL, NULL, inst, NULL);
        sfree(uappname);
    }

    /*
     * Initialise the fonts, simultaneously correcting the guesses
     * for font_{width,height}.
     */
    init_fonts(0,0);

    /*
     * Initialise the terminal. (We have to do this _after_
     * creating the window, since the terminal is the first thing
     * which will call schedule_timer(), which will in turn call
     * timer_change_notify() which will expect hwnd to exist.)
     */
    wintw->vt = &windows_termwin_vt;
    term = term_init(conf, &ucsdata, wintw);
    setup_clipboards(term, conf);
    logctx = log_init(win_gui_logpolicy, conf);
    term_provide_logctx(term, logctx);
    term_size(term, conf_get_int(conf, CONF_height),
              conf_get_int(conf, CONF_width),
              conf_get_int(conf, CONF_savelines));

    /*
     * Correct the guesses for extra_{width,height}.
     */
    {
        RECT cr, wr;
        GetWindowRect(hwnd, &wr);
        GetClientRect(hwnd, &cr);
        offset_width = offset_height = conf_get_int(conf, CONF_window_border);
        extra_width = wr.right - wr.left - cr.right + cr.left + offset_width*2;
        extra_height = wr.bottom - wr.top - cr.bottom + cr.top +offset_height*2;
    }

    /*
     * Resize the window, now we know what size we _really_ want it
     * to be.
     */
    guess_width = extra_width + font_width * term->cols;
    guess_height = extra_height + font_height * term->rows;
    SetWindowPos(hwnd, NULL, 0, 0, guess_width, guess_height,
                 SWP_NOMOVE | SWP_NOREDRAW | SWP_NOZORDER);

    /*
     * Set up a caret bitmap, with no content.
     */
    {
        char *bits;
        int size = (font_width + 15) / 16 * 2 * font_height;
        bits = snewn(size, char);
        memset(bits, 0, size);
        caretbm = CreateBitmap(font_width, font_height, 1, 1, bits);
        sfree(bits);
    }
    CreateCaret(hwnd, caretbm, font_width, font_height);

    /*
     * Initialise the scroll bar.
     */
    {
        SCROLLINFO si;

        si.cbSize = sizeof(si);
        si.fMask = SIF_ALL | SIF_DISABLENOSCROLL;
        si.nMin = 0;
        si.nMax = term->rows - 1;
        si.nPage = term->rows;
        si.nPos = 0;
        SetScrollInfo(hwnd, SB_VERT, &si, false);
    }

    /*
     * Prepare the mouse handler.
     */
    lastact = MA_NOTHING;
    lastbtn = MBT_NOTHING;
    dbltime = GetDoubleClickTime();

    /*
     * Set up the session-control options on the system menu.
     */
    {
        HMENU m;
        int j;
        char *str;

        popup_menus[SYSMENU].menu = GetSystemMenu(hwnd, false);
        popup_menus[CTXMENU].menu = CreatePopupMenu();
        AppendMenu(popup_menus[CTXMENU].menu, MF_ENABLED, IDM_COPY, "&Copy");
        AppendMenu(popup_menus[CTXMENU].menu, MF_ENABLED, IDM_PASTE, "&Paste");

        savedsess_menu = CreateMenu();
        get_sesslist(&sesslist, true);
        update_savedsess_menu();

        for (j = 0; j < lenof(popup_menus); j++) {
            m = popup_menus[j].menu;

            AppendMenu(m, MF_SEPARATOR, 0, 0);
            AppendMenu(m, MF_ENABLED, IDM_SHOWLOG, "&Event Log");
            AppendMenu(m, MF_SEPARATOR, 0, 0);
            AppendMenu(m, MF_ENABLED, IDM_NEWSESS, "Ne&w Session...");
            AppendMenu(m, MF_ENABLED, IDM_DUPSESS, "&Duplicate Session");
            AppendMenu(m, MF_POPUP | MF_ENABLED, (UINT_PTR) savedsess_menu,
                       "Sa&ved Sessions");
            AppendMenu(m, MF_ENABLED, IDM_RECONF, "Chan&ge Settings...");
            AppendMenu(m, MF_SEPARATOR, 0, 0);
            AppendMenu(m, MF_ENABLED, IDM_COPYALL, "C&opy All to Clipboard");
            AppendMenu(m, MF_ENABLED, IDM_CLRSB, "C&lear Scrollback");
            AppendMenu(m, MF_ENABLED, IDM_RESET, "Rese&t Terminal");
            AppendMenu(m, MF_SEPARATOR, 0, 0);
            AppendMenu(m, (conf_get_int(conf, CONF_resize_action)
                           == RESIZE_DISABLED) ? MF_GRAYED : MF_ENABLED,
                       IDM_FULLSCREEN, "&Full Screen");
            AppendMenu(m, MF_SEPARATOR, 0, 0);
            if (has_help())
                AppendMenu(m, MF_ENABLED, IDM_HELP, "&Help");
            str = dupprintf("&About %s", appname);
            AppendMenu(m, MF_ENABLED, IDM_ABOUT, str);
            sfree(str);
        }
    }

    if (restricted_acl) {
        lp_eventlog(win_gui_logpolicy, "Running with restricted process ACL");
    }

    winselgui_set_hwnd(hwnd);
    start_backend();

    /*
     * Set up the initial input locale.
     */
    set_input_locale(GetKeyboardLayout(0));

    /*
     * Finally show the window!
     */
    ShowWindow(hwnd, show);
    SetForegroundWindow(hwnd);

    /*
     * Set the palette up.
     */
    pal = NULL;
    logpal = NULL;
    init_palette();

    term_set_focus(term, GetForegroundWindow() == hwnd);
    UpdateWindow(hwnd);

    while (1) {
        HANDLE *handles;
        int nhandles, n;
        DWORD timeout;

        if (toplevel_callback_pending() ||
            PeekMessage(&msg, NULL, 0, 0, PM_NOREMOVE)) {
            /*
             * If we have anything we'd like to do immediately, set
             * the timeout for MsgWaitForMultipleObjects to zero so
             * that we'll only do a quick check of our handles and
             * then get on with whatever that was.
             *
             * One such option is a pending toplevel callback. The
             * other is a non-empty Windows message queue, which you'd
             * think we could leave to MsgWaitForMultipleObjects to
             * check for us along with all the handles, but in fact we
             * can't because once PeekMessage in one iteration of this
             * loop has removed a message from the queue, the whole
             * queue is considered uninteresting by the next
             * invocation of MWFMO. So we check ourselves whether the
             * message queue is non-empty, and if so, set this timeout
             * to zero to ensure MWFMO doesn't block.
             */
            timeout = 0;
        } else {
            timeout = INFINITE;
            /* The messages seem unreliable; especially if we're being tricky */
            term_set_focus(term, GetForegroundWindow() == hwnd);
        }

        handles = handle_get_events(&nhandles);

        n = MsgWaitForMultipleObjects(nhandles, handles, false,
                                      timeout, QS_ALLINPUT);

        if ((unsigned)(n - WAIT_OBJECT_0) < (unsigned)nhandles) {
            handle_got_event(handles[n - WAIT_OBJECT_0]);
            sfree(handles);
        } else
            sfree(handles);

        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT)
                goto finished;         /* two-level break */

            if (!(IsWindow(logbox) && IsDialogMessage(logbox, &msg)))
                DispatchMessageW(&msg);

            /*
             * WM_NETEVENT messages seem to jump ahead of others in
             * the message queue. I'm not sure why; the docs for
             * PeekMessage mention that messages are prioritised in
             * some way, but I'm unclear on which priorities go where.
             *
             * Anyway, in practice I observe that WM_NETEVENT seems to
             * jump to the head of the queue, which means that if we
             * were to only process one message every time round this
             * loop, we'd get nothing but NETEVENTs if the server
             * flooded us with data, and stop responding to any other
             * kind of window message. So instead, we keep on round
             * this loop until we've consumed at least one message
             * that _isn't_ a NETEVENT, or run out of messages
             * completely (whichever comes first). And we don't go to
             * run_toplevel_callbacks (which is where the netevents
             * are actually processed, causing fresh NETEVENT messages
             * to appear) until we've done this.
             */
            if (msg.message != WM_NETEVENT)
                break;
        }

        run_toplevel_callbacks();
    }

    finished:
    cleanup_exit(msg.wParam);          /* this doesn't return... */
    return msg.wParam;                 /* ... but optimiser doesn't know */
}

static void setup_clipboards(Terminal *term, Conf *conf)
{
    assert(term->mouse_select_clipboards[0] == CLIP_LOCAL);

    term->n_mouse_select_clipboards = 1;

    if (conf_get_bool(conf, CONF_mouseautocopy)) {
        term->mouse_select_clipboards[
            term->n_mouse_select_clipboards++] = CLIP_SYSTEM;
    }

    switch (conf_get_int(conf, CONF_mousepaste)) {
      case CLIPUI_IMPLICIT:
        term->mouse_paste_clipboard = CLIP_LOCAL;
        break;
      case CLIPUI_EXPLICIT:
        term->mouse_paste_clipboard = CLIP_SYSTEM;
        break;
      default:
        term->mouse_paste_clipboard = CLIP_NULL;
        break;
    }
}

/*
 * Clean up and exit.
 */
void cleanup_exit(int code)
{
    /*
     * Clean up.
     */
    deinit_fonts();
    sfree(logpal);
    if (pal)
        DeleteObject(pal);
    sk_cleanup();

    if (conf_get_int(conf, CONF_protocol) == PROT_SSH) {
        random_save_seed();
    }
    shutdown_help();

    /* Clean up COM. */
    CoUninitialize();

    exit(code);
}

/*
 * Refresh the saved-session submenu from `sesslist'.
 */
static void update_savedsess_menu(void)
{
    int i;
    while (DeleteMenu(savedsess_menu, 0, MF_BYPOSITION)) ;
    /* skip sesslist.sessions[0] == Default Settings */
    for (i = 1;
         i < ((sesslist.nsessions <= MENU_SAVED_MAX+1) ? sesslist.nsessions
                                                       : MENU_SAVED_MAX+1);
         i++)
        AppendMenu(savedsess_menu, MF_ENABLED,
                   IDM_SAVED_MIN + (i-1)*MENU_SAVED_STEP,
                   sesslist.sessions[i]);
    if (sesslist.nsessions <= 1)
        AppendMenu(savedsess_menu, MF_GRAYED, IDM_SAVED_MIN, "(No sessions)");
}

/*
 * Update the Special Commands submenu.
 */
static void win_seat_update_specials_menu(Seat *seat)
{
    HMENU new_menu;
    int i, j;

    if (backend)
        specials = backend_get_specials(backend);
    else
        specials = NULL;

    if (specials) {
        /* We can't use Windows to provide a stack for submenus, so
         * here's a lame "stack" that will do for now. */
        HMENU saved_menu = NULL;
        int nesting = 1;
        new_menu = CreatePopupMenu();
        for (i = 0; nesting > 0; i++) {
            assert(IDM_SPECIAL_MIN + 0x10 * i < IDM_SPECIAL_MAX);
            switch (specials[i].code) {
              case SS_SEP:
                AppendMenu(new_menu, MF_SEPARATOR, 0, 0);
                break;
              case SS_SUBMENU:
                assert(nesting < 2);
                nesting++;
                saved_menu = new_menu; /* XXX lame stacking */
                new_menu = CreatePopupMenu();
                AppendMenu(saved_menu, MF_POPUP | MF_ENABLED,
                           (UINT_PTR) new_menu, specials[i].name);
                break;
              case SS_EXITMENU:
                nesting--;
                if (nesting) {
                    new_menu = saved_menu; /* XXX lame stacking */
                    saved_menu = NULL;
                }
                break;
              default:
                AppendMenu(new_menu, MF_ENABLED, IDM_SPECIAL_MIN + 0x10 * i,
                           specials[i].name);
                break;
            }
        }
        /* Squirrel the highest special. */
        n_specials = i - 1;
    } else {
        new_menu = NULL;
        n_specials = 0;
    }

    for (j = 0; j < lenof(popup_menus); j++) {
        if (specials_menu) {
            /* XXX does this free up all submenus? */
            DeleteMenu(popup_menus[j].menu, (UINT_PTR)specials_menu,
                       MF_BYCOMMAND);
            DeleteMenu(popup_menus[j].menu, IDM_SPECIALSEP, MF_BYCOMMAND);
        }
        if (new_menu) {
            InsertMenu(popup_menus[j].menu, IDM_SHOWLOG,
                       MF_BYCOMMAND | MF_POPUP | MF_ENABLED,
                       (UINT_PTR) new_menu, "S&pecial Command");
            InsertMenu(popup_menus[j].menu, IDM_SHOWLOG,
                       MF_BYCOMMAND | MF_SEPARATOR, IDM_SPECIALSEP, 0);
        }
    }
    specials_menu = new_menu;
}

static void update_mouse_pointer(void)
{
    LPTSTR curstype = NULL;
    bool force_visible = false;
    static bool forced_visible = false;
    switch (busy_status) {
      case BUSY_NOT:
        if (send_raw_mouse)
            curstype = IDC_ARROW;
        else
            curstype = IDC_IBEAM;
        break;
      case BUSY_WAITING:
        curstype = IDC_APPSTARTING; /* this may be an abuse */
        force_visible = true;
        break;
      case BUSY_CPU:
        curstype = IDC_WAIT;
        force_visible = true;
        break;
      default:
        unreachable("Bad busy_status");
    }
    {
        HCURSOR cursor = LoadCursor(NULL, curstype);
        SetClassLongPtr(hwnd, GCLP_HCURSOR, (LONG_PTR)cursor);
        SetCursor(cursor); /* force redraw of cursor at current posn */
    }
    if (force_visible != forced_visible) {
        /* We want some cursor shapes to be visible always.
         * Along with show_mouseptr(), this manages the ShowCursor()
         * counter such that if we switch back to a non-force_visible
         * cursor, the previous visibility state is restored. */
        ShowCursor(force_visible);
        forced_visible = force_visible;
    }
}

static void win_seat_set_busy_status(Seat *seat, BusyStatus status)
{
    busy_status = status;
    update_mouse_pointer();
}

/*
 * set or clear the "raw mouse message" mode
 */
static void wintw_set_raw_mouse_mode(TermWin *tw, bool activate)
{
    activate = activate && !conf_get_bool(conf, CONF_no_mouse_rep);
    send_raw_mouse = activate;
    update_mouse_pointer();
}

/*
 * Print a message box and close the connection.
 */
static void win_seat_connection_fatal(Seat *seat, const char *msg)
{
    char *title = dupprintf("%s Fatal Error", appname);
    MessageBox(hwnd, msg, title, MB_ICONERROR | MB_OK);
    sfree(title);

    if (conf_get_int(conf, CONF_close_on_exit) == FORCE_ON)
        PostQuitMessage(1);
    else {
        queue_toplevel_callback(close_session, NULL);
    }
}

/*
 * Report an error at the command-line parsing stage.
 */
void cmdline_error(const char *fmt, ...)
{
    va_list ap;
    char *message, *title;

    va_start(ap, fmt);
    message = dupvprintf(fmt, ap);
    va_end(ap);
    title = dupprintf("%s Command Line Error", appname);
    MessageBox(hwnd, message, title, MB_ICONERROR | MB_OK);
    sfree(message);
    sfree(title);
    exit(1);
}

/*
 * Actually do the job requested by a WM_NETEVENT
 */
static void wm_netevent_callback(void *vctx)
{
    struct wm_netevent_params *params = (struct wm_netevent_params *)vctx;
    select_result(params->wParam, params->lParam);
    sfree(vctx);
}

/*
 * Copy the colour palette from the configuration data into defpal.
 * This is non-trivial because the colour indices are different.
 */
static void conftopalette(void)
{
    int i;
    static const int ww[] = {
        256, 257, 258, 259, 260, 261,
        0, 8, 1, 9, 2, 10, 3, 11,
        4, 12, 5, 13, 6, 14, 7, 15
    };

    for (i = 0; i < 22; i++) {
        int w = ww[i];
        defpal[w].rgbtRed = conf_get_int_int(conf, CONF_colours, i*3+0);
        defpal[w].rgbtGreen = conf_get_int_int(conf, CONF_colours, i*3+1);
        defpal[w].rgbtBlue = conf_get_int_int(conf, CONF_colours, i*3+2);
    }
    for (i = 0; i < NEXTCOLOURS; i++) {
        if (i < 216) {
            int r = i / 36, g = (i / 6) % 6, b = i % 6;
            defpal[i+16].rgbtRed = r ? r * 40 + 55 : 0;
            defpal[i+16].rgbtGreen = g ? g * 40 + 55 : 0;
            defpal[i+16].rgbtBlue = b ? b * 40 + 55 : 0;
        } else {
            int shade = i - 216;
            shade = shade * 10 + 8;
            defpal[i+16].rgbtRed = defpal[i+16].rgbtGreen =
                defpal[i+16].rgbtBlue = shade;
        }
    }

    /* Override with system colours if appropriate */
    if (conf_get_bool(conf, CONF_system_colour))
        systopalette();
}

/*
 * Override bit of defpal with colours from the system.
 * (NB that this takes a copy the system colours at the time this is called,
 * so subsequent colour scheme changes don't take effect. To fix that we'd
 * probably want to be using GetSysColorBrush() and the like.)
 */
static void systopalette(void)
{
    int i;
    static const struct { int nIndex; int norm; int bold; } or[] =
    {
        { COLOR_WINDOWTEXT,     256, 257 }, /* Default Foreground */
        { COLOR_WINDOW,         258, 259 }, /* Default Background */
        { COLOR_HIGHLIGHTTEXT,  260, 260 }, /* Cursor Text */
        { COLOR_HIGHLIGHT,      261, 261 }, /* Cursor Colour */
    };

    for (i = 0; i < (sizeof(or)/sizeof(or[0])); i++) {
        COLORREF colour = GetSysColor(or[i].nIndex);
        defpal[or[i].norm].rgbtRed =
           defpal[or[i].bold].rgbtRed = GetRValue(colour);
        defpal[or[i].norm].rgbtGreen =
           defpal[or[i].bold].rgbtGreen = GetGValue(colour);
        defpal[or[i].norm].rgbtBlue =
           defpal[or[i].bold].rgbtBlue = GetBValue(colour);
    }
}

static void internal_set_colour(int i, int r, int g, int b)
{
    assert(i >= 0);
    assert(i < NALLCOLOURS);
    if (pal)
        colours[i] = PALETTERGB(r, g, b);
    else
        colours[i] = RGB(r, g, b);
    colours_rgb[i].r = r;
    colours_rgb[i].g = g;
    colours_rgb[i].b = b;
}

/*
 * Set up the colour palette.
 */
static void init_palette(void)
{
    int i;
    HDC hdc = GetDC(hwnd);
    if (hdc) {
        if (conf_get_bool(conf, CONF_try_palette) &&
            GetDeviceCaps(hdc, RASTERCAPS) & RC_PALETTE) {
            /*
             * This is a genuine case where we must use smalloc
             * because the snew macros can't cope.
             */
            logpal = smalloc(sizeof(*logpal)
                             - sizeof(logpal->palPalEntry)
                             + NALLCOLOURS * sizeof(PALETTEENTRY));
            logpal->palVersion = 0x300;
            logpal->palNumEntries = NALLCOLOURS;
            for (i = 0; i < NALLCOLOURS; i++) {
                logpal->palPalEntry[i].peRed = defpal[i].rgbtRed;
                logpal->palPalEntry[i].peGreen = defpal[i].rgbtGreen;
                logpal->palPalEntry[i].peBlue = defpal[i].rgbtBlue;
                logpal->palPalEntry[i].peFlags = PC_NOCOLLAPSE;
            }
            pal = CreatePalette(logpal);
            if (pal) {
                SelectPalette(hdc, pal, false);
                RealizePalette(hdc);
                SelectPalette(hdc, GetStockObject(DEFAULT_PALETTE), false);
            }
        }
        ReleaseDC(hwnd, hdc);
    }
    for (i = 0; i < NALLCOLOURS; i++)
        internal_set_colour(i, defpal[i].rgbtRed,
                            defpal[i].rgbtGreen, defpal[i].rgbtBlue);
}

/*
 * This is a wrapper to ExtTextOut() to force Windows to display
 * the precise glyphs we give it. Otherwise it would do its own
 * bidi and Arabic shaping, and we would end up uncertain which
 * characters it had put where.
 */
static void exact_textout(HDC hdc, int x, int y, CONST RECT *lprc,
                          unsigned short *lpString, UINT cbCount,
                          CONST INT *lpDx, bool opaque)
{
#ifdef __LCC__
    /*
     * The LCC include files apparently don't supply the
     * GCP_RESULTSW type, but we can make do with GCP_RESULTS
     * proper: the differences aren't important to us (the only
     * variable-width string parameter is one we don't use anyway).
     */
    GCP_RESULTS gcpr;
#else
    GCP_RESULTSW gcpr;
#endif
    char *buffer = snewn(cbCount*2+2, char);
    char *classbuffer = snewn(cbCount, char);
    memset(&gcpr, 0, sizeof(gcpr));
    memset(buffer, 0, cbCount*2+2);
    memset(classbuffer, GCPCLASS_NEUTRAL, cbCount);

    gcpr.lStructSize = sizeof(gcpr);
    gcpr.lpGlyphs = (void *)buffer;
    gcpr.lpClass = (void *)classbuffer;
    gcpr.nGlyphs = cbCount;
    GetCharacterPlacementW(hdc, lpString, cbCount, 0, &gcpr,
                           FLI_MASK | GCP_CLASSIN | GCP_DIACRITIC);

    ExtTextOut(hdc, x, y,
               ETO_GLYPH_INDEX | ETO_CLIPPED | (opaque ? ETO_OPAQUE : 0),
               lprc, buffer, cbCount, lpDx);
}

/*
 * The exact_textout() wrapper, unfortunately, destroys the useful
 * Windows `font linking' behaviour: automatic handling of Unicode
 * code points not supported in this font by falling back to a font
 * which does contain them. Therefore, we adopt a multi-layered
 * approach: for any potentially-bidi text, we use exact_textout(),
 * and for everything else we use a simple ExtTextOut as we did
 * before exact_textout() was introduced.
 */
static void general_textout(HDC hdc, int x, int y, CONST RECT *lprc,
                            unsigned short *lpString, UINT cbCount,
                            CONST INT *lpDx, bool opaque)
{
    int i, j, xp, xn;
    int bkmode = 0;
    bool got_bkmode = false;

    xp = xn = x;

    for (i = 0; i < (int)cbCount ;) {
        bool rtl = is_rtl(lpString[i]);

        xn += lpDx[i];

        for (j = i+1; j < (int)cbCount; j++) {
            if (rtl != is_rtl(lpString[j]))
                break;
            xn += lpDx[j];
        }

        /*
         * Now [i,j) indicates a maximal substring of lpString
         * which should be displayed using the same textout
         * function.
         */
        if (rtl) {
            exact_textout(hdc, xp, y, lprc, lpString+i, j-i,
                          font_varpitch ? NULL : lpDx+i, opaque);
        } else {
            ExtTextOutW(hdc, xp, y, ETO_CLIPPED | (opaque ? ETO_OPAQUE : 0),
                        lprc, lpString+i, j-i,
                        font_varpitch ? NULL : lpDx+i);
        }

        i = j;
        xp = xn;

        bkmode = GetBkMode(hdc);
        got_bkmode = true;
        SetBkMode(hdc, TRANSPARENT);
        opaque = false;
    }

    if (got_bkmode)
        SetBkMode(hdc, bkmode);
}

static int get_font_width(HDC hdc, const TEXTMETRIC *tm)
{
    int ret;
    /* Note that the TMPF_FIXED_PITCH bit is defined upside down :-( */
    if (!(tm->tmPitchAndFamily & TMPF_FIXED_PITCH)) {
        ret = tm->tmAveCharWidth;
    } else {
#define FIRST '0'
#define LAST '9'
        ABCFLOAT widths[LAST-FIRST + 1];
        int j;

        font_varpitch = true;
        font_dualwidth = true;
        if (GetCharABCWidthsFloat(hdc, FIRST, LAST, widths)) {
            ret = 0;
            for (j = 0; j < lenof(widths); j++) {
                int width = (int)(0.5 + widths[j].abcfA +
                                  widths[j].abcfB + widths[j].abcfC);
                if (ret < width)
                    ret = width;
            }
        } else {
            ret = tm->tmMaxCharWidth;
        }
#undef FIRST
#undef LAST
    }
    return ret;
}

/*
 * Initialise all the fonts we will need initially. There may be as many as
 * three or as few as one.  The other (potentially) twenty-one fonts are done
 * if/when they are needed.
 *
 * We also:
 *
 * - check the font width and height, correcting our guesses if
 *   necessary.
 *
 * - verify that the bold font is the same width as the ordinary
 *   one, and engage shadow bolding if not.
 *
 * - verify that the underlined font is the same width as the
 *   ordinary one (manual underlining by means of line drawing can
 *   be done in a pinch).
 *
 * - find a trust sigil icon that will look OK with the chosen font.
 */
static void init_fonts(int pick_width, int pick_height)
{
    TEXTMETRIC tm;
    CPINFO cpinfo;
    FontSpec *font;
    int fontsize[3];
    int i;
    int quality;
    HDC hdc;
    int fw_dontcare, fw_bold;

    for (i = 0; i < FONT_MAXNO; i++)
        fonts[i] = NULL;

    bold_font_mode = conf_get_int(conf, CONF_bold_style) & 1 ?
        BOLD_FONT : BOLD_NONE;
    bold_colours = conf_get_int(conf, CONF_bold_style) & 2 ? true : false;
    und_mode = UND_FONT;

    font = conf_get_fontspec(conf, CONF_font);
    if (font->isbold) {
        fw_dontcare = FW_BOLD;
        fw_bold = FW_HEAVY;
    } else {
        fw_dontcare = FW_DONTCARE;
        fw_bold = FW_BOLD;
    }

    hdc = GetDC(hwnd);

    if (pick_height)
        font_height = pick_height;
    else {
        font_height = font->height;
        if (font_height > 0) {
            font_height =
                -MulDiv(font_height, GetDeviceCaps(hdc, LOGPIXELSY), 72);
        }
    }
    font_width = pick_width;

    quality = conf_get_int(conf, CONF_font_quality);
#define f(i,c,w,u) \
    fonts[i] = CreateFont (font_height, font_width, 0, 0, w, false, u, false, \
                           c, OUT_DEFAULT_PRECIS, \
                           CLIP_DEFAULT_PRECIS, FONT_QUALITY(quality), \
                           FIXED_PITCH | FF_DONTCARE, font->name)

    f(FONT_NORMAL, font->charset, fw_dontcare, false);

    SelectObject(hdc, fonts[FONT_NORMAL]);
    GetTextMetrics(hdc, &tm);

    GetObject(fonts[FONT_NORMAL], sizeof(LOGFONT), &lfont);

    /* Note that the TMPF_FIXED_PITCH bit is defined upside down :-( */
    if (!(tm.tmPitchAndFamily & TMPF_FIXED_PITCH)) {
        font_varpitch = false;
        font_dualwidth = (tm.tmAveCharWidth != tm.tmMaxCharWidth);
    } else {
        font_varpitch = true;
        font_dualwidth = true;
    }
    if (pick_width == 0 || pick_height == 0) {
        font_height = tm.tmHeight;
        font_width = get_font_width(hdc, &tm);
    }

#ifdef RDB_DEBUG_PATCH
    debug("Primary font H=%d, AW=%d, MW=%d\n",
          tm.tmHeight, tm.tmAveCharWidth, tm.tmMaxCharWidth);
#endif

    {
        CHARSETINFO info;
        DWORD cset = tm.tmCharSet;
        memset(&info, 0xFF, sizeof(info));

        /* !!! Yes the next line is right */
        if (cset == OEM_CHARSET)
            ucsdata.font_codepage = GetOEMCP();
        else
            if (TranslateCharsetInfo ((DWORD *)(ULONG_PTR)cset,
                                      &info, TCI_SRCCHARSET))
                ucsdata.font_codepage = info.ciACP;
        else
            ucsdata.font_codepage = -1;

        GetCPInfo(ucsdata.font_codepage, &cpinfo);
        ucsdata.dbcs_screenfont = (cpinfo.MaxCharSize > 1);
    }

    f(FONT_UNDERLINE, font->charset, fw_dontcare, true);

    /*
     * Some fonts, e.g. 9-pt Courier, draw their underlines
     * outside their character cell. We successfully prevent
     * screen corruption by clipping the text output, but then
     * we lose the underline completely. Here we try to work
     * out whether this is such a font, and if it is, we set a
     * flag that causes underlines to be drawn by hand.
     *
     * Having tried other more sophisticated approaches (such
     * as examining the TEXTMETRIC structure or requesting the
     * height of a string), I think we'll do this the brute
     * force way: we create a small bitmap, draw an underlined
     * space on it, and test to see whether any pixels are
     * foreground-coloured. (Since we expect the underline to
     * go all the way across the character cell, we only search
     * down a single column of the bitmap, half way across.)
     */
    {
        HDC und_dc;
        HBITMAP und_bm, und_oldbm;
        int i;
        bool gotit;
        COLORREF c;

        und_dc = CreateCompatibleDC(hdc);
        und_bm = CreateCompatibleBitmap(hdc, font_width, font_height);
        und_oldbm = SelectObject(und_dc, und_bm);
        SelectObject(und_dc, fonts[FONT_UNDERLINE]);
        SetTextAlign(und_dc, TA_TOP | TA_LEFT | TA_NOUPDATECP);
        SetTextColor(und_dc, RGB(255, 255, 255));
        SetBkColor(und_dc, RGB(0, 0, 0));
        SetBkMode(und_dc, OPAQUE);
        ExtTextOut(und_dc, 0, 0, ETO_OPAQUE, NULL, " ", 1, NULL);
        gotit = false;
        for (i = 0; i < font_height; i++) {
            c = GetPixel(und_dc, font_width / 2, i);
            if (c != RGB(0, 0, 0))
                gotit = true;
        }
        SelectObject(und_dc, und_oldbm);
        DeleteObject(und_bm);
        DeleteDC(und_dc);
        if (!gotit) {
            und_mode = UND_LINE;
            DeleteObject(fonts[FONT_UNDERLINE]);
            fonts[FONT_UNDERLINE] = 0;
        }
    }

    if (bold_font_mode == BOLD_FONT) {
        f(FONT_BOLD, font->charset, fw_bold, false);
    }
#undef f

    descent = tm.tmAscent + 1;
    if (descent >= font_height)
        descent = font_height - 1;

    for (i = 0; i < 3; i++) {
        if (fonts[i]) {
            if (SelectObject(hdc, fonts[i]) && GetTextMetrics(hdc, &tm))
                fontsize[i] = get_font_width(hdc, &tm) + 256 * tm.tmHeight;
            else
                fontsize[i] = -i;
        } else
            fontsize[i] = -i;
    }

    ReleaseDC(hwnd, hdc);

    if (trust_icon != INVALID_HANDLE_VALUE) {
        DestroyIcon(trust_icon);
    }
    trust_icon = LoadImage(hinst, MAKEINTRESOURCE(IDI_MAINICON),
                           IMAGE_ICON, font_width*2, font_height,
                           LR_DEFAULTCOLOR);

    if (fontsize[FONT_UNDERLINE] != fontsize[FONT_NORMAL]) {
        und_mode = UND_LINE;
        DeleteObject(fonts[FONT_UNDERLINE]);
        fonts[FONT_UNDERLINE] = 0;
    }

    if (bold_font_mode == BOLD_FONT &&
        fontsize[FONT_BOLD] != fontsize[FONT_NORMAL]) {
        bold_font_mode = BOLD_SHADOW;
        DeleteObject(fonts[FONT_BOLD]);
        fonts[FONT_BOLD] = 0;
    }
    fontflag[0] = true;
    fontflag[1] = true;
    fontflag[2] = true;

    init_ucs(conf, &ucsdata);
}

static void another_font(int fontno)
{
    int basefont;
    int fw_dontcare, fw_bold, quality;
    int c, w, x;
    bool u;
    char *s;
    FontSpec *font;

    if (fontno < 0 || fontno >= FONT_MAXNO || fontflag[fontno])
        return;

    basefont = (fontno & ~(FONT_BOLDUND));
    if (basefont != fontno && !fontflag[basefont])
        another_font(basefont);

    font = conf_get_fontspec(conf, CONF_font);

    if (font->isbold) {
        fw_dontcare = FW_BOLD;
        fw_bold = FW_HEAVY;
    } else {
        fw_dontcare = FW_DONTCARE;
        fw_bold = FW_BOLD;
    }

    c = font->charset;
    w = fw_dontcare;
    u = false;
    s = font->name;
    x = font_width;

    if (fontno & FONT_WIDE)
        x *= 2;
    if (fontno & FONT_NARROW)
        x = (x+1)/2;
    if (fontno & FONT_OEM)
        c = OEM_CHARSET;
    if (fontno & FONT_BOLD)
        w = fw_bold;
    if (fontno & FONT_UNDERLINE)
        u = true;

    quality = conf_get_int(conf, CONF_font_quality);

    fonts[fontno] =
        CreateFont(font_height * (1 + !!(fontno & FONT_HIGH)), x, 0, 0, w,
                   false, u, false, c, OUT_DEFAULT_PRECIS,
                   CLIP_DEFAULT_PRECIS, FONT_QUALITY(quality),
                   DEFAULT_PITCH | FF_DONTCARE, s);

    fontflag[fontno] = true;
}

static void deinit_fonts(void)
{
    int i;
    for (i = 0; i < FONT_MAXNO; i++) {
        if (fonts[i])
            DeleteObject(fonts[i]);
        fonts[i] = 0;
        fontflag[i] = false;
    }

    if (trust_icon != INVALID_HANDLE_VALUE) {
        DestroyIcon(trust_icon);
    }
    trust_icon = INVALID_HANDLE_VALUE;
}

static void wintw_request_resize(TermWin *tw, int w, int h)
{
    int width, height;

    /* If the window is maximized suppress resizing attempts */
    if (IsZoomed(hwnd)) {
        if (conf_get_int(conf, CONF_resize_action) == RESIZE_TERM)
            return;
    }

    if (conf_get_int(conf, CONF_resize_action) == RESIZE_DISABLED) return;
    if (h == term->rows && w == term->cols) return;

    /* Sanity checks ... */
    {
        static int first_time = 1;
        static RECT ss;

        switch (first_time) {
          case 1:
            /* Get the size of the screen */
            if (get_fullscreen_rect(&ss))
                /* first_time = 0 */ ;
            else {
                first_time = 2;
                break;
            }
          case 0:
            /* Make sure the values are sane */
            width = (ss.right - ss.left - extra_width) / 4;
            height = (ss.bottom - ss.top - extra_height) / 6;

            if (w > width || h > height)
                return;
            if (w < 15)
                w = 15;
            if (h < 1)
                h = 1;
        }
    }

    term_size(term, h, w, conf_get_int(conf, CONF_savelines));

    if (conf_get_int(conf, CONF_resize_action) != RESIZE_FONT &&
        !IsZoomed(hwnd)) {
        width = extra_width + font_width * w;
        height = extra_height + font_height * h;

        SetWindowPos(hwnd, NULL, 0, 0, width, height,
            SWP_NOACTIVATE | SWP_NOCOPYBITS |
            SWP_NOMOVE | SWP_NOZORDER);
    } else
        reset_window(0);

    InvalidateRect(hwnd, NULL, true);
}

static void reset_window(int reinit) {
    /*
     * This function decides how to resize or redraw when the
     * user changes something.
     *
     * This function doesn't like to change the terminal size but if the
     * font size is locked that may be it's only soluion.
     */
    int win_width, win_height, resize_action, window_border;
    RECT cr, wr;

#ifdef RDB_DEBUG_PATCH
    debug("reset_window()\n");
#endif

    /* Current window sizes ... */
    GetWindowRect(hwnd, &wr);
    GetClientRect(hwnd, &cr);

    win_width  = cr.right - cr.left;
    win_height = cr.bottom - cr.top;

    resize_action = conf_get_int(conf, CONF_resize_action);
    window_border = conf_get_int(conf, CONF_window_border);

    if (resize_action == RESIZE_DISABLED)
        reinit = 2;

    /* Are we being forced to reload the fonts ? */
    if (reinit>1) {
#ifdef RDB_DEBUG_PATCH
        debug("reset_window() -- Forced deinit\n");
#endif
        deinit_fonts();
        init_fonts(0,0);
    }

    /* Oh, looks like we're minimised */
    if (win_width == 0 || win_height == 0)
        return;

    /* Is the window out of position ? */
    if ( !reinit &&
            (offset_width != (win_width-font_width*term->cols)/2 ||
             offset_height != (win_height-font_height*term->rows)/2) ){
        offset_width = (win_width-font_width*term->cols)/2;
        offset_height = (win_height-font_height*term->rows)/2;
        InvalidateRect(hwnd, NULL, true);
#ifdef RDB_DEBUG_PATCH
        debug("reset_window() -> Reposition terminal\n");
#endif
    }

    if (IsZoomed(hwnd)) {
        /* We're fullscreen, this means we must not change the size of
         * the window so it's the font size or the terminal itself.
         */

        extra_width = wr.right - wr.left - cr.right + cr.left;
        extra_height = wr.bottom - wr.top - cr.bottom + cr.top;

        if (resize_action != RESIZE_TERM) {
            if (font_width != win_width/term->cols ||
                font_height != win_height/term->rows) {
                deinit_fonts();
                init_fonts(win_width/term->cols, win_height/term->rows);
                offset_width = (win_width-font_width*term->cols)/2;
                offset_height = (win_height-font_height*term->rows)/2;
                InvalidateRect(hwnd, NULL, true);
#ifdef RDB_DEBUG_PATCH
                debug("reset_window() -> Z font resize to (%d, %d)\n",
                      font_width, font_height);
#endif
            }
        } else {
            if (font_width * term->cols != win_width ||
                font_height * term->rows != win_height) {
                /* Our only choice at this point is to change the
                 * size of the terminal; Oh well.
                 */
                term_size(term, win_height/font_height, win_width/font_width,
                          conf_get_int(conf, CONF_savelines));
                offset_width = (win_width-font_width*term->cols)/2;
                offset_height = (win_height-font_height*term->rows)/2;
                InvalidateRect(hwnd, NULL, true);
#ifdef RDB_DEBUG_PATCH
                debug("reset_window() -> Zoomed term_size\n");
#endif
            }
        }
        return;
    }

    /* Hmm, a force re-init means we should ignore the current window
     * so we resize to the default font size.
     */
    if (reinit>0) {
#ifdef RDB_DEBUG_PATCH
        debug("reset_window() -> Forced re-init\n");
#endif

        offset_width = offset_height = window_border;
        extra_width = wr.right - wr.left - cr.right + cr.left + offset_width*2;
        extra_height = wr.bottom - wr.top - cr.bottom + cr.top +offset_height*2;

        if (win_width != font_width*term->cols + offset_width*2 ||
            win_height != font_height*term->rows + offset_height*2) {

            /* If this is too large windows will resize it to the maximum
             * allowed window size, we will then be back in here and resize
             * the font or terminal to fit.
             */
            SetWindowPos(hwnd, NULL, 0, 0,
                         font_width*term->cols + extra_width,
                         font_height*term->rows + extra_height,
                         SWP_NOMOVE | SWP_NOZORDER);
        }

        InvalidateRect(hwnd, NULL, true);
        return;
    }

    /* Okay the user doesn't want us to change the font so we try the
     * window. But that may be too big for the screen which forces us
     * to change the terminal.
     */
    if ((resize_action == RESIZE_TERM && reinit<=0) ||
        (resize_action == RESIZE_EITHER && reinit<0) ||
            reinit>0) {
        offset_width = offset_height = window_border;
        extra_width = wr.right - wr.left - cr.right + cr.left + offset_width*2;
        extra_height = wr.bottom - wr.top - cr.bottom + cr.top +offset_height*2;

        if (win_width != font_width*term->cols + offset_width*2 ||
            win_height != font_height*term->rows + offset_height*2) {

            static RECT ss;
            int width, height;

                get_fullscreen_rect(&ss);

            width = (ss.right - ss.left - extra_width) / font_width;
            height = (ss.bottom - ss.top - extra_height) / font_height;

            /* Grrr too big */
            if ( term->rows > height || term->cols > width ) {
                if (resize_action == RESIZE_EITHER) {
                    /* Make the font the biggest we can */
                    if (term->cols > width)
                        font_width = (ss.right - ss.left - extra_width)
                            / term->cols;
                    if (term->rows > height)
                        font_height = (ss.bottom - ss.top - extra_height)
                            / term->rows;

                    deinit_fonts();
                    init_fonts(font_width, font_height);

                    width = (ss.right - ss.left - extra_width) / font_width;
                    height = (ss.bottom - ss.top - extra_height) / font_height;
                } else {
                    if ( height > term->rows ) height = term->rows;
                    if ( width > term->cols )  width = term->cols;
                    term_size(term, height, width,
                              conf_get_int(conf, CONF_savelines));
#ifdef RDB_DEBUG_PATCH
                    debug("reset_window() -> term resize to (%d,%d)\n",
                          height, width);
#endif
                }
            }

            SetWindowPos(hwnd, NULL, 0, 0,
                         font_width*term->cols + extra_width,
                         font_height*term->rows + extra_height,
                         SWP_NOMOVE | SWP_NOZORDER);

            InvalidateRect(hwnd, NULL, true);
#ifdef RDB_DEBUG_PATCH
            debug("reset_window() -> window resize to (%d,%d)\n",
                  font_width*term->cols + extra_width,
                  font_height*term->rows + extra_height);
#endif
        }
        return;
    }

    /* We're allowed to or must change the font but do we want to ?  */

    if (font_width != (win_width-window_border*2)/term->cols ||
        font_height != (win_height-window_border*2)/term->rows) {

        deinit_fonts();
        init_fonts((win_width-window_border*2)/term->cols,
                   (win_height-window_border*2)/term->rows);
        offset_width = (win_width-font_width*term->cols)/2;
        offset_height = (win_height-font_height*term->rows)/2;

        extra_width = wr.right - wr.left - cr.right + cr.left +offset_width*2;
        extra_height = wr.bottom - wr.top - cr.bottom + cr.top+offset_height*2;

        InvalidateRect(hwnd, NULL, true);
#ifdef RDB_DEBUG_PATCH
        debug("reset_window() -> font resize to (%d,%d)\n",
              font_width, font_height);
#endif
    }
}

static void set_input_locale(HKL kl)
{
    char lbuf[20];

    GetLocaleInfo(LOWORD(kl), LOCALE_IDEFAULTANSICODEPAGE,
                  lbuf, sizeof(lbuf));

    kbd_codepage = atoi(lbuf);
}

static void click(Mouse_Button b, int x, int y,
                  bool shift, bool ctrl, bool alt)
{
    int thistime = GetMessageTime();

    if (send_raw_mouse &&
        !(shift && conf_get_bool(conf, CONF_mouse_override))) {
        lastbtn = MBT_NOTHING;
        term_mouse(term, b, translate_button(b), MA_CLICK,
                   x, y, shift, ctrl, alt);
        return;
    }

    if (lastbtn == b && thistime - lasttime < dbltime) {
        lastact = (lastact == MA_CLICK ? MA_2CLK :
                   lastact == MA_2CLK ? MA_3CLK :
                   lastact == MA_3CLK ? MA_CLICK : MA_NOTHING);
    } else {
        lastbtn = b;
        lastact = MA_CLICK;
    }
    if (lastact != MA_NOTHING)
        term_mouse(term, b, translate_button(b), lastact,
                   x, y, shift, ctrl, alt);
    lasttime = thistime;
}

/*
 * Translate a raw mouse button designation (LEFT, MIDDLE, RIGHT)
 * into a cooked one (SELECT, EXTEND, PASTE).
 */
static Mouse_Button translate_button(Mouse_Button button)
{
    if (button == MBT_LEFT)
        return MBT_SELECT;
    if (button == MBT_MIDDLE)
        return conf_get_int(conf, CONF_mouse_is_xterm) == 1 ?
        MBT_PASTE : MBT_EXTEND;
    if (button == MBT_RIGHT)
        return conf_get_int(conf, CONF_mouse_is_xterm) == 1 ?
        MBT_EXTEND : MBT_PASTE;
    return 0;                          /* shouldn't happen */
}

static void show_mouseptr(bool show)
{
    /* NB that the counter in ShowCursor() is also frobbed by
     * update_mouse_pointer() */
    static bool cursor_visible = true;
    if (!conf_get_bool(conf, CONF_hide_mouseptr))
        show = true;                   /* override if this feature disabled */
    if (cursor_visible && !show)
        ShowCursor(false);
    else if (!cursor_visible && show)
        ShowCursor(true);
    cursor_visible = show;
}

static bool is_alt_pressed(void)
{
    BYTE keystate[256];
    int r = GetKeyboardState(keystate);
    if (!r)
        return false;
    if (keystate[VK_MENU] & 0x80)
        return true;
    if (keystate[VK_RMENU] & 0x80)
        return true;
    return false;
}

static bool resizing;

static void win_seat_notify_remote_exit(Seat *seat)
{
    int exitcode, close_on_exit;

    if (!session_closed &&
        (exitcode = backend_exitcode(backend)) >= 0) {
        close_on_exit = conf_get_int(conf, CONF_close_on_exit);
        /* Abnormal exits will already have set session_closed and taken
         * appropriate action. */
        if (close_on_exit == FORCE_ON ||
            (close_on_exit == AUTO && exitcode != INT_MAX)) {
            PostQuitMessage(0);
        } else {
            queue_toplevel_callback(close_session, NULL);
            session_closed = true;
            /* exitcode == INT_MAX indicates that the connection was closed
             * by a fatal error, so an error box will be coming our way and
             * we should not generate this informational one. */
            if (exitcode != INT_MAX)
                MessageBox(hwnd, "Connection closed by remote host",
                           appname, MB_OK | MB_ICONINFORMATION);
        }
    }
}

void timer_change_notify(unsigned long next)
{
    unsigned long now = GETTICKCOUNT();
    long ticks;
    if (now - next < INT_MAX)
        ticks = 0;
    else
        ticks = next - now;
    KillTimer(hwnd, TIMING_TIMER_ID);
    SetTimer(hwnd, TIMING_TIMER_ID, ticks, NULL);
    timing_next_time = next;
}

static void conf_cache_data(void)
{
    /* Cache some items from conf to speed lookups in very hot code */
    cursor_type = conf_get_int(conf, CONF_cursor_type);
    vtmode = conf_get_int(conf, CONF_vtmode);
}

static const int clips_system[] = { CLIP_SYSTEM };

static HDC make_hdc(void)
{
    HDC hdc;

    if (!hwnd)
        return NULL;

    hdc = GetDC(hwnd);
    if (!hdc)
        return NULL;

    SelectPalette(hdc, pal, false);
    return hdc;
}

static void free_hdc(HDC hdc)
{
    assert(hwnd);
    SelectPalette(hdc, GetStockObject(DEFAULT_PALETTE), false);
    ReleaseDC(hwnd, hdc);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT message,
                                WPARAM wParam, LPARAM lParam)
{
    HDC hdc;
    static bool ignore_clip = false;
    static bool need_backend_resize = false;
    static bool fullscr_on_max = false;
    static bool processed_resize = false;
    static UINT last_mousemove = 0;
    int resize_action;

    switch (message) {
      case WM_TIMER:
        if ((UINT_PTR)wParam == TIMING_TIMER_ID) {
            unsigned long next;

            KillTimer(hwnd, TIMING_TIMER_ID);
            if (run_timers(timing_next_time, &next)) {
                timer_change_notify(next);
            } else {
            }
        }
        return 0;
      case WM_CREATE:
        break;
      case WM_CLOSE:
        {
            char *str;
            show_mouseptr(true);
            str = dupprintf("%s Exit Confirmation", appname);
            if (session_closed || !conf_get_bool(conf, CONF_warn_on_close) ||
                MessageBox(hwnd,
                           "Are you sure you want to close this session?",
                           str, MB_ICONWARNING | MB_OKCANCEL | MB_DEFBUTTON1)
                == IDOK)
                DestroyWindow(hwnd);
            sfree(str);
        }
        return 0;
      case WM_DESTROY:
        show_mouseptr(true);
        PostQuitMessage(0);
        return 0;
      case WM_INITMENUPOPUP:
        if ((HMENU)wParam == savedsess_menu) {
            /* About to pop up Saved Sessions sub-menu.
             * Refresh the session list. */
            get_sesslist(&sesslist, false); /* free */
            get_sesslist(&sesslist, true);
            update_savedsess_menu();
            return 0;
        }
        break;
      case WM_COMMAND:
      case WM_SYSCOMMAND:
        switch (wParam & ~0xF) {       /* low 4 bits reserved to Windows */
          case IDM_SHOWLOG:
            showeventlog(hwnd);
            break;
          case IDM_NEWSESS:
          case IDM_DUPSESS:
          case IDM_SAVEDSESS:
            {
                char b[2048];
                char *cl;
                const char *argprefix;
                bool inherit_handles;
                STARTUPINFO si;
                PROCESS_INFORMATION pi;
                HANDLE filemap = NULL;

                if (restricted_acl)
                    argprefix = "&R";
                else
                    argprefix = "";

                if (wParam == IDM_DUPSESS) {
                    /*
                     * Allocate a file-mapping memory chunk for the
                     * config structure.
                     */
                    SECURITY_ATTRIBUTES sa;
                    strbuf *serbuf;
                    void *p;
                    int size;

                    serbuf = strbuf_new();
                    conf_serialise(BinarySink_UPCAST(serbuf), conf);
                    size = serbuf->len;

                    sa.nLength = sizeof(sa);
                    sa.lpSecurityDescriptor = NULL;
                    sa.bInheritHandle = true;
                    filemap = CreateFileMapping(INVALID_HANDLE_VALUE,
                                                &sa,
                                                PAGE_READWRITE,
                                                0, size, NULL);
                    if (filemap && filemap != INVALID_HANDLE_VALUE) {
                        p = MapViewOfFile(filemap, FILE_MAP_WRITE, 0, 0, size);
                        if (p) {
                            memcpy(p, serbuf->s, size);
                            UnmapViewOfFile(p);
                        }
                    }

                    strbuf_free(serbuf);
                    inherit_handles = true;
                    cl = dupprintf("putty %s&%p:%u", argprefix,
                                   filemap, (unsigned)size);
                } else if (wParam == IDM_SAVEDSESS) {
                    unsigned int sessno = ((lParam - IDM_SAVED_MIN)
                                           / MENU_SAVED_STEP) + 1;
                    if (sessno < (unsigned)sesslist.nsessions) {
                        const char *session = sesslist.sessions[sessno];
                        cl = dupprintf("putty %s@%s", argprefix, session);
                        inherit_handles = false;
                    } else
                        break;
                } else /* IDM_NEWSESS */ {
                    cl = dupprintf("putty%s%s",
                                   *argprefix ? " " : "",
                                   argprefix);
                    inherit_handles = false;
                }

                GetModuleFileName(NULL, b, sizeof(b) - 1);
                si.cb = sizeof(si);
                si.lpReserved = NULL;
                si.lpDesktop = NULL;
                si.lpTitle = NULL;
                si.dwFlags = 0;
                si.cbReserved2 = 0;
                si.lpReserved2 = NULL;
                CreateProcess(b, cl, NULL, NULL, inherit_handles,
                              NORMAL_PRIORITY_CLASS, NULL, NULL, &si, &pi);
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);

                if (filemap)
                    CloseHandle(filemap);
                sfree(cl);
            }
            break;
          case IDM_RESTART:
            if (!backend) {
                lp_eventlog(win_gui_logpolicy,
                            "----- Session restarted -----");
                term_pwron(term, false);
                start_backend();
            }

            break;
          case IDM_RECONF:
            {
                Conf *prev_conf;
                int init_lvl = 1;
                bool reconfig_result;

                if (reconfiguring)
                    break;
                else
                    reconfiguring = true;

                /*
                 * Copy the current window title into the stored
                 * previous configuration, so that doing nothing to
                 * the window title field in the config box doesn't
                 * reset the title to its startup state.
                 */
                conf_set_str(conf, CONF_wintitle, window_name);

                prev_conf = conf_copy(conf);

                reconfig_result =
                    do_reconfig(hwnd, backend ? backend_cfg_info(backend) : 0);
                reconfiguring = false;
                if (!reconfig_result) {
                    conf_free(prev_conf);
                    break;
                }

                conf_cache_data();

                resize_action = conf_get_int(conf, CONF_resize_action);
                {
                    /* Disable full-screen if resizing forbidden */
                    int i;
                    for (i = 0; i < lenof(popup_menus); i++)
                        EnableMenuItem(popup_menus[i].menu, IDM_FULLSCREEN,
                                       MF_BYCOMMAND |
                                       (resize_action == RESIZE_DISABLED
                                        ? MF_GRAYED : MF_ENABLED));
                    /* Gracefully unzoom if necessary */
                    if (IsZoomed(hwnd) && (resize_action == RESIZE_DISABLED))
                        ShowWindow(hwnd, SW_RESTORE);
                }

                /* Pass new config data to the logging module */
                log_reconfig(logctx, conf);

                sfree(logpal);
                /*
                 * Flush the line discipline's edit buffer in the
                 * case where local editing has just been disabled.
                 */
                if (ldisc) {
                    ldisc_configure(ldisc, conf);
                    ldisc_echoedit_update(ldisc);
                }
                if (pal)
                    DeleteObject(pal);
                logpal = NULL;
                pal = NULL;
                conftopalette();
                init_palette();

                /* Pass new config data to the terminal */
                term_reconfig(term, conf);
                setup_clipboards(term, conf);

                /* Pass new config data to the back end */
                if (backend)
                    backend_reconfig(backend, conf);

                /* Screen size changed ? */
                if (conf_get_int(conf, CONF_height) !=
                    conf_get_int(prev_conf, CONF_height) ||
                    conf_get_int(conf, CONF_width) !=
                    conf_get_int(prev_conf, CONF_width) ||
                    conf_get_int(conf, CONF_savelines) !=
                    conf_get_int(prev_conf, CONF_savelines) ||
                    resize_action == RESIZE_FONT ||
                    (resize_action == RESIZE_EITHER && IsZoomed(hwnd)) ||
                    resize_action == RESIZE_DISABLED)
                    term_size(term, conf_get_int(conf, CONF_height),
                              conf_get_int(conf, CONF_width),
                              conf_get_int(conf, CONF_savelines));

                /* Enable or disable the scroll bar, etc */
                {
                    LONG nflg, flag = GetWindowLongPtr(hwnd, GWL_STYLE);
                    LONG nexflag, exflag =
                        GetWindowLongPtr(hwnd, GWL_EXSTYLE);

                    nexflag = exflag;
                    if (conf_get_bool(conf, CONF_alwaysontop) !=
                        conf_get_bool(prev_conf, CONF_alwaysontop)) {
                        if (conf_get_bool(conf, CONF_alwaysontop)) {
                            nexflag |= WS_EX_TOPMOST;
                            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                                         SWP_NOMOVE | SWP_NOSIZE);
                        } else {
                            nexflag &= ~(WS_EX_TOPMOST);
                            SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                                         SWP_NOMOVE | SWP_NOSIZE);
                        }
                    }
                    if (conf_get_bool(conf, CONF_sunken_edge))
                        nexflag |= WS_EX_CLIENTEDGE;
                    else
                        nexflag &= ~(WS_EX_CLIENTEDGE);

                    nflg = flag;
                    if (conf_get_bool(conf, is_full_screen() ?
                                      CONF_scrollbar_in_fullscreen :
                                      CONF_scrollbar))
                        nflg |= WS_VSCROLL;
                    else
                        nflg &= ~WS_VSCROLL;

                    if (resize_action == RESIZE_DISABLED ||
                        is_full_screen())
                        nflg &= ~WS_THICKFRAME;
                    else
                        nflg |= WS_THICKFRAME;

                    if (resize_action == RESIZE_DISABLED)
                        nflg &= ~WS_MAXIMIZEBOX;
                    else
                        nflg |= WS_MAXIMIZEBOX;

                    if (nflg != flag || nexflag != exflag) {
                        if (nflg != flag)
                            SetWindowLongPtr(hwnd, GWL_STYLE, nflg);
                        if (nexflag != exflag)
                            SetWindowLongPtr(hwnd, GWL_EXSTYLE, nexflag);

                        SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                                     SWP_NOACTIVATE | SWP_NOCOPYBITS |
                                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                                     SWP_FRAMECHANGED);

                        init_lvl = 2;
                    }
                }

                /* Oops */
                if (resize_action == RESIZE_DISABLED && IsZoomed(hwnd)) {
                    force_normal(hwnd);
                    init_lvl = 2;
                }

                win_set_title(wintw, conf_get_str(conf, CONF_wintitle));
                if (IsIconic(hwnd)) {
                    SetWindowText(hwnd,
                                  conf_get_bool(conf, CONF_win_name_always) ?
                                  window_name : icon_name);
                }

                {
                    FontSpec *font = conf_get_fontspec(conf, CONF_font);
                    FontSpec *prev_font = conf_get_fontspec(prev_conf,
                                                             CONF_font);

                    if (!strcmp(font->name, prev_font->name) ||
                        !strcmp(conf_get_str(conf, CONF_line_codepage),
                                conf_get_str(prev_conf, CONF_line_codepage)) ||
                        font->isbold != prev_font->isbold ||
                        font->height != prev_font->height ||
                        font->charset != prev_font->charset ||
                        conf_get_int(conf, CONF_font_quality) !=
                        conf_get_int(prev_conf, CONF_font_quality) ||
                        conf_get_int(conf, CONF_vtmode) !=
                        conf_get_int(prev_conf, CONF_vtmode) ||
                        conf_get_int(conf, CONF_bold_style) !=
                        conf_get_int(prev_conf, CONF_bold_style) ||
                        resize_action == RESIZE_DISABLED ||
                        resize_action == RESIZE_EITHER ||
                        resize_action != conf_get_int(prev_conf,
                                                      CONF_resize_action))
                        init_lvl = 2;
                }

                InvalidateRect(hwnd, NULL, true);
                reset_window(init_lvl);

                conf_free(prev_conf);
            }
            break;
          case IDM_COPYALL:
            term_copyall(term, clips_system, lenof(clips_system));
            break;
          case IDM_COPY:
            term_request_copy(term, clips_system, lenof(clips_system));
            break;
          case IDM_PASTE:
            term_request_paste(term, CLIP_SYSTEM);
            break;
          case IDM_CLRSB:
            term_clrsb(term);
            break;
          case IDM_RESET:
            term_pwron(term, true);
            if (ldisc)
                ldisc_echoedit_update(ldisc);
            break;
          case IDM_ABOUT:
            showabout(hwnd);
            break;
          case IDM_HELP:
            launch_help(hwnd, NULL);
            break;
          case SC_MOUSEMENU:
            /*
             * We get this if the System menu has been activated
             * using the mouse.
             */
            show_mouseptr(true);
            break;
          case SC_KEYMENU:
            /*
             * We get this if the System menu has been activated
             * using the keyboard. This might happen from within
             * TranslateKey, in which case it really wants to be
             * followed by a `space' character to actually _bring
             * the menu up_ rather than just sitting there in
             * `ready to appear' state.
             */
            show_mouseptr(true);    /* make sure pointer is visible */
            if( lParam == 0 )
                PostMessage(hwnd, WM_CHAR, ' ', 0);
            break;
          case IDM_FULLSCREEN:
            flip_full_screen();
            break;
          default:
            if (wParam >= IDM_SAVED_MIN && wParam < IDM_SAVED_MAX) {
                SendMessage(hwnd, WM_SYSCOMMAND, IDM_SAVEDSESS, wParam);
            }
            if (wParam >= IDM_SPECIAL_MIN && wParam <= IDM_SPECIAL_MAX) {
                int i = (wParam - IDM_SPECIAL_MIN) / 0x10;
                /*
                 * Ensure we haven't been sent a bogus SYSCOMMAND
                 * which would cause us to reference invalid memory
                 * and crash. Perhaps I'm just too paranoid here.
                 */
                if (i >= n_specials)
                    break;
                if (backend)
                    backend_special(
                        backend, specials[i].code, specials[i].arg);
            }
        }
        break;

#define X_POS(l) ((int)(short)LOWORD(l))
#define Y_POS(l) ((int)(short)HIWORD(l))

#define TO_CHR_X(x) ((((x)<0 ? (x)-font_width+1 : (x))-offset_width) / font_width)
#define TO_CHR_Y(y) ((((y)<0 ? (y)-font_height+1: (y))-offset_height) / font_height)
      case WM_LBUTTONDOWN:
      case WM_MBUTTONDOWN:
      case WM_RBUTTONDOWN:
      case WM_LBUTTONUP:
      case WM_MBUTTONUP:
      case WM_RBUTTONUP:
        if (message == WM_RBUTTONDOWN &&
            ((wParam & MK_CONTROL) ||
             (conf_get_int(conf, CONF_mouse_is_xterm) == 2))) {
            POINT cursorpos;

            show_mouseptr(true);    /* make sure pointer is visible */
            GetCursorPos(&cursorpos);
            TrackPopupMenu(popup_menus[CTXMENU].menu,
                           TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON,
                           cursorpos.x, cursorpos.y,
                           0, hwnd, NULL);
            break;
        }
        {
            int button;
            bool press;

            switch (message) {
              case WM_LBUTTONDOWN:
                button = MBT_LEFT;
                wParam |= MK_LBUTTON;
                press = true;
                break;
              case WM_MBUTTONDOWN:
                button = MBT_MIDDLE;
                wParam |= MK_MBUTTON;
                press = true;
                break;
              case WM_RBUTTONDOWN:
                button = MBT_RIGHT;
                wParam |= MK_RBUTTON;
                press = true;
                break;
              case WM_LBUTTONUP:
                button = MBT_LEFT;
                wParam &= ~MK_LBUTTON;
                press = false;
                break;
              case WM_MBUTTONUP:
                button = MBT_MIDDLE;
                wParam &= ~MK_MBUTTON;
                press = false;
                break;
              case WM_RBUTTONUP:
                button = MBT_RIGHT;
                wParam &= ~MK_RBUTTON;
                press = false;
                break;
              default: /* shouldn't happen */
                button = 0;
                press = false;
            }
            show_mouseptr(true);
            /*
             * Special case: in full-screen mode, if the left
             * button is clicked in the very top left corner of the
             * window, we put up the System menu instead of doing
             * selection.
             */
            {
                bool mouse_on_hotspot = false;
                POINT pt;

                GetCursorPos(&pt);
#ifndef NO_MULTIMON
                {
                    HMONITOR mon;
                    MONITORINFO mi;

                    mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONULL);

                    if (mon != NULL) {
                        mi.cbSize = sizeof(MONITORINFO);
                        GetMonitorInfo(mon, &mi);

                        if (mi.rcMonitor.left == pt.x &&
                            mi.rcMonitor.top == pt.y) {
                            mouse_on_hotspot = true;
                        }
                    }
                }
#else
                if (pt.x == 0 && pt.y == 0) {
                    mouse_on_hotspot = true;
                }
#endif
                if (is_full_screen() && press &&
                    button == MBT_LEFT && mouse_on_hotspot) {
                    SendMessage(hwnd, WM_SYSCOMMAND, SC_MOUSEMENU,
                                MAKELPARAM(pt.x, pt.y));
                    return 0;
                }
            }

            if (press) {
                click(button,
                      TO_CHR_X(X_POS(lParam)), TO_CHR_Y(Y_POS(lParam)),
                      wParam & MK_SHIFT, wParam & MK_CONTROL,
                      is_alt_pressed());
                SetCapture(hwnd);
            } else {
                term_mouse(term, button, translate_button(button), MA_RELEASE,
                           TO_CHR_X(X_POS(lParam)),
                           TO_CHR_Y(Y_POS(lParam)), wParam & MK_SHIFT,
                           wParam & MK_CONTROL, is_alt_pressed());
                if (!(wParam & (MK_LBUTTON | MK_MBUTTON | MK_RBUTTON)))
                    ReleaseCapture();
            }
        }
        return 0;
      case WM_MOUSEMOVE:
        {
            /*
             * Windows seems to like to occasionally send MOUSEMOVE
             * events even if the mouse hasn't moved. Don't unhide
             * the mouse pointer in this case.
             */
            static WPARAM wp = 0;
            static LPARAM lp = 0;
            if (wParam != wp || lParam != lp ||
                last_mousemove != WM_MOUSEMOVE) {
                show_mouseptr(true);
                wp = wParam; lp = lParam;
                last_mousemove = WM_MOUSEMOVE;
            }
        }
        /*
         * Add the mouse position and message time to the random
         * number noise.
         */
        noise_ultralight(NOISE_SOURCE_MOUSEPOS, lParam);

        if (wParam & (MK_LBUTTON | MK_MBUTTON | MK_RBUTTON) &&
            GetCapture() == hwnd) {
            Mouse_Button b;
            if (wParam & MK_LBUTTON)
                b = MBT_LEFT;
            else if (wParam & MK_MBUTTON)
                b = MBT_MIDDLE;
            else
                b = MBT_RIGHT;
            term_mouse(term, b, translate_button(b), MA_DRAG,
                       TO_CHR_X(X_POS(lParam)),
                       TO_CHR_Y(Y_POS(lParam)), wParam & MK_SHIFT,
                       wParam & MK_CONTROL, is_alt_pressed());
        }
        return 0;
      case WM_NCMOUSEMOVE:
        {
            static WPARAM wp = 0;
            static LPARAM lp = 0;
            if (wParam != wp || lParam != lp ||
                last_mousemove != WM_NCMOUSEMOVE) {
                show_mouseptr(true);
                wp = wParam; lp = lParam;
                last_mousemove = WM_NCMOUSEMOVE;
            }
        }
        noise_ultralight(NOISE_SOURCE_MOUSEPOS, lParam);
        break;
      case WM_IGNORE_CLIP:
        ignore_clip = wParam;          /* don't panic on DESTROYCLIPBOARD */
        break;
      case WM_DESTROYCLIPBOARD:
        if (!ignore_clip)
            term_lost_clipboard_ownership(term, CLIP_SYSTEM);
        ignore_clip = false;
        return 0;
      case WM_PAINT:
        {
            PAINTSTRUCT p;

            HideCaret(hwnd);
            hdc = BeginPaint(hwnd, &p);
            if (pal) {
                SelectPalette(hdc, pal, true);
                RealizePalette(hdc);
            }

            /*
             * We have to be careful about term_paint(). It will
             * set a bunch of character cells to INVALID and then
             * call do_paint(), which will redraw those cells and
             * _then mark them as done_. This may not be accurate:
             * when painting in WM_PAINT context we are restricted
             * to the rectangle which has just been exposed - so if
             * that only covers _part_ of a character cell and the
             * rest of it was already visible, that remainder will
             * not be redrawn at all. Accordingly, we must not
             * paint any character cell in a WM_PAINT context which
             * already has a pending update due to terminal output.
             * The simplest solution to this - and many, many
             * thanks to Hung-Te Lin for working all this out - is
             * not to do any actual painting at _all_ if there's a
             * pending terminal update: just mark the relevant
             * character cells as INVALID and wait for the
             * scheduled full update to sort it out.
             *
             * I have a suspicion this isn't the _right_ solution.
             * An alternative approach would be to have terminal.c
             * separately track what _should_ be on the terminal
             * screen and what _is_ on the terminal screen, and
             * have two completely different types of redraw (one
             * for full updates, which syncs the former with the
             * terminal itself, and one for WM_PAINT which syncs
             * the latter with the former); yet another possibility
             * would be to have the Windows front end do what the
             * GTK one already does, and maintain a bitmap of the
             * current terminal appearance so that WM_PAINT becomes
             * completely trivial. However, this should do for now.
             */
            assert(!wintw_hdc);
            wintw_hdc = hdc;
            term_paint(term,
                       (p.rcPaint.left-offset_width)/font_width,
                       (p.rcPaint.top-offset_height)/font_height,
                       (p.rcPaint.right-offset_width-1)/font_width,
                       (p.rcPaint.bottom-offset_height-1)/font_height,
                       !term->window_update_pending);
            wintw_hdc = NULL;

            if (p.fErase ||
                p.rcPaint.left  < offset_width  ||
                p.rcPaint.top   < offset_height ||
                p.rcPaint.right >= offset_width + font_width*term->cols ||
                p.rcPaint.bottom>= offset_height + font_height*term->rows)
            {
                HBRUSH fillcolour, oldbrush;
                HPEN   edge, oldpen;
                fillcolour = CreateSolidBrush (
                                    colours[ATTR_DEFBG>>ATTR_BGSHIFT]);
                oldbrush = SelectObject(hdc, fillcolour);
                edge = CreatePen(PS_SOLID, 0,
                                    colours[ATTR_DEFBG>>ATTR_BGSHIFT]);
                oldpen = SelectObject(hdc, edge);

                /*
                 * Jordan Russell reports that this apparently
                 * ineffectual IntersectClipRect() call masks a
                 * Windows NT/2K bug causing strange display
                 * problems when the PuTTY window is taller than
                 * the primary monitor. It seems harmless enough...
                 */
                IntersectClipRect(hdc,
                        p.rcPaint.left, p.rcPaint.top,
                        p.rcPaint.right, p.rcPaint.bottom);

                ExcludeClipRect(hdc,
                        offset_width, offset_height,
                        offset_width+font_width*term->cols,
                        offset_height+font_height*term->rows);

                Rectangle(hdc, p.rcPaint.left, p.rcPaint.top,
                          p.rcPaint.right, p.rcPaint.bottom);

                /* SelectClipRgn(hdc, NULL); */

                SelectObject(hdc, oldbrush);
                DeleteObject(fillcolour);
                SelectObject(hdc, oldpen);
                DeleteObject(edge);
            }
            SelectObject(hdc, GetStockObject(SYSTEM_FONT));
            SelectObject(hdc, GetStockObject(WHITE_PEN));
            EndPaint(hwnd, &p);
            ShowCaret(hwnd);
        }
        return 0;
      case WM_NETEVENT:
        {
            /*
             * To protect against re-entrancy when Windows's recv()
             * immediately triggers a new WSAAsyncSelect window
             * message, we don't call select_result directly from this
             * handler but instead wait until we're back out at the
             * top level of the message loop.
             */
            struct wm_netevent_params *params =
                snew(struct wm_netevent_params);
            params->wParam = wParam;
            params->lParam = lParam;
            queue_toplevel_callback(wm_netevent_callback, params);
        }
        return 0;
      case WM_SETFOCUS:
        term_set_focus(term, true);
        CreateCaret(hwnd, caretbm, font_width, font_height);
        ShowCaret(hwnd);
        flash_window(0);               /* stop */
        compose_state = 0;
        term_update(term);
        break;
      case WM_KILLFOCUS:
        show_mouseptr(true);
        term_set_focus(term, false);
        DestroyCaret();
        caret_x = caret_y = -1;        /* ensure caret is replaced next time */
        term_update(term);
        break;
      case WM_ENTERSIZEMOVE:
#ifdef RDB_DEBUG_PATCH
        debug("WM_ENTERSIZEMOVE\n");
#endif
        EnableSizeTip(true);
        resizing = true;
        need_backend_resize = false;
        break;
      case WM_EXITSIZEMOVE:
        EnableSizeTip(false);
        resizing = false;
#ifdef RDB_DEBUG_PATCH
        debug("WM_EXITSIZEMOVE\n");
#endif
        if (need_backend_resize) {
            term_size(term, conf_get_int(conf, CONF_height),
                      conf_get_int(conf, CONF_width),
                      conf_get_int(conf, CONF_savelines));
            InvalidateRect(hwnd, NULL, true);
        }
        break;
      case WM_SIZING:
        /*
         * This does two jobs:
         * 1) Keep the sizetip uptodate
         * 2) Make sure the window size is _stepped_ in units of the font size.
         */
        resize_action = conf_get_int(conf, CONF_resize_action);
        if (resize_action == RESIZE_TERM ||
            (resize_action == RESIZE_EITHER && !is_alt_pressed())) {
            int width, height, w, h, ew, eh;
            LPRECT r = (LPRECT) lParam;

            if (!need_backend_resize && resize_action == RESIZE_EITHER &&
                (conf_get_int(conf, CONF_height) != term->rows ||
                 conf_get_int(conf, CONF_width) != term->cols)) {
                /*
                 * Great! It seems that both the terminal size and the
                 * font size have been changed and the user is now dragging.
                 *
                 * It will now be difficult to get back to the configured
                 * font size!
                 *
                 * This would be easier but it seems to be too confusing.
                 */
                conf_set_int(conf, CONF_height, term->rows);
                conf_set_int(conf, CONF_width, term->cols);

                InvalidateRect(hwnd, NULL, true);
                need_backend_resize = true;
            }

            width = r->right - r->left - extra_width;
            height = r->bottom - r->top - extra_height;
            w = (width + font_width / 2) / font_width;
            if (w < 1)
                w = 1;
            h = (height + font_height / 2) / font_height;
            if (h < 1)
                h = 1;
            UpdateSizeTip(hwnd, w, h);
            ew = width - w * font_width;
            eh = height - h * font_height;
            if (ew != 0) {
                if (wParam == WMSZ_LEFT ||
                    wParam == WMSZ_BOTTOMLEFT || wParam == WMSZ_TOPLEFT)
                    r->left += ew;
                else
                    r->right -= ew;
            }
            if (eh != 0) {
                if (wParam == WMSZ_TOP ||
                    wParam == WMSZ_TOPRIGHT || wParam == WMSZ_TOPLEFT)
                    r->top += eh;
                else
                    r->bottom -= eh;
            }
            if (ew || eh)
                return 1;
            else
                return 0;
        } else {
            int width, height, w, h, rv = 0;
            int window_border = conf_get_int(conf, CONF_window_border);
            int ex_width = extra_width + (window_border - offset_width) * 2;
            int ex_height = extra_height + (window_border - offset_height) * 2;
            LPRECT r = (LPRECT) lParam;

            width = r->right - r->left - ex_width;
            height = r->bottom - r->top - ex_height;

            w = (width + term->cols/2)/term->cols;
            h = (height + term->rows/2)/term->rows;
            if ( r->right != r->left + w*term->cols + ex_width)
                rv = 1;

            if (wParam == WMSZ_LEFT ||
                wParam == WMSZ_BOTTOMLEFT || wParam == WMSZ_TOPLEFT)
                r->left = r->right - w*term->cols - ex_width;
            else
                r->right = r->left + w*term->cols + ex_width;

            if (r->bottom != r->top + h*term->rows + ex_height)
                rv = 1;

            if (wParam == WMSZ_TOP ||
                wParam == WMSZ_TOPRIGHT || wParam == WMSZ_TOPLEFT)
                r->top = r->bottom - h*term->rows - ex_height;
            else
                r->bottom = r->top + h*term->rows + ex_height;

            return rv;
        }
        /* break;  (never reached) */
      case WM_FULLSCR_ON_MAX:
        fullscr_on_max = true;
        break;
      case WM_MOVE:
        sys_cursor_update();
        break;
      case WM_SIZE:
        resize_action = conf_get_int(conf, CONF_resize_action);
#ifdef RDB_DEBUG_PATCH
        debug("WM_SIZE %s (%d,%d)\n",
              (wParam == SIZE_MINIMIZED) ? "SIZE_MINIMIZED":
              (wParam == SIZE_MAXIMIZED) ? "SIZE_MAXIMIZED":
              (wParam == SIZE_RESTORED && resizing) ? "to":
              (wParam == SIZE_RESTORED) ? "SIZE_RESTORED":
              "...",
              LOWORD(lParam), HIWORD(lParam));
#endif
        if (wParam == SIZE_MINIMIZED)
            SetWindowText(hwnd,
                          conf_get_bool(conf, CONF_win_name_always) ?
                          window_name : icon_name);
        if (wParam == SIZE_RESTORED || wParam == SIZE_MAXIMIZED)
            SetWindowText(hwnd, window_name);
        if (wParam == SIZE_RESTORED) {
            processed_resize = false;
            clear_full_screen();
            if (processed_resize) {
                /*
                 * Inhibit normal processing of this WM_SIZE; a
                 * secondary one was triggered just now by
                 * clear_full_screen which contained the correct
                 * client area size.
                 */
                return 0;
            }
        }
        if (wParam == SIZE_MAXIMIZED && fullscr_on_max) {
            fullscr_on_max = false;
            processed_resize = false;
            make_full_screen();
            if (processed_resize) {
                /*
                 * Inhibit normal processing of this WM_SIZE; a
                 * secondary one was triggered just now by
                 * make_full_screen which contained the correct client
                 * area size.
                 */
                return 0;
            }
        }

        processed_resize = true;

        if (resize_action == RESIZE_DISABLED) {
            /* A resize, well it better be a minimize. */
            reset_window(-1);
        } else {

            int width, height, w, h;
            int window_border = conf_get_int(conf, CONF_window_border);

            width = LOWORD(lParam);
            height = HIWORD(lParam);

            if (wParam == SIZE_MAXIMIZED) {
                was_zoomed = true;
                prev_rows = term->rows;
                prev_cols = term->cols;
                if (resize_action == RESIZE_TERM) {
                    w = width / font_width;
                    if (w < 1) w = 1;
                    h = height / font_height;
                    if (h < 1) h = 1;

                    if (resizing) {
                        /*
                         * As below, if we're in the middle of an
                         * interactive resize we don't call
                         * back->size. In Windows 7, this case can
                         * arise in maximisation as well via the Aero
                         * snap UI.
                         */
                        need_backend_resize = true;
                        conf_set_int(conf, CONF_height, h);
                        conf_set_int(conf, CONF_width, w);
                    } else {
                        term_size(term, h, w,
                                  conf_get_int(conf, CONF_savelines));
                    }
                }
                reset_window(0);
            } else if (wParam == SIZE_RESTORED && was_zoomed) {
                was_zoomed = false;
                if (resize_action == RESIZE_TERM) {
                    w = (width-window_border*2) / font_width;
                    if (w < 1) w = 1;
                    h = (height-window_border*2) / font_height;
                    if (h < 1) h = 1;
                    term_size(term, h, w, conf_get_int(conf, CONF_savelines));
                    reset_window(2);
                } else if (resize_action != RESIZE_FONT)
                    reset_window(2);
                else
                    reset_window(0);
            } else if (wParam == SIZE_MINIMIZED) {
                /* do nothing */
            } else if (resize_action == RESIZE_TERM ||
                       (resize_action == RESIZE_EITHER &&
                        !is_alt_pressed())) {
                w = (width-window_border*2) / font_width;
                if (w < 1) w = 1;
                h = (height-window_border*2) / font_height;
                if (h < 1) h = 1;

                if (resizing) {
                    /*
                     * Don't call back->size in mid-resize. (To
                     * prevent massive numbers of resize events
                     * getting sent down the connection during an NT
                     * opaque drag.)
                     */
                    need_backend_resize = true;
                    conf_set_int(conf, CONF_height, h);
                    conf_set_int(conf, CONF_width, w);
                } else {
                    term_size(term, h, w, conf_get_int(conf, CONF_savelines));
                }
            } else {
                reset_window(0);
            }
        }
        sys_cursor_update();
        return 0;
      case WM_VSCROLL:
        switch (LOWORD(wParam)) {
          case SB_BOTTOM:
            term_scroll(term, -1, 0);
            break;
          case SB_TOP:
            term_scroll(term, +1, 0);
            break;
          case SB_LINEDOWN:
            term_scroll(term, 0, +1);
            break;
          case SB_LINEUP:
            term_scroll(term, 0, -1);
            break;
          case SB_PAGEDOWN:
            term_scroll(term, 0, +term->rows / 2);
            break;
          case SB_PAGEUP:
            term_scroll(term, 0, -term->rows / 2);
            break;
          case SB_THUMBPOSITION:
          case SB_THUMBTRACK:
            /*
             * Use GetScrollInfo instead of HIWORD(wParam) to get
             * 32-bit scroll position.
             */
            {
                SCROLLINFO si;

                si.cbSize = sizeof(si);
                si.fMask = SIF_TRACKPOS;
                if (GetScrollInfo(hwnd, SB_VERT, &si) == 0)
                    si.nTrackPos = HIWORD(wParam);
                term_scroll(term, 1, si.nTrackPos);
            }
            break;
        }
        break;
      case WM_PALETTECHANGED:
        if ((HWND) wParam != hwnd && pal != NULL) {
            HDC hdc = make_hdc();
            if (hdc) {
                if (RealizePalette(hdc) > 0)
                    UpdateColors(hdc);
                free_hdc(hdc);
            }
        }
        break;
      case WM_QUERYNEWPALETTE:
        if (pal != NULL) {
            HDC hdc = make_hdc();
            if (hdc) {
                if (RealizePalette(hdc) > 0)
                    UpdateColors(hdc);
                free_hdc(hdc);
                return true;
            }
        }
        return false;
      case WM_KEYDOWN:
      case WM_SYSKEYDOWN:
      case WM_KEYUP:
      case WM_SYSKEYUP:
        /*
         * Add the scan code and keypress timing to the random
         * number noise.
         */
        noise_ultralight(NOISE_SOURCE_KEY, lParam);

        /*
         * We don't do TranslateMessage since it disassociates the
         * resulting CHAR message from the KEYDOWN that sparked it,
         * which we occasionally don't want. Instead, we process
         * KEYDOWN, and call the Win32 translator functions so that
         * we get the translations under _our_ control.
         */
        {
            unsigned char buf[20];
            int len;

            if (wParam == VK_PROCESSKEY || /* IME PROCESS key */
                wParam == VK_PACKET) {     /* 'this key is a Unicode char' */
                if (message == WM_KEYDOWN) {
                    MSG m;
                    m.hwnd = hwnd;
                    m.message = WM_KEYDOWN;
                    m.wParam = wParam;
                    m.lParam = lParam & 0xdfff;
                    TranslateMessage(&m);
                } else break; /* pass to Windows for default processing */
            } else {
                len = TranslateKey(message, wParam, lParam, buf);
                if (len == -1)
                    return DefWindowProcW(hwnd, message, wParam, lParam);

                if (len != 0) {
                    /*
                     * We need not bother about stdin backlogs
                     * here, because in GUI PuTTY we can't do
                     * anything about it anyway; there's no means
                     * of asking Windows to hold off on KEYDOWN
                     * messages. We _have_ to buffer everything
                     * we're sent.
                     */
                    term_keyinput(term, -1, buf, len);
                    show_mouseptr(false);
                }
            }
        }
        return 0;
      case WM_INPUTLANGCHANGE:
        /* wParam == Font number */
        /* lParam == Locale */
        set_input_locale((HKL)lParam);
        sys_cursor_update();
        break;
      case WM_IME_STARTCOMPOSITION:
        {
            HIMC hImc = ImmGetContext(hwnd);
            ImmSetCompositionFont(hImc, &lfont);
            ImmReleaseContext(hwnd, hImc);
        }
        break;
      case WM_IME_COMPOSITION:
        {
            HIMC hIMC;
            int n;
            char *buff;

            if (osPlatformId == VER_PLATFORM_WIN32_WINDOWS ||
                osPlatformId == VER_PLATFORM_WIN32s)
                break; /* no Unicode */

            if ((lParam & GCS_RESULTSTR) == 0) /* Composition unfinished. */
                break; /* fall back to DefWindowProc */

            hIMC = ImmGetContext(hwnd);
            n = ImmGetCompositionStringW(hIMC, GCS_RESULTSTR, NULL, 0);

            if (n > 0) {
                int i;
                buff = snewn(n, char);
                ImmGetCompositionStringW(hIMC, GCS_RESULTSTR, buff, n);
                /*
                 * Jaeyoun Chung reports that Korean character
                 * input doesn't work correctly if we do a single
                 * term_keyinputw covering the whole of buff. So
                 * instead we send the characters one by one.
                 */
                /* don't divide SURROGATE PAIR */
                if (ldisc) {
                    for (i = 0; i < n; i += 2) {
                        WCHAR hs = *(unsigned short *)(buff+i);
                        if (IS_HIGH_SURROGATE(hs) && i+2 < n) {
                            WCHAR ls = *(unsigned short *)(buff+i+2);
                            if (IS_LOW_SURROGATE(ls)) {
                                term_keyinputw(
                                    term, (unsigned short *)(buff+i), 2);
                                i += 2;
                                continue;
                            }
                        }
                        term_keyinputw(
                            term, (unsigned short *)(buff+i), 1);
                    }
                }
                free(buff);
            }
            ImmReleaseContext(hwnd, hIMC);
            return 1;
        }

      case WM_IME_CHAR:
        if (wParam & 0xFF00) {
            char buf[2];

            buf[1] = wParam;
            buf[0] = wParam >> 8;
            term_keyinput(term, kbd_codepage, buf, 2);
        } else {
            char c = (unsigned char) wParam;
            term_seen_key_event(term);
            term_keyinput(term, kbd_codepage, &c, 1);
        }
        return (0);
      case WM_CHAR:
      case WM_SYSCHAR:
        /*
         * Nevertheless, we are prepared to deal with WM_CHAR
         * messages, should they crop up. So if someone wants to
         * post the things to us as part of a macro manoeuvre,
         * we're ready to cope.
         */
        {
            static wchar_t pending_surrogate = 0;
            wchar_t c = wParam;

            if (IS_HIGH_SURROGATE(c)) {
                pending_surrogate = c;
            } else if (IS_SURROGATE_PAIR(pending_surrogate, c)) {
                wchar_t pair[2];
                pair[0] = pending_surrogate;
                pair[1] = c;
                term_keyinputw(term, pair, 2);
            } else if (!IS_SURROGATE(c)) {
                term_keyinputw(term, &c, 1);
            }
        }
        return 0;
      case WM_SYSCOLORCHANGE:
        if (conf_get_bool(conf, CONF_system_colour)) {
            /* Refresh palette from system colours. */
            /* XXX actually this zaps the entire palette. */
            systopalette();
            init_palette();
            /* Force a repaint of the terminal window. */
            term_invalidate(term);
        }
        break;
      case WM_AGENT_CALLBACK:
        {
            struct agent_callback *c = (struct agent_callback *)lParam;
            c->callback(c->callback_ctx, c->data, c->len);
            sfree(c);
        }
        return 0;
      case WM_GOT_CLIPDATA:
        process_clipdata((HGLOBAL)lParam, wParam);
        return 0;
      default:
        if (message == wm_mousewheel || message == WM_MOUSEWHEEL) {
            bool shift_pressed = false, control_pressed = false;

            if (message == WM_MOUSEWHEEL) {
                wheel_accumulator += (short)HIWORD(wParam);
                shift_pressed=LOWORD(wParam) & MK_SHIFT;
                control_pressed=LOWORD(wParam) & MK_CONTROL;
            } else {
                BYTE keys[256];
                wheel_accumulator += (int)wParam;
                if (GetKeyboardState(keys)!=0) {
                    shift_pressed=keys[VK_SHIFT]&0x80;
                    control_pressed=keys[VK_CONTROL]&0x80;
                }
            }

            /* process events when the threshold is reached */
            while (abs(wheel_accumulator) >= WHEEL_DELTA) {
                int b;

                /* reduce amount for next time */
                if (wheel_accumulator > 0) {
                    b = MBT_WHEEL_UP;
                    wheel_accumulator -= WHEEL_DELTA;
                } else if (wheel_accumulator < 0) {
                    b = MBT_WHEEL_DOWN;
                    wheel_accumulator += WHEEL_DELTA;
                } else
                    break;

                if (send_raw_mouse &&
                    !(conf_get_bool(conf, CONF_mouse_override) &&
                      shift_pressed)) {
                    /* Mouse wheel position is in screen coordinates for
                     * some reason */
                    POINT p;
                    p.x = X_POS(lParam); p.y = Y_POS(lParam);
                    if (ScreenToClient(hwnd, &p)) {
                        /* send a mouse-down followed by a mouse up */
                        term_mouse(term, b, translate_button(b),
                                   MA_CLICK,
                                   TO_CHR_X(p.x),
                                   TO_CHR_Y(p.y), shift_pressed,
                                   control_pressed, is_alt_pressed());
                    } /* else: not sure when this can fail */
                } else {
                    /* trigger a scroll */
                    term_scroll(term, 0,
                                b == MBT_WHEEL_UP ?
                                -term->rows / 2 : term->rows / 2);
                }
            }
            return 0;
        }
    }

    /*
     * Any messages we don't process completely above are passed through to
     * DefWindowProc() for default processing.
     */
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

/*
 * Move the system caret. (We maintain one, even though it's
 * invisible, for the benefit of blind people: apparently some
 * helper software tracks the system caret, so we should arrange to
 * have one.)
 */
static void wintw_set_cursor_pos(TermWin *tw, int x, int y)
{
    int cx, cy;

    if (!term->has_focus) return;

    /*
     * Avoid gratuitously re-updating the cursor position and IMM
     * window if there's no actual change required.
     */
    cx = x * font_width + offset_width;
    cy = y * font_height + offset_height;
    if (cx == caret_x && cy == caret_y)
        return;
    caret_x = cx;
    caret_y = cy;

    sys_cursor_update();
}

static void sys_cursor_update(void)
{
    COMPOSITIONFORM cf;
    HIMC hIMC;

    if (!term->has_focus) return;

    if (caret_x < 0 || caret_y < 0)
        return;

    SetCaretPos(caret_x, caret_y);

    /* IMM calls on Win98 and beyond only */
    if (osPlatformId == VER_PLATFORM_WIN32s) return; /* 3.11 */

    if (osPlatformId == VER_PLATFORM_WIN32_WINDOWS &&
        osMinorVersion == 0) return; /* 95 */

    /* we should have the IMM functions */
    hIMC = ImmGetContext(hwnd);
    cf.dwStyle = CFS_POINT;
    cf.ptCurrentPos.x = caret_x;
    cf.ptCurrentPos.y = caret_y;
    ImmSetCompositionWindow(hIMC, &cf);

    ImmReleaseContext(hwnd, hIMC);
}

/*
 * Draw a line of text in the window, at given character
 * coordinates, in given attributes.
 *
 * We are allowed to fiddle with the contents of `text'.
 */
static void do_text_internal(
    int x, int y, wchar_t *text, int len,
    unsigned long attr, int lattr, truecolour truecolour)
{
    COLORREF fg, bg, t;
    int nfg, nbg, nfont;
    RECT line_box;
    bool force_manual_underline = false;
    int fnt_width, char_width;
    int text_adjust = 0;
    int xoffset = 0;
    int maxlen, remaining;
    bool opaque;
    bool is_cursor = false;
    static int *lpDx = NULL;
    static size_t lpDx_len = 0;
    int *lpDx_maybe;
    int len2; /* for SURROGATE PAIR */

    lattr &= LATTR_MODE;

    char_width = fnt_width = font_width * (1 + (lattr != LATTR_NORM));

    if (attr & ATTR_WIDE)
        char_width *= 2;

    /* Only want the left half of double width lines */
    if (lattr != LATTR_NORM && x*2 >= term->cols)
        return;

    x *= fnt_width;
    y *= font_height;
    x += offset_width;
    y += offset_height;

    if ((attr & TATTR_ACTCURS) && (cursor_type == 0 || term->big_cursor)) {
        truecolour.fg = truecolour.bg = optionalrgb_none;
        attr &= ~(ATTR_REVERSE|ATTR_BLINK|ATTR_COLOURS|ATTR_DIM);
        /* cursor fg and bg */
        attr |= (260 << ATTR_FGSHIFT) | (261 << ATTR_BGSHIFT);
        is_cursor = true;
    }

    nfont = 0;
    if (vtmode == VT_POORMAN && lattr != LATTR_NORM) {
        /* Assume a poorman font is borken in other ways too. */
        lattr = LATTR_WIDE;
    } else
        switch (lattr) {
          case LATTR_NORM:
            break;
          case LATTR_WIDE:
            nfont |= FONT_WIDE;
            break;
          default:
            nfont |= FONT_WIDE + FONT_HIGH;
            break;
        }
    if (attr & ATTR_NARROW)
        nfont |= FONT_NARROW;

#ifdef USES_VTLINE_HACK
    /* Special hack for the VT100 linedraw glyphs. */
    if (text[0] >= 0x23BA && text[0] <= 0x23BD) {
        switch ((unsigned char) (text[0])) {
          case 0xBA:
            text_adjust = -2 * font_height / 5;
            break;
          case 0xBB:
            text_adjust = -1 * font_height / 5;
            break;
          case 0xBC:
            text_adjust = font_height / 5;
            break;
          case 0xBD:
            text_adjust = 2 * font_height / 5;
            break;
        }
        if (lattr == LATTR_TOP || lattr == LATTR_BOT)
            text_adjust *= 2;
        text[0] = ucsdata.unitab_xterm['q'];
        if (attr & ATTR_UNDER) {
            attr &= ~ATTR_UNDER;
            force_manual_underline = true;
        }
    }
#endif

    /* Anything left as an original character set is unprintable. */
    if (DIRECT_CHAR(text[0]) &&
        (len < 2 || !IS_SURROGATE_PAIR(text[0], text[1]))) {
        int i;
        for (i = 0; i < len; i++)
            text[i] = 0xFFFD;
    }

    /* OEM CP */
    if ((text[0] & CSET_MASK) == CSET_OEMCP)
        nfont |= FONT_OEM;

    nfg = ((attr & ATTR_FGMASK) >> ATTR_FGSHIFT);
    nbg = ((attr & ATTR_BGMASK) >> ATTR_BGSHIFT);
    if (bold_font_mode == BOLD_FONT && (attr & ATTR_BOLD))
        nfont |= FONT_BOLD;
    if (und_mode == UND_FONT && (attr & ATTR_UNDER))
        nfont |= FONT_UNDERLINE;
    another_font(nfont);
    if (!fonts[nfont]) {
        if (nfont & FONT_UNDERLINE)
            force_manual_underline = true;
        /* Don't do the same for manual bold, it could be bad news. */

        nfont &= ~(FONT_BOLD | FONT_UNDERLINE);
    }
    another_font(nfont);
    if (!fonts[nfont])
        nfont = FONT_NORMAL;
    if (attr & ATTR_REVERSE) {
        struct optionalrgb trgb;

        t = nfg;
        nfg = nbg;
        nbg = t;

        trgb = truecolour.fg;
        truecolour.fg = truecolour.bg;
        truecolour.bg = trgb;
    }
    if (bold_colours && (attr & ATTR_BOLD) && !is_cursor) {
        if (nfg < 16) nfg |= 8;
        else if (nfg >= 256) nfg |= 1;
    }
    if (bold_colours && (attr & ATTR_BLINK)) {
        if (nbg < 16) nbg |= 8;
        else if (nbg >= 256) nbg |= 1;
    }
    if (!pal && truecolour.fg.enabled)
        fg = RGB(truecolour.fg.r, truecolour.fg.g, truecolour.fg.b);
    else
        fg = colours[nfg];

    if (!pal && truecolour.bg.enabled)
        bg = RGB(truecolour.bg.r, truecolour.bg.g, truecolour.bg.b);
    else
        bg = colours[nbg];

    if (!pal && (attr & ATTR_DIM)) {
        fg = RGB(GetRValue(fg) * 2 / 3,
                 GetGValue(fg) * 2 / 3,
                 GetBValue(fg) * 2 / 3);
    }

    SelectObject(wintw_hdc, fonts[nfont]);
    SetTextColor(wintw_hdc, fg);
    SetBkColor(wintw_hdc, bg);
    if (attr & TATTR_COMBINING)
        SetBkMode(wintw_hdc, TRANSPARENT);
    else
        SetBkMode(wintw_hdc, OPAQUE);
    line_box.left = x;
    line_box.top = y;
    line_box.right = x + char_width * len;
    line_box.bottom = y + font_height;
    /* adjust line_box.right for SURROGATE PAIR & VARIATION SELECTOR */
    {
        int i;
        int rc_width = 0;
        for (i = 0; i < len ; i++) {
            if (i+1 < len && IS_HIGH_VARSEL(text[i], text[i+1])) {
                i++;
            } else if (i+1 < len && IS_SURROGATE_PAIR(text[i], text[i+1])) {
                rc_width += char_width;
                i++;
            } else if (IS_LOW_VARSEL(text[i])) {
                /* do nothing */
            } else {
                rc_width += char_width;
            }
        }
        line_box.right = line_box.left + rc_width;
    }

    /* Only want the left half of double width lines */
    if (line_box.right > font_width*term->cols+offset_width)
        line_box.right = font_width*term->cols+offset_width;

    if (font_varpitch) {
        /*
         * If we're using a variable-pitch font, we unconditionally
         * draw the glyphs one at a time and centre them in their
         * character cells (which means in particular that we must
         * disable the lpDx mechanism). This gives slightly odd but
         * generally reasonable results.
         */
        xoffset = char_width / 2;
        SetTextAlign(wintw_hdc, TA_TOP | TA_CENTER | TA_NOUPDATECP);
        lpDx_maybe = NULL;
        maxlen = 1;
    } else {
        /*
         * In a fixed-pitch font, we draw the whole string in one go
         * in the normal way.
         */
        xoffset = 0;
        SetTextAlign(wintw_hdc, TA_TOP | TA_LEFT | TA_NOUPDATECP);
        lpDx_maybe = lpDx;
        maxlen = len;
    }

    opaque = true;                     /* start by erasing the rectangle */
    for (remaining = len; remaining > 0;
         text += len, remaining -= len, x += char_width * len2) {
        len = (maxlen < remaining ? maxlen : remaining);
        /* don't divide SURROGATE PAIR and VARIATION SELECTOR */
        len2 = len;
        if (maxlen == 1) {
            if (remaining >= 1 && IS_SURROGATE_PAIR(text[0], text[1]))
                len++;
            if (remaining-len >= 1 && IS_LOW_VARSEL(text[len]))
                len++;
            else if (remaining-len >= 2 &&
                     IS_HIGH_VARSEL(text[len], text[len+1]))
                len += 2;
        }

        if (len > lpDx_len) {
            sgrowarray(lpDx, lpDx_len, len);
            if (lpDx_maybe) lpDx_maybe = lpDx;
        }

        {
            int i;
            /* only last char has dx width in SURROGATE PAIR and
             * VARIATION sequence */
            for (i = 0; i < len; i++) {
                lpDx[i] = char_width;
                if (i+1 < len && IS_HIGH_VARSEL(text[i], text[i+1])) {
                    if (i > 0) lpDx[i-1] = 0;
                    lpDx[i] = 0;
                    i++;
                    lpDx[i] = char_width;
                } else if (i+1 < len && IS_SURROGATE_PAIR(text[i],text[i+1])) {
                    lpDx[i] = 0;
                    i++;
                    lpDx[i] = char_width;
                } else if (IS_LOW_VARSEL(text[i])) {
                    if (i > 0) lpDx[i-1] = 0;
                    lpDx[i] = char_width;
                }
            }
        }

        /* We're using a private area for direct to font. (512 chars.) */
        if (ucsdata.dbcs_screenfont && (text[0] & CSET_MASK) == CSET_ACP) {
            /* Ho Hum, dbcs fonts are a PITA! */
            /* To display on W9x I have to convert to UCS */
            static wchar_t *uni_buf = 0;
            static int uni_len = 0;
            int nlen, mptr;
            if (len > uni_len) {
                sfree(uni_buf);
                uni_len = len;
                uni_buf = snewn(uni_len, wchar_t);
            }

            for(nlen = mptr = 0; mptr<len; mptr++) {
                uni_buf[nlen] = 0xFFFD;
                if (IsDBCSLeadByteEx(ucsdata.font_codepage,
                                     (BYTE) text[mptr])) {
                    char dbcstext[2];
                    dbcstext[0] = text[mptr] & 0xFF;
                    dbcstext[1] = text[mptr+1] & 0xFF;
                    lpDx[nlen] += char_width;
                    MultiByteToWideChar(ucsdata.font_codepage, MB_USEGLYPHCHARS,
                                        dbcstext, 2, uni_buf+nlen, 1);
                    mptr++;
                }
                else
                {
                    char dbcstext[1];
                    dbcstext[0] = text[mptr] & 0xFF;
                    MultiByteToWideChar(ucsdata.font_codepage, MB_USEGLYPHCHARS,
                                        dbcstext, 1, uni_buf+nlen, 1);
                }
                nlen++;
            }
            if (nlen <= 0)
                return;                /* Eeek! */

            ExtTextOutW(wintw_hdc, x + xoffset,
                        y - font_height * (lattr == LATTR_BOT) + text_adjust,
                        ETO_CLIPPED | (opaque ? ETO_OPAQUE : 0),
                        &line_box, uni_buf, nlen,
                        lpDx_maybe);
            if (bold_font_mode == BOLD_SHADOW && (attr & ATTR_BOLD)) {
                SetBkMode(wintw_hdc, TRANSPARENT);
                ExtTextOutW(wintw_hdc, x + xoffset - 1,
                            y - font_height * (lattr ==
                                               LATTR_BOT) + text_adjust,
                            ETO_CLIPPED, &line_box, uni_buf, nlen, lpDx_maybe);
            }

            lpDx[0] = -1;
        } else if (DIRECT_FONT(text[0])) {
            static char *directbuf = NULL;
            static size_t directlen = 0;

            sgrowarray(directbuf, directlen, len);
            for (size_t i = 0; i < len; i++)
                directbuf[i] = text[i] & 0xFF;

            ExtTextOut(wintw_hdc, x + xoffset,
                       y - font_height * (lattr == LATTR_BOT) + text_adjust,
                       ETO_CLIPPED | (opaque ? ETO_OPAQUE : 0),
                       &line_box, directbuf, len, lpDx_maybe);
            if (bold_font_mode == BOLD_SHADOW && (attr & ATTR_BOLD)) {
                SetBkMode(wintw_hdc, TRANSPARENT);

                /* GRR: This draws the character outside its box and
                 * can leave 'droppings' even with the clip box! I
                 * suppose I could loop it one character at a time ...
                 * yuk.
                 *
                 * Or ... I could do a test print with "W", and use +1
                 * or -1 for this shift depending on if the leftmost
                 * column is blank...
                 */
                ExtTextOut(wintw_hdc, x + xoffset - 1,
                           y - font_height * (lattr ==
                                              LATTR_BOT) + text_adjust,
                           ETO_CLIPPED, &line_box, directbuf, len, lpDx_maybe);
            }
        } else {
            /* And 'normal' unicode characters */
            static WCHAR *wbuf = NULL;
            static int wlen = 0;
            int i;

            if (wlen < len) {
                sfree(wbuf);
                wlen = len;
                wbuf = snewn(wlen, WCHAR);
            }

            for (i = 0; i < len; i++)
                wbuf[i] = text[i];

            /* print Glyphs as they are, without Windows' Shaping*/
            general_textout(wintw_hdc, x + xoffset,
                            y - font_height * (lattr==LATTR_BOT) + text_adjust,
                            &line_box, wbuf, len, lpDx,
                            opaque && !(attr & TATTR_COMBINING));

            /* And the shadow bold hack. */
            if (bold_font_mode == BOLD_SHADOW && (attr & ATTR_BOLD)) {
                SetBkMode(wintw_hdc, TRANSPARENT);
                ExtTextOutW(wintw_hdc, x + xoffset - 1,
                            y - font_height * (lattr ==
                                               LATTR_BOT) + text_adjust,
                            ETO_CLIPPED, &line_box, wbuf, len, lpDx_maybe);
            }
        }

        /*
         * If we're looping round again, stop erasing the background
         * rectangle.
         */
        SetBkMode(wintw_hdc, TRANSPARENT);
        opaque = false;
    }
    if (lattr != LATTR_TOP && (force_manual_underline ||
                               (und_mode == UND_LINE
                                && (attr & ATTR_UNDER)))) {
        HPEN oldpen;
        int dec = descent;
        if (lattr == LATTR_BOT)
            dec = dec * 2 - font_height;

        oldpen = SelectObject(wintw_hdc, CreatePen(PS_SOLID, 0, fg));
        MoveToEx(wintw_hdc, line_box.left, line_box.top + dec, NULL);
        LineTo(wintw_hdc, line_box.right, line_box.top + dec);
        oldpen = SelectObject(wintw_hdc, oldpen);
        DeleteObject(oldpen);
    }
}

/*
 * Wrapper that handles combining characters.
 */
static void wintw_draw_text(
    TermWin *tw, int x, int y, wchar_t *text, int len,
    unsigned long attr, int lattr, truecolour truecolour)
{
    if (attr & TATTR_COMBINING) {
        unsigned long a = 0;
        int len0 = 1;
        /* don't divide SURROGATE PAIR and VARIATION SELECTOR */
        if (len >= 2 && IS_SURROGATE_PAIR(text[0], text[1]))
            len0 = 2;
        if (len-len0 >= 1 && IS_LOW_VARSEL(text[len0])) {
            attr &= ~TATTR_COMBINING;
            do_text_internal(x, y, text, len0+1, attr, lattr, truecolour);
            text += len0+1;
            len -= len0+1;
            a = TATTR_COMBINING;
        } else if (len-len0 >= 2 && IS_HIGH_VARSEL(text[len0], text[len0+1])) {
            attr &= ~TATTR_COMBINING;
            do_text_internal(x, y, text, len0+2, attr, lattr, truecolour);
            text += len0+2;
            len -= len0+2;
            a = TATTR_COMBINING;
        } else {
            attr &= ~TATTR_COMBINING;
        }

        while (len--) {
            if (len >= 1 && IS_SURROGATE_PAIR(text[0], text[1])) {
                do_text_internal(x, y, text, 2, attr | a, lattr, truecolour);
                len--;
                text++;
            } else
                do_text_internal(x, y, text, 1, attr | a, lattr, truecolour);

            text++;
            a = TATTR_COMBINING;
        }
    } else
        do_text_internal(x, y, text, len, attr, lattr, truecolour);
}

static void wintw_draw_cursor(
    TermWin *tw, int x, int y, wchar_t *text, int len,
    unsigned long attr, int lattr, truecolour truecolour)
{
    int fnt_width;
    int char_width;
    int ctype = cursor_type;

    lattr &= LATTR_MODE;

    if ((attr & TATTR_ACTCURS) && (ctype == 0 || term->big_cursor)) {
        if (*text != UCSWIDE) {
            win_draw_text(tw, x, y, text, len, attr, lattr, truecolour);
            return;
        }
        ctype = 2;
        attr |= TATTR_RIGHTCURS;
    }

    fnt_width = char_width = font_width * (1 + (lattr != LATTR_NORM));
    if (attr & ATTR_WIDE)
        char_width *= 2;
    x *= fnt_width;
    y *= font_height;
    x += offset_width;
    y += offset_height;

    if ((attr & TATTR_PASCURS) && (ctype == 0 || term->big_cursor)) {
        POINT pts[5];
        HPEN oldpen;
        pts[0].x = pts[1].x = pts[4].x = x;
        pts[2].x = pts[3].x = x + char_width - 1;
        pts[0].y = pts[3].y = pts[4].y = y;
        pts[1].y = pts[2].y = y + font_height - 1;
        oldpen = SelectObject(wintw_hdc, CreatePen(PS_SOLID, 0, colours[261]));
        Polyline(wintw_hdc, pts, 5);
        oldpen = SelectObject(wintw_hdc, oldpen);
        DeleteObject(oldpen);
    } else if ((attr & (TATTR_ACTCURS | TATTR_PASCURS)) && ctype != 0) {
        int startx, starty, dx, dy, length, i;
        if (ctype == 1) {
            startx = x;
            starty = y + descent;
            dx = 1;
            dy = 0;
            length = char_width;
        } else {
            int xadjust = 0;
            if (attr & TATTR_RIGHTCURS)
                xadjust = char_width - 1;
            startx = x + xadjust;
            starty = y;
            dx = 0;
            dy = 1;
            length = font_height;
        }
        if (attr & TATTR_ACTCURS) {
            HPEN oldpen;
            oldpen =
                SelectObject(wintw_hdc, CreatePen(PS_SOLID, 0, colours[261]));
            MoveToEx(wintw_hdc, startx, starty, NULL);
            LineTo(wintw_hdc, startx + dx * length, starty + dy * length);
            oldpen = SelectObject(wintw_hdc, oldpen);
            DeleteObject(oldpen);
        } else {
            for (i = 0; i < length; i++) {
                if (i % 2 == 0) {
                    SetPixel(wintw_hdc, startx, starty, colours[261]);
                }
                startx += dx;
                starty += dy;
            }
        }
    }
}

static void wintw_draw_trust_sigil(TermWin *tw, int x, int y)
{
    x *= font_width;
    y *= font_height;
    x += offset_width;
    y += offset_height;

    DrawIconEx(wintw_hdc, x, y, trust_icon, font_width * 2, font_height,
               0, NULL, DI_NORMAL);
}

/* This function gets the actual width of a character in the normal font.
 */
static int wintw_char_width(TermWin *tw, int uc)
{
    int ibuf = 0;

    /* If the font max is the same as the font ave width then this
     * function is a no-op.
     */
    if (!font_dualwidth) return 1;

    switch (uc & CSET_MASK) {
      case CSET_ASCII:
        uc = ucsdata.unitab_line[uc & 0xFF];
        break;
      case CSET_LINEDRW:
        uc = ucsdata.unitab_xterm[uc & 0xFF];
        break;
      case CSET_SCOACS:
        uc = ucsdata.unitab_scoacs[uc & 0xFF];
        break;
    }
    if (DIRECT_FONT(uc)) {
        if (ucsdata.dbcs_screenfont) return 1;

        /* Speedup, I know of no font where ascii is the wrong width */
        if ((uc&~CSET_MASK) >= ' ' && (uc&~CSET_MASK)<= '~')
            return 1;

        if ( (uc & CSET_MASK) == CSET_ACP ) {
            SelectObject(wintw_hdc, fonts[FONT_NORMAL]);
        } else if ( (uc & CSET_MASK) == CSET_OEMCP ) {
            another_font(FONT_OEM);
            if (!fonts[FONT_OEM]) return 0;

            SelectObject(wintw_hdc, fonts[FONT_OEM]);
        } else
            return 0;

        if (GetCharWidth32(wintw_hdc, uc & ~CSET_MASK,
                           uc & ~CSET_MASK, &ibuf) != 1 &&
            GetCharWidth(wintw_hdc, uc & ~CSET_MASK,
                         uc & ~CSET_MASK, &ibuf) != 1)
            return 0;
    } else {
        /* Speedup, I know of no font where ascii is the wrong width */
        if (uc >= ' ' && uc <= '~') return 1;

        SelectObject(wintw_hdc, fonts[FONT_NORMAL]);
        if (GetCharWidth32W(wintw_hdc, uc, uc, &ibuf) == 1)
            /* Okay that one worked */ ;
        else if (GetCharWidthW(wintw_hdc, uc, uc, &ibuf) == 1)
            /* This should work on 9x too, but it's "less accurate" */ ;
        else
            return 0;
    }

    ibuf += font_width / 2 -1;
    ibuf /= font_width;

    return ibuf;
}

DECL_WINDOWS_FUNCTION(static, BOOL, FlashWindowEx, (PFLASHWINFO));
DECL_WINDOWS_FUNCTION(static, BOOL, ToUnicodeEx,
                      (UINT, UINT, const BYTE *, LPWSTR, int, UINT, HKL));
DECL_WINDOWS_FUNCTION(static, BOOL, PlaySound, (LPCTSTR, HMODULE, DWORD));

static void init_winfuncs(void)
{
    HMODULE user32_module = load_system32_dll("user32.dll");
    HMODULE winmm_module = load_system32_dll("winmm.dll");
    GET_WINDOWS_FUNCTION(user32_module, FlashWindowEx);
    GET_WINDOWS_FUNCTION(user32_module, ToUnicodeEx);
    GET_WINDOWS_FUNCTION_PP(winmm_module, PlaySound);
}

/*
 * Translate a WM_(SYS)?KEY(UP|DOWN) message into a string of ASCII
 * codes. Returns number of bytes used, zero to drop the message,
 * -1 to forward the message to Windows, or another negative number
 * to indicate a NUL-terminated "special" string.
 */
static int TranslateKey(UINT message, WPARAM wParam, LPARAM lParam,
                        unsigned char *output)
{
    BYTE keystate[256];
    int scan, shift_state;
    bool left_alt = false, key_down;
    int r, i;
    unsigned char *p = output;
    static int alt_sum = 0;
    int funky_type = conf_get_int(conf, CONF_funky_type);
    bool no_applic_k = conf_get_bool(conf, CONF_no_applic_k);
    bool ctrlaltkeys = conf_get_bool(conf, CONF_ctrlaltkeys);
    bool nethack_keypad = conf_get_bool(conf, CONF_nethack_keypad);
    char keypad_key = '\0';

    HKL kbd_layout = GetKeyboardLayout(0);

    static wchar_t keys_unicode[3];
    static int compose_char = 0;
    static WPARAM compose_keycode = 0;

    r = GetKeyboardState(keystate);
    if (!r)
        memset(keystate, 0, sizeof(keystate));
    else {
#if 0
#define SHOW_TOASCII_RESULT
        {                              /* Tell us all about key events */
            static BYTE oldstate[256];
            static int first = 1;
            static int scan;
            int ch;
            if (first)
                memcpy(oldstate, keystate, sizeof(oldstate));
            first = 0;

            if ((HIWORD(lParam) & (KF_UP | KF_REPEAT)) == KF_REPEAT) {
                debug("+");
            } else if ((HIWORD(lParam) & KF_UP)
                       && scan == (HIWORD(lParam) & 0xFF)) {
                debug(". U");
            } else {
                debug(".\n");
                if (wParam >= VK_F1 && wParam <= VK_F20)
                    debug("K_F%d", wParam + 1 - VK_F1);
                else
                    switch (wParam) {
                      case VK_SHIFT:
                        debug("SHIFT");
                        break;
                      case VK_CONTROL:
                        debug("CTRL");
                        break;
                      case VK_MENU:
                        debug("ALT");
                        break;
                      default:
                        debug("VK_%02x", wParam);
                    }
                if (message == WM_SYSKEYDOWN || message == WM_SYSKEYUP)
                    debug("*");
                debug(", S%02x", scan = (HIWORD(lParam) & 0xFF));

                ch = MapVirtualKeyEx(wParam, 2, kbd_layout);
                if (ch >= ' ' && ch <= '~')
                    debug(", '%c'", ch);
                else if (ch)
                    debug(", $%02x", ch);

                if (keys_unicode[0])
                    debug(", KB0=%04x", keys_unicode[0]);
                if (keys_unicode[1])
                    debug(", KB1=%04x", keys_unicode[1]);
                if (keys_unicode[2])
                    debug(", KB2=%04x", keys_unicode[2]);

                if ((keystate[VK_SHIFT] & 0x80) != 0)
                    debug(", S");
                if ((keystate[VK_CONTROL] & 0x80) != 0)
                    debug(", C");
                if ((HIWORD(lParam) & KF_EXTENDED))
                    debug(", E");
                if ((HIWORD(lParam) & KF_UP))
                    debug(", U");
            }

            if ((HIWORD(lParam) & (KF_UP | KF_REPEAT)) == KF_REPEAT);
            else if ((HIWORD(lParam) & KF_UP))
                oldstate[wParam & 0xFF] ^= 0x80;
            else
                oldstate[wParam & 0xFF] ^= 0x81;

            for (ch = 0; ch < 256; ch++)
                if (oldstate[ch] != keystate[ch])
                    debug(", M%02x=%02x", ch, keystate[ch]);

            memcpy(oldstate, keystate, sizeof(oldstate));
        }
#endif

        if (wParam == VK_MENU && (HIWORD(lParam) & KF_EXTENDED)) {
            keystate[VK_RMENU] = keystate[VK_MENU];
        }


        /* Nastyness with NUMLock - Shift-NUMLock is left alone though */
        if ((funky_type == FUNKY_VT400 ||
             (funky_type <= FUNKY_LINUX && term->app_keypad_keys &&
              !no_applic_k))
            && wParam == VK_NUMLOCK && !(keystate[VK_SHIFT] & 0x80)) {

            wParam = VK_EXECUTE;

            /* UnToggle NUMLock */
            if ((HIWORD(lParam) & (KF_UP | KF_REPEAT)) == 0)
                keystate[VK_NUMLOCK] ^= 1;
        }

        /* And write back the 'adjusted' state */
        SetKeyboardState(keystate);
    }

    /* Disable Auto repeat if required */
    if (term->repeat_off &&
        (HIWORD(lParam) & (KF_UP | KF_REPEAT)) == KF_REPEAT)
        return 0;

    if ((HIWORD(lParam) & KF_ALTDOWN) && (keystate[VK_RMENU] & 0x80) == 0)
        left_alt = true;

    key_down = ((HIWORD(lParam) & KF_UP) == 0);

    /* Make sure Ctrl-ALT is not the same as AltGr for ToAscii unless told. */
    if (left_alt && (keystate[VK_CONTROL] & 0x80)) {
        if (ctrlaltkeys)
            keystate[VK_MENU] = 0;
        else {
            keystate[VK_RMENU] = 0x80;
            left_alt = false;
        }
    }

    scan = (HIWORD(lParam) & (KF_UP | KF_EXTENDED | 0xFF));
    shift_state = ((keystate[VK_SHIFT] & 0x80) != 0)
        + ((keystate[VK_CONTROL] & 0x80) != 0) * 2;

    /* Note if AltGr was pressed and if it was used as a compose key */
    if (!compose_state) {
        compose_keycode = 0x100;
        if (conf_get_bool(conf, CONF_compose_key)) {
            if (wParam == VK_MENU && (HIWORD(lParam) & KF_EXTENDED))
                compose_keycode = wParam;
        }
        if (wParam == VK_APPS)
            compose_keycode = wParam;
    }

    if (wParam == compose_keycode) {
        if (compose_state == 0
            && (HIWORD(lParam) & (KF_UP | KF_REPEAT)) == 0) compose_state =
                1;
        else if (compose_state == 1 && (HIWORD(lParam) & KF_UP))
            compose_state = 2;
        else
            compose_state = 0;
    } else if (compose_state == 1 && wParam != VK_CONTROL)
        compose_state = 0;

    if (compose_state > 1 && left_alt)
        compose_state = 0;

    /* Sanitize the number pad if not using a PC NumPad */
    if (left_alt || (term->app_keypad_keys && !no_applic_k
                     && funky_type != FUNKY_XTERM)
        || funky_type == FUNKY_VT400 || nethack_keypad || compose_state) {
        if ((HIWORD(lParam) & KF_EXTENDED) == 0) {
            int nParam = 0;
            switch (wParam) {
              case VK_INSERT:
                nParam = VK_NUMPAD0;
                break;
              case VK_END:
                nParam = VK_NUMPAD1;
                break;
              case VK_DOWN:
                nParam = VK_NUMPAD2;
                break;
              case VK_NEXT:
                nParam = VK_NUMPAD3;
                break;
              case VK_LEFT:
                nParam = VK_NUMPAD4;
                break;
              case VK_CLEAR:
                nParam = VK_NUMPAD5;
                break;
              case VK_RIGHT:
                nParam = VK_NUMPAD6;
                break;
              case VK_HOME:
                nParam = VK_NUMPAD7;
                break;
              case VK_UP:
                nParam = VK_NUMPAD8;
                break;
              case VK_PRIOR:
                nParam = VK_NUMPAD9;
                break;
              case VK_DELETE:
                nParam = VK_DECIMAL;
                break;
            }
            if (nParam) {
                if (keystate[VK_NUMLOCK] & 1)
                    shift_state |= 1;
                wParam = nParam;
            }
        }
    }

    /* If a key is pressed and AltGr is not active */
    if (key_down && (keystate[VK_RMENU] & 0x80) == 0 && !compose_state) {
        /* Okay, prepare for most alts then ... */
        if (left_alt)
            *p++ = '\033';

        /* Lets see if it's a pattern we know all about ... */
        if (wParam == VK_PRIOR && shift_state == 1) {
            SendMessage(hwnd, WM_VSCROLL, SB_PAGEUP, 0);
            return 0;
        }
        if (wParam == VK_PRIOR && shift_state == 3) { /* ctrl-shift-pageup */
            SendMessage(hwnd, WM_VSCROLL, SB_TOP, 0);
            return 0;
        }
        if (wParam == VK_NEXT && shift_state == 3) { /* ctrl-shift-pagedown */
            SendMessage(hwnd, WM_VSCROLL, SB_BOTTOM, 0);
            return 0;
        }

        if (wParam == VK_PRIOR && shift_state == 2) {
            SendMessage(hwnd, WM_VSCROLL, SB_LINEUP, 0);
            return 0;
        }
        if (wParam == VK_NEXT && shift_state == 1) {
            SendMessage(hwnd, WM_VSCROLL, SB_PAGEDOWN, 0);
            return 0;
        }
        if (wParam == VK_NEXT && shift_state == 2) {
            SendMessage(hwnd, WM_VSCROLL, SB_LINEDOWN, 0);
            return 0;
        }
        if ((wParam == VK_PRIOR || wParam == VK_NEXT) && shift_state == 3) {
            term_scroll_to_selection(term, (wParam == VK_PRIOR ? 0 : 1));
            return 0;
        }
        if (wParam == VK_INSERT && shift_state == 2) {
            switch (conf_get_int(conf, CONF_ctrlshiftins)) {
              case CLIPUI_IMPLICIT:
                break;          /* no need to re-copy to CLIP_LOCAL */
              case CLIPUI_EXPLICIT:
                term_request_copy(term, clips_system, lenof(clips_system));
                break;
              default:
                break;
            }
            return 0;
        }
        if (wParam == VK_INSERT && shift_state == 1) {
            switch (conf_get_int(conf, CONF_ctrlshiftins)) {
              case CLIPUI_IMPLICIT:
                term_request_paste(term, CLIP_LOCAL);
                break;
              case CLIPUI_EXPLICIT:
                term_request_paste(term, CLIP_SYSTEM);
                break;
              default:
                break;
            }
            return 0;
        }
        if (wParam == 'C' && shift_state == 3) {
            switch (conf_get_int(conf, CONF_ctrlshiftcv)) {
              case CLIPUI_IMPLICIT:
                break;          /* no need to re-copy to CLIP_LOCAL */
              case CLIPUI_EXPLICIT:
                term_request_copy(term, clips_system, lenof(clips_system));
                break;
              default:
                break;
            }
            return 0;
        }
        if (wParam == 'V' && shift_state == 3) {
            switch (conf_get_int(conf, CONF_ctrlshiftcv)) {
              case CLIPUI_IMPLICIT:
                term_request_paste(term, CLIP_LOCAL);
                break;
              case CLIPUI_EXPLICIT:
                term_request_paste(term, CLIP_SYSTEM);
                break;
              default:
                break;
            }
            return 0;
        }
        if (left_alt && wParam == VK_F4 && conf_get_bool(conf, CONF_alt_f4)) {
            return -1;
        }
        if (left_alt && wParam == VK_SPACE && conf_get_bool(conf,
                                                            CONF_alt_space)) {
            SendMessage(hwnd, WM_SYSCOMMAND, SC_KEYMENU, 0);
            return -1;
        }
        if (left_alt && wParam == VK_RETURN &&
            conf_get_bool(conf, CONF_fullscreenonaltenter) &&
            (conf_get_int(conf, CONF_resize_action) != RESIZE_DISABLED)) {
            if ((HIWORD(lParam) & (KF_UP | KF_REPEAT)) != KF_REPEAT)
                flip_full_screen();
            return -1;
        }
        /* Control-Numlock for app-keypad mode switch */
        if (wParam == VK_PAUSE && shift_state == 2) {
            term->app_keypad_keys = !term->app_keypad_keys;
            return 0;
        }

        if (wParam == VK_BACK && shift_state == 0) {    /* Backspace */
            *p++ = (conf_get_bool(conf, CONF_bksp_is_delete) ? 0x7F : 0x08);
            *p++ = 0;
            return -2;
        }
        if (wParam == VK_BACK && shift_state == 1) {    /* Shift Backspace */
            /* We do the opposite of what is configured */
            *p++ = (conf_get_bool(conf, CONF_bksp_is_delete) ? 0x08 : 0x7F);
            *p++ = 0;
            return -2;
        }
        if (wParam == VK_TAB && shift_state == 1) {     /* Shift tab */
            *p++ = 0x1B;
            *p++ = '[';
            *p++ = 'Z';
            return p - output;
        }
        if (wParam == VK_SPACE && shift_state == 2) {   /* Ctrl-Space */
            *p++ = 0;
            return p - output;
        }
        if (wParam == VK_SPACE && shift_state == 3) {   /* Ctrl-Shift-Space */
            *p++ = 160;
            return p - output;
        }
        if (wParam == VK_CANCEL && shift_state == 2) {  /* Ctrl-Break */
            if (backend)
                backend_special(backend, SS_BRK, 0);
            return 0;
        }
        if (wParam == VK_PAUSE) {      /* Break/Pause */
            *p++ = 26;
            *p++ = 0;
            return -2;
        }
        /* Control-2 to Control-8 are special */
        if (shift_state == 2 && wParam >= '2' && wParam <= '8') {
            *p++ = "\000\033\034\035\036\037\177"[wParam - '2'];
            return p - output;
        }
        if (shift_state == 2 && (wParam == 0xBD || wParam == 0xBF)) {
            *p++ = 0x1F;
            return p - output;
        }
        if (shift_state == 2 && (wParam == 0xDF || wParam == 0xDC)) {
            *p++ = 0x1C;
            return p - output;
        }
        if (shift_state == 3 && wParam == 0xDE) {
            *p++ = 0x1E;               /* Ctrl-~ == Ctrl-^ in xterm at least */
            return p - output;
        }

        switch (wParam) {
          case VK_NUMPAD0: keypad_key = '0'; goto numeric_keypad;
          case VK_NUMPAD1: keypad_key = '1'; goto numeric_keypad;
          case VK_NUMPAD2: keypad_key = '2'; goto numeric_keypad;
          case VK_NUMPAD3: keypad_key = '3'; goto numeric_keypad;
          case VK_NUMPAD4: keypad_key = '4'; goto numeric_keypad;
          case VK_NUMPAD5: keypad_key = '5'; goto numeric_keypad;
          case VK_NUMPAD6: keypad_key = '6'; goto numeric_keypad;
          case VK_NUMPAD7: keypad_key = '7'; goto numeric_keypad;
          case VK_NUMPAD8: keypad_key = '8'; goto numeric_keypad;
          case VK_NUMPAD9: keypad_key = '9'; goto numeric_keypad;
          case VK_DECIMAL: keypad_key = '.'; goto numeric_keypad;
          case VK_ADD: keypad_key = '+'; goto numeric_keypad;
          case VK_SUBTRACT: keypad_key = '-'; goto numeric_keypad;
          case VK_MULTIPLY: keypad_key = '*'; goto numeric_keypad;
          case VK_DIVIDE: keypad_key = '/'; goto numeric_keypad;
          case VK_EXECUTE: keypad_key = 'G'; goto numeric_keypad;
            /* also the case for VK_RETURN below can sometimes come here */
          numeric_keypad:
            /* Left Alt overrides all numeric keypad usage to act as
             * numeric character code input */
            if (left_alt) {
                if (keypad_key >= '0' && keypad_key <= '9')
                    alt_sum = alt_sum * 10 + keypad_key - '0';
                else
                    alt_sum = 0;
                break;
            }

            {
                int nchars = format_numeric_keypad_key(
                    (char *)p, term, keypad_key,
                    shift_state & 1, shift_state & 2);
                if (!nchars) {
                    /*
                     * If we didn't get an escape sequence out of the
                     * numeric keypad key, then that must be because
                     * we're in Num Lock mode without application
                     * keypad enabled. In that situation we leave this
                     * keypress to the ToUnicode/ToAsciiEx handler
                     * below, which will translate it according to the
                     * appropriate keypad layout (e.g. so that what a
                     * Brit thinks of as keypad '.' can become ',' in
                     * the German layout).
                     *
                     * An exception is the keypad Return key: if we
                     * didn't get an escape sequence for that, we
                     * treat it like ordinary Return, taking into
                     * account Telnet special new line codes and
                     * config options.
                     */
                    if (keypad_key == '\r')
                        goto ordinary_return_key;
                    break;
                }

                p += nchars;
                return p - output;
            }

            int fkey_number;
          case VK_F1: fkey_number = 1; goto numbered_function_key;
          case VK_F2: fkey_number = 2; goto numbered_function_key;
          case VK_F3: fkey_number = 3; goto numbered_function_key;
          case VK_F4: fkey_number = 4; goto numbered_function_key;
          case VK_F5: fkey_number = 5; goto numbered_function_key;
          case VK_F6: fkey_number = 6; goto numbered_function_key;
          case VK_F7: fkey_number = 7; goto numbered_function_key;
          case VK_F8: fkey_number = 8; goto numbered_function_key;
          case VK_F9: fkey_number = 9; goto numbered_function_key;
          case VK_F10: fkey_number = 10; goto numbered_function_key;
          case VK_F11: fkey_number = 11; goto numbered_function_key;
          case VK_F12: fkey_number = 12; goto numbered_function_key;
          case VK_F13: fkey_number = 13; goto numbered_function_key;
          case VK_F14: fkey_number = 14; goto numbered_function_key;
          case VK_F15: fkey_number = 15; goto numbered_function_key;
          case VK_F16: fkey_number = 16; goto numbered_function_key;
          case VK_F17: fkey_number = 17; goto numbered_function_key;
          case VK_F18: fkey_number = 18; goto numbered_function_key;
          case VK_F19: fkey_number = 19; goto numbered_function_key;
          case VK_F20: fkey_number = 20; goto numbered_function_key;
          numbered_function_key:
            p += format_function_key((char *)p, term, fkey_number,
                                     shift_state & 1, shift_state & 2);
            return p - output;

            SmallKeypadKey sk_key;
          case VK_HOME: sk_key = SKK_HOME; goto small_keypad_key;
          case VK_END: sk_key = SKK_END; goto small_keypad_key;
          case VK_INSERT: sk_key = SKK_INSERT; goto small_keypad_key;
          case VK_DELETE: sk_key = SKK_DELETE; goto small_keypad_key;
          case VK_PRIOR: sk_key = SKK_PGUP; goto small_keypad_key;
          case VK_NEXT: sk_key = SKK_PGDN; goto small_keypad_key;
          small_keypad_key:
            /* These keys don't generate terminal input with Ctrl */
            if (shift_state & 2)
                break;

            p += format_small_keypad_key((char *)p, term, sk_key);
            return p - output;

            char xkey;
          case VK_UP: xkey = 'A'; goto arrow_key;
          case VK_DOWN: xkey = 'B'; goto arrow_key;
          case VK_RIGHT: xkey = 'C'; goto arrow_key;
          case VK_LEFT: xkey = 'D'; goto arrow_key;
          case VK_CLEAR: xkey = 'G'; goto arrow_key; /* close enough */
          arrow_key:
            p += format_arrow_key((char *)p, term, xkey, shift_state & 2);
            return p - output;

          case VK_RETURN:
            if (HIWORD(lParam) & KF_EXTENDED) {
                keypad_key = '\r';
                goto numeric_keypad;
            }
          ordinary_return_key:
            if (shift_state == 0 && term->cr_lf_return) {
                *p++ = '\r';
                *p++ = '\n';
                return p - output;
            } else {
                *p++ = 0x0D;
                *p++ = 0;
                return -2;
            }
        }
    }

    /* Okay we've done everything interesting; let windows deal with
     * the boring stuff */
    {
        bool capsOn = false;

        /* helg: clear CAPS LOCK state if caps lock switches to cyrillic */
        if(keystate[VK_CAPITAL] != 0 &&
           conf_get_bool(conf, CONF_xlat_capslockcyr)) {
            capsOn= !left_alt;
            keystate[VK_CAPITAL] = 0;
        }

        /* XXX how do we know what the max size of the keys array should
         * be is? There's indication on MS' website of an Inquire/InquireEx
         * functioning returning a KBINFO structure which tells us. */
        if (osPlatformId == VER_PLATFORM_WIN32_NT && p_ToUnicodeEx) {
            r = p_ToUnicodeEx(wParam, scan, keystate, keys_unicode,
                              lenof(keys_unicode), 0, kbd_layout);
        } else {
            /* XXX 'keys' parameter is declared in MSDN documentation as
             * 'LPWORD lpChar'.
             * The experience of a French user indicates that on
             * Win98, WORD[] should be passed in, but on Win2K, it should
             * be BYTE[]. German WinXP and my Win2K with "US International"
             * driver corroborate this.
             * Experimentally I've conditionalised the behaviour on the
             * Win9x/NT split, but I suspect it's worse than that.
             * See wishlist item `win-dead-keys' for more horrible detail
             * and speculations. */
            int i;
            static WORD keys[3];
            static BYTE keysb[3];
            r = ToAsciiEx(wParam, scan, keystate, keys, 0, kbd_layout);
            if (r > 0) {
                for (i = 0; i < r; i++) {
                    keysb[i] = (BYTE)keys[i];
                }
                MultiByteToWideChar(CP_ACP, 0, (LPCSTR)keysb, r,
                                    keys_unicode, lenof(keys_unicode));
            }
        }
#ifdef SHOW_TOASCII_RESULT
        if (r == 1 && !key_down) {
            if (alt_sum) {
                if (in_utf(term) || ucsdata.dbcs_screenfont)
                    debug(", (U+%04x)", alt_sum);
                else
                    debug(", LCH(%d)", alt_sum);
            } else {
                debug(", ACH(%d)", keys_unicode[0]);
            }
        } else if (r > 0) {
            int r1;
            debug(", ASC(");
            for (r1 = 0; r1 < r; r1++) {
                debug("%s%d", r1 ? "," : "", keys_unicode[r1]);
            }
            debug(")");
        }
#endif
        if (r > 0) {
            WCHAR keybuf;

            p = output;
            for (i = 0; i < r; i++) {
                wchar_t wch = keys_unicode[i];

                if (compose_state == 2 && wch >= ' ' && wch < 0x80) {
                    compose_char = wch;
                    compose_state++;
                    continue;
                }
                if (compose_state == 3 && wch >= ' ' && wch < 0x80) {
                    int nc;
                    compose_state = 0;

                    if ((nc = check_compose(compose_char, wch)) == -1) {
                        MessageBeep(MB_ICONHAND);
                        return 0;
                    }
                    keybuf = nc;
                    term_keyinputw(term, &keybuf, 1);
                    continue;
                }

                compose_state = 0;

                if (!key_down) {
                    if (alt_sum) {
                        if (in_utf(term) || ucsdata.dbcs_screenfont) {
                            keybuf = alt_sum;
                            term_keyinputw(term, &keybuf, 1);
                        } else {
                            char ch = (char) alt_sum;
                            /*
                             * We need not bother about stdin
                             * backlogs here, because in GUI PuTTY
                             * we can't do anything about it
                             * anyway; there's no means of asking
                             * Windows to hold off on KEYDOWN
                             * messages. We _have_ to buffer
                             * everything we're sent.
                             */
                            term_keyinput(term, -1, &ch, 1);
                        }
                        alt_sum = 0;
                    } else {
                        term_keyinputw(term, &wch, 1);
                    }
                } else {
                    if(capsOn && wch < 0x80) {
                        WCHAR cbuf[2];
                        cbuf[0] = 27;
                        cbuf[1] = xlat_uskbd2cyrllic(wch);
                        term_keyinputw(term, cbuf+!left_alt, 1+!!left_alt);
                    } else {
                        WCHAR cbuf[2];
                        cbuf[0] = '\033';
                        cbuf[1] = wch;
                        term_keyinputw(term, cbuf +!left_alt, 1+!!left_alt);
                    }
                }
                show_mouseptr(false);
            }

            /* This is so the ALT-Numpad and dead keys work correctly. */
            keys_unicode[0] = 0;

            return p - output;
        }
        /* If we're definitely not building up an ALT-54321 then clear it */
        if (!left_alt)
            keys_unicode[0] = 0;
        /* If we will be using alt_sum fix the 256s */
        else if (keys_unicode[0] && (in_utf(term) || ucsdata.dbcs_screenfont))
            keys_unicode[0] = 10;
    }

    /*
     * ALT alone may or may not want to bring up the System menu.
     * If it's not meant to, we return 0 on presses or releases of
     * ALT, to show that we've swallowed the keystroke. Otherwise
     * we return -1, which means Windows will give the keystroke
     * its default handling (i.e. bring up the System menu).
     */
    if (wParam == VK_MENU && !conf_get_bool(conf, CONF_alt_only))
        return 0;

    return -1;
}

static void wintw_set_title(TermWin *tw, const char *title)
{
    sfree(window_name);
    window_name = snewn(1 + strlen(title), char);
    strcpy(window_name, title);
    if (conf_get_bool(conf, CONF_win_name_always) || !IsIconic(hwnd))
        SetWindowText(hwnd, title);
}

static void wintw_set_icon_title(TermWin *tw, const char *title)
{
    sfree(icon_name);
    icon_name = snewn(1 + strlen(title), char);
    strcpy(icon_name, title);
    if (!conf_get_bool(conf, CONF_win_name_always) && IsIconic(hwnd))
        SetWindowText(hwnd, title);
}

static void wintw_set_scrollbar(TermWin *tw, int total, int start, int page)
{
    SCROLLINFO si;

    if (!conf_get_bool(conf, is_full_screen() ?
                       CONF_scrollbar_in_fullscreen : CONF_scrollbar))
        return;

    si.cbSize = sizeof(si);
    si.fMask = SIF_ALL | SIF_DISABLENOSCROLL;
    si.nMin = 0;
    si.nMax = total - 1;
    si.nPage = page;
    si.nPos = start;
    if (hwnd)
        SetScrollInfo(hwnd, SB_VERT, &si, true);
}

static bool wintw_setup_draw_ctx(TermWin *tw)
{
    assert(!wintw_hdc);
    wintw_hdc = make_hdc();
    return wintw_hdc != NULL;
}

static void wintw_free_draw_ctx(TermWin *tw)
{
    assert(wintw_hdc);
    free_hdc(wintw_hdc);
    wintw_hdc = NULL;
}

static void real_palette_set(int n, int r, int g, int b)
{
    internal_set_colour(n, r, g, b);
    if (pal) {
        logpal->palPalEntry[n].peRed = r;
        logpal->palPalEntry[n].peGreen = g;
        logpal->palPalEntry[n].peBlue = b;
        logpal->palPalEntry[n].peFlags = PC_NOCOLLAPSE;
        SetPaletteEntries(pal, 0, NALLCOLOURS, logpal->palPalEntry);
    }
}

static bool wintw_palette_get(TermWin *tw, int n, int *r, int *g, int *b)
{
    if (n < 0 || n >= NALLCOLOURS)
        return false;
    *r = colours_rgb[n].r;
    *g = colours_rgb[n].g;
    *b = colours_rgb[n].b;
    return true;
}

static void wintw_palette_set(TermWin *tw, int n, int r, int g, int b)
{
    if (n >= 16)
        n += 256 - 16;
    if (n >= NALLCOLOURS)
        return;
    real_palette_set(n, r, g, b);
    if (pal) {
        HDC hdc = make_hdc();
        UnrealizeObject(pal);
        RealizePalette(hdc);
        free_hdc(hdc);
    } else {
        if (n == (ATTR_DEFBG>>ATTR_BGSHIFT))
            /* If Default Background changes, we need to ensure any
             * space between the text area and the window border is
             * redrawn. */
            InvalidateRect(hwnd, NULL, true);
    }
}

static void wintw_palette_reset(TermWin *tw)
{
    int i;

    /* And this */
    for (i = 0; i < NALLCOLOURS; i++) {
        internal_set_colour(i, defpal[i].rgbtRed,
                            defpal[i].rgbtGreen, defpal[i].rgbtBlue);
        if (pal) {
            logpal->palPalEntry[i].peRed = defpal[i].rgbtRed;
            logpal->palPalEntry[i].peGreen = defpal[i].rgbtGreen;
            logpal->palPalEntry[i].peBlue = defpal[i].rgbtBlue;
            logpal->palPalEntry[i].peFlags = 0;
        }
    }

    if (pal) {
        HDC hdc;
        SetPaletteEntries(pal, 0, NALLCOLOURS, logpal->palPalEntry);
        hdc = make_hdc();
        RealizePalette(hdc);
        free_hdc(hdc);
    } else {
        /* Default Background may have changed. Ensure any space between
         * text area and window border is redrawn. */
        InvalidateRect(hwnd, NULL, true);
    }
}

void write_aclip(int clipboard, char *data, int len, bool must_deselect)
{
    HGLOBAL clipdata;
    void *lock;

    if (clipboard != CLIP_SYSTEM)
        return;

    clipdata = GlobalAlloc(GMEM_DDESHARE | GMEM_MOVEABLE, len + 1);
    if (!clipdata)
        return;
    lock = GlobalLock(clipdata);
    if (!lock)
        return;
    memcpy(lock, data, len);
    ((unsigned char *) lock)[len] = 0;
    GlobalUnlock(clipdata);

    if (!must_deselect)
        SendMessage(hwnd, WM_IGNORE_CLIP, true, 0);

    if (OpenClipboard(hwnd)) {
        EmptyClipboard();
        SetClipboardData(CF_TEXT, clipdata);
        CloseClipboard();
    } else
        GlobalFree(clipdata);

    if (!must_deselect)
        SendMessage(hwnd, WM_IGNORE_CLIP, false, 0);
}

typedef struct _rgbindex {
    int index;
    COLORREF ref;
} rgbindex;

int cmpCOLORREF(void *va, void *vb)
{
    COLORREF a = ((rgbindex *)va)->ref;
    COLORREF b = ((rgbindex *)vb)->ref;
    return (a < b) ? -1 : (a > b) ? +1 : 0;
}

/*
 * Note: unlike write_aclip() this will not append a nul.
 */
static void wintw_clip_write(
    TermWin *tw, int clipboard, wchar_t *data, int *attr,
    truecolour *truecolour, int len, bool must_deselect)
{
    HGLOBAL clipdata, clipdata2, clipdata3;
    int len2;
    void *lock, *lock2, *lock3;

    if (clipboard != CLIP_SYSTEM)
        return;

    len2 = WideCharToMultiByte(CP_ACP, 0, data, len, 0, 0, NULL, NULL);

    clipdata = GlobalAlloc(GMEM_DDESHARE | GMEM_MOVEABLE,
                           len * sizeof(wchar_t));
    clipdata2 = GlobalAlloc(GMEM_DDESHARE | GMEM_MOVEABLE, len2);

    if (!clipdata || !clipdata2) {
        if (clipdata)
            GlobalFree(clipdata);
        if (clipdata2)
            GlobalFree(clipdata2);
        return;
    }
    if (!(lock = GlobalLock(clipdata))) {
        GlobalFree(clipdata);
        GlobalFree(clipdata2);
        return;
    }
    if (!(lock2 = GlobalLock(clipdata2))) {
        GlobalUnlock(clipdata);
        GlobalFree(clipdata);
        GlobalFree(clipdata2);
        return;
    }

    memcpy(lock, data, len * sizeof(wchar_t));
    WideCharToMultiByte(CP_ACP, 0, data, len, lock2, len2, NULL, NULL);

    if (conf_get_bool(conf, CONF_rtf_paste)) {
        wchar_t unitab[256];
        strbuf *rtf = strbuf_new();
        unsigned char *tdata = (unsigned char *)lock2;
        wchar_t *udata = (wchar_t *)lock;
        int uindex = 0, tindex = 0;
        int multilen, blen, alen, totallen, i;
        char before[16], after[4];
        int fgcolour,  lastfgcolour  = -1;
        int bgcolour,  lastbgcolour  = -1;
        COLORREF fg,   lastfg = -1;
        COLORREF bg,   lastbg = -1;
        int attrBold,  lastAttrBold  = 0;
        int attrUnder, lastAttrUnder = 0;
        int palette[NALLCOLOURS];
        int numcolours;
        tree234 *rgbtree = NULL;
        FontSpec *font = conf_get_fontspec(conf, CONF_font);

        get_unitab(CP_ACP, unitab, 0);

        strbuf_catf(
            rtf, "{\\rtf1\\ansi\\deff0{\\fonttbl\\f0\\fmodern %s;}\\f0\\fs%d",
            font->name, font->height*2);

        /*
         * Add colour palette
         * {\colortbl ;\red255\green0\blue0;\red0\green0\blue128;}
         */

        /*
         * First - Determine all colours in use
         *    o  Foregound and background colours share the same palette
         */
        if (attr) {
            memset(palette, 0, sizeof(palette));
            for (i = 0; i < (len-1); i++) {
                fgcolour = ((attr[i] & ATTR_FGMASK) >> ATTR_FGSHIFT);
                bgcolour = ((attr[i] & ATTR_BGMASK) >> ATTR_BGSHIFT);

                if (attr[i] & ATTR_REVERSE) {
                    int tmpcolour = fgcolour;   /* Swap foreground and background */
                    fgcolour = bgcolour;
                    bgcolour = tmpcolour;
                }

                if (bold_colours && (attr[i] & ATTR_BOLD)) {
                    if (fgcolour  <   8)        /* ANSI colours */
                        fgcolour +=   8;
                    else if (fgcolour >= 256)   /* Default colours */
                        fgcolour ++;
                }

                if ((attr[i] & ATTR_BLINK)) {
                    if (bgcolour  <   8)        /* ANSI colours */
                        bgcolour +=   8;
                    else if (bgcolour >= 256)   /* Default colours */
                        bgcolour ++;
                }

                palette[fgcolour]++;
                palette[bgcolour]++;
            }

            if (truecolour) {
                rgbtree = newtree234(cmpCOLORREF);
                for (i = 0; i < (len-1); i++) {
                    if (truecolour[i].fg.enabled) {
                        rgbindex *rgbp = snew(rgbindex);
                        rgbp->ref = RGB(truecolour[i].fg.r,
                                        truecolour[i].fg.g,
                                        truecolour[i].fg.b);
                        if (add234(rgbtree, rgbp) != rgbp)
                            sfree(rgbp);
                    }
                    if (truecolour[i].bg.enabled) {
                        rgbindex *rgbp = snew(rgbindex);
                        rgbp->ref = RGB(truecolour[i].bg.r,
                                        truecolour[i].bg.g,
                                        truecolour[i].bg.b);
                        if (add234(rgbtree, rgbp) != rgbp)
                            sfree(rgbp);
                    }
                }
            }

            /*
             * Next - Create a reduced palette
             */
            numcolours = 0;
            for (i = 0; i < NALLCOLOURS; i++) {
                if (palette[i] != 0)
                    palette[i]  = ++numcolours;
            }

            if (rgbtree) {
                rgbindex *rgbp;
                for (i = 0; (rgbp = index234(rgbtree, i)) != NULL; i++)
                    rgbp->index = ++numcolours;
            }

            /*
             * Finally - Write the colour table
             */
            put_datapl(rtf, PTRLEN_LITERAL("{\\colortbl ;"));

            for (i = 0; i < NALLCOLOURS; i++) {
                if (palette[i] != 0) {
                    strbuf_catf(rtf, "\\red%d\\green%d\\blue%d;",
                                defpal[i].rgbtRed, defpal[i].rgbtGreen,
                                defpal[i].rgbtBlue);
                }
            }
            if (rgbtree) {
                rgbindex *rgbp;
                for (i = 0; (rgbp = index234(rgbtree, i)) != NULL; i++)
                    strbuf_catf(rtf, "\\red%d\\green%d\\blue%d;",
                                GetRValue(rgbp->ref), GetGValue(rgbp->ref),
                                GetBValue(rgbp->ref));
            }
            put_datapl(rtf, PTRLEN_LITERAL("}"));
        }

        /*
         * We want to construct a piece of RTF that specifies the
         * same Unicode text. To do this we will read back in
         * parallel from the Unicode data in `udata' and the
         * non-Unicode data in `tdata'. For each character in
         * `tdata' which becomes the right thing in `udata' when
         * looked up in `unitab', we just copy straight over from
         * tdata. For each one that doesn't, we must WCToMB it
         * individually and produce a \u escape sequence.
         *
         * It would probably be more robust to just bite the bullet
         * and WCToMB each individual Unicode character one by one,
         * then MBToWC each one back to see if it was an accurate
         * translation; but that strikes me as a horrifying number
         * of Windows API calls so I want to see if this faster way
         * will work. If it screws up badly we can always revert to
         * the simple and slow way.
         */
        while (tindex < len2 && uindex < len &&
               tdata[tindex] && udata[uindex]) {
            if (tindex + 1 < len2 &&
                tdata[tindex] == '\r' &&
                tdata[tindex+1] == '\n') {
                tindex++;
                uindex++;
            }

            /*
             * Set text attributes
             */
            if (attr) {
                /*
                 * Determine foreground and background colours
                 */
                if (truecolour && truecolour[tindex].fg.enabled) {
                    fgcolour = -1;
                    fg = RGB(truecolour[tindex].fg.r,
                             truecolour[tindex].fg.g,
                             truecolour[tindex].fg.b);
                } else {
                    fgcolour = ((attr[tindex] & ATTR_FGMASK) >> ATTR_FGSHIFT);
                    fg = -1;
                }

                if (truecolour && truecolour[tindex].bg.enabled) {
                    bgcolour = -1;
                    bg = RGB(truecolour[tindex].bg.r,
                             truecolour[tindex].bg.g,
                             truecolour[tindex].bg.b);
                } else {
                    bgcolour = ((attr[tindex] & ATTR_BGMASK) >> ATTR_BGSHIFT);
                    bg = -1;
                }

                if (attr[tindex] & ATTR_REVERSE) {
                    int tmpcolour = fgcolour;       /* Swap foreground and background */
                    fgcolour = bgcolour;
                    bgcolour = tmpcolour;

                    COLORREF tmpref = fg;
                    fg = bg;
                    bg = tmpref;
                }

                if (bold_colours && (attr[tindex] & ATTR_BOLD) && (fgcolour >= 0)) {
                    if (fgcolour  <   8)            /* ANSI colours */
                        fgcolour +=   8;
                    else if (fgcolour >= 256)       /* Default colours */
                        fgcolour ++;
                }

                if ((attr[tindex] & ATTR_BLINK) && (bgcolour >= 0)) {
                    if (bgcolour  <   8)            /* ANSI colours */
                        bgcolour +=   8;
                    else if (bgcolour >= 256)       /* Default colours */
                        bgcolour ++;
                }

                /*
                 * Collect other attributes
                 */
                if (bold_font_mode != BOLD_NONE)
                    attrBold  = attr[tindex] & ATTR_BOLD;
                else
                    attrBold  = 0;

                attrUnder = attr[tindex] & ATTR_UNDER;

                /*
                 * Reverse video
                 *   o  If video isn't reversed, ignore colour attributes for default foregound
                 *      or background.
                 *   o  Special case where bolded text is displayed using the default foregound
                 *      and background colours - force to bolded RTF.
                 */
                if (!(attr[tindex] & ATTR_REVERSE)) {
                    if (bgcolour >= 256)            /* Default color */
                        bgcolour  = -1;             /* No coloring */

                    if (fgcolour >= 256) {          /* Default colour */
                        if (bold_colours && (fgcolour & 1) && bgcolour == -1)
                            attrBold = ATTR_BOLD;   /* Emphasize text with bold attribute */

                        fgcolour  = -1;             /* No coloring */
                    }
                }

                /*
                 * Write RTF text attributes
                 */
                if ((lastfgcolour != fgcolour) || (lastfg != fg)) {
                    lastfgcolour  = fgcolour;
                    lastfg        = fg;
                    if (fg == -1) {
                        strbuf_catf(rtf, "\\cf%d ",
                                    (fgcolour >= 0) ? palette[fgcolour] : 0);
                    } else {
                        rgbindex rgb, *rgbp;
                        rgb.ref = fg;
                        if ((rgbp = find234(rgbtree, &rgb, NULL)) != NULL)
                            strbuf_catf(rtf, "\\cf%d ", rgbp->index);
                    }
                }

                if ((lastbgcolour != bgcolour) || (lastbg != bg)) {
                    lastbgcolour  = bgcolour;
                    lastbg        = bg;
                    if (bg == -1)
                        strbuf_catf(rtf, "\\highlight%d ",
                                    (bgcolour >= 0) ? palette[bgcolour] : 0);
                    else {
                        rgbindex rgb, *rgbp;
                        rgb.ref = bg;
                        if ((rgbp = find234(rgbtree, &rgb, NULL)) != NULL)
                            strbuf_catf(rtf, "\\highlight%d ", rgbp->index);
                    }
                }

                if (lastAttrBold != attrBold) {
                    lastAttrBold  = attrBold;
                    put_datapl(rtf, attrBold ?
                               PTRLEN_LITERAL("\\b ") :
                               PTRLEN_LITERAL("\\b0 "));
                }

                if (lastAttrUnder != attrUnder) {
                    lastAttrUnder  = attrUnder;
                    put_datapl(rtf, attrUnder ?
                               PTRLEN_LITERAL("\\ul ") :
                               PTRLEN_LITERAL("\\ulnone "));
                }
            }

            if (unitab[tdata[tindex]] == udata[uindex]) {
                multilen = 1;
                before[0] = '\0';
                after[0] = '\0';
                blen = alen = 0;
            } else {
                multilen = WideCharToMultiByte(CP_ACP, 0, unitab+uindex, 1,
                                               NULL, 0, NULL, NULL);
                if (multilen != 1) {
                    blen = sprintf(before, "{\\uc%d\\u%d", (int)multilen,
                                   (int)udata[uindex]);
                    alen = 1; strcpy(after, "}");
                } else {
                    blen = sprintf(before, "\\u%d", udata[uindex]);
                    alen = 0; after[0] = '\0';
                }
            }
            assert(tindex + multilen <= len2);
            totallen = blen + alen;
            for (i = 0; i < multilen; i++) {
                if (tdata[tindex+i] == '\\' ||
                    tdata[tindex+i] == '{' ||
                    tdata[tindex+i] == '}')
                    totallen += 2;
                else if (tdata[tindex+i] == 0x0D || tdata[tindex+i] == 0x0A)
                    totallen += 6;     /* \par\r\n */
                else if (tdata[tindex+i] > 0x7E || tdata[tindex+i] < 0x20)
                    totallen += 4;
                else
                    totallen++;
            }

            put_data(rtf, before, blen);
            for (i = 0; i < multilen; i++) {
                if (tdata[tindex+i] == '\\' ||
                    tdata[tindex+i] == '{' ||
                    tdata[tindex+i] == '}') {
                    put_byte(rtf, '\\');
                    put_byte(rtf, tdata[tindex+i]);
                } else if (tdata[tindex+i] == 0x0D || tdata[tindex+i] == 0x0A) {
                    put_datapl(rtf, PTRLEN_LITERAL("\\par\r\n"));
                } else if (tdata[tindex+i] > 0x7E || tdata[tindex+i] < 0x20) {
                    strbuf_catf(rtf, "\\'%02x", tdata[tindex+i]);
                } else {
                    put_byte(rtf, tdata[tindex+i]);
                }
            }
            put_data(rtf, after, alen);

            tindex += multilen;
            uindex++;
        }

        put_datapl(rtf, PTRLEN_LITERAL("}\0\0")); /* Terminate RTF stream */

        clipdata3 = GlobalAlloc(GMEM_DDESHARE | GMEM_MOVEABLE, rtf->len);
        if (clipdata3 && (lock3 = GlobalLock(clipdata3)) != NULL) {
            memcpy(lock3, rtf->u, rtf->len);
            GlobalUnlock(clipdata3);
        }
        strbuf_free(rtf);

        if (rgbtree) {
            rgbindex *rgbp;
            while ((rgbp = delpos234(rgbtree, 0)) != NULL)
                sfree(rgbp);
            freetree234(rgbtree);
        }
    } else
        clipdata3 = NULL;

    GlobalUnlock(clipdata);
    GlobalUnlock(clipdata2);

    if (!must_deselect)
        SendMessage(hwnd, WM_IGNORE_CLIP, true, 0);

    if (OpenClipboard(hwnd)) {
        EmptyClipboard();
        SetClipboardData(CF_UNICODETEXT, clipdata);
        SetClipboardData(CF_TEXT, clipdata2);
        if (clipdata3)
            SetClipboardData(RegisterClipboardFormat(CF_RTF), clipdata3);
        CloseClipboard();
    } else {
        GlobalFree(clipdata);
        GlobalFree(clipdata2);
    }

    if (!must_deselect)
        SendMessage(hwnd, WM_IGNORE_CLIP, false, 0);
}

static DWORD WINAPI clipboard_read_threadfunc(void *param)
{
    HWND hwnd = (HWND)param;
    HGLOBAL clipdata;

    if (OpenClipboard(NULL)) {
        if ((clipdata = GetClipboardData(CF_UNICODETEXT))) {
            SendMessage(hwnd, WM_GOT_CLIPDATA,
                        (WPARAM)true, (LPARAM)clipdata);
        } else if ((clipdata = GetClipboardData(CF_TEXT))) {
            SendMessage(hwnd, WM_GOT_CLIPDATA,
                        (WPARAM)false, (LPARAM)clipdata);
        }
        CloseClipboard();
    }

    return 0;
}

static void process_clipdata(HGLOBAL clipdata, bool unicode)
{
    wchar_t *clipboard_contents = NULL;
    size_t clipboard_length = 0;

    if (unicode) {
        wchar_t *p = GlobalLock(clipdata);
        wchar_t *p2;

        if (p) {
            /* Unwilling to rely on Windows having wcslen() */
            for (p2 = p; *p2; p2++);
            clipboard_length = p2 - p;
            clipboard_contents = snewn(clipboard_length + 1, wchar_t);
            memcpy(clipboard_contents, p, clipboard_length * sizeof(wchar_t));
            clipboard_contents[clipboard_length] = L'\0';
            term_do_paste(term, clipboard_contents, clipboard_length);
        }
    } else {
        char *s = GlobalLock(clipdata);
        int i;

        if (s) {
            i = MultiByteToWideChar(CP_ACP, 0, s, strlen(s) + 1, 0, 0);
            clipboard_contents = snewn(i, wchar_t);
            MultiByteToWideChar(CP_ACP, 0, s, strlen(s) + 1,
                                clipboard_contents, i);
            clipboard_length = i - 1;
            clipboard_contents[clipboard_length] = L'\0';
            term_do_paste(term, clipboard_contents, clipboard_length);
        }
    }

    sfree(clipboard_contents);
}

static void wintw_clip_request_paste(TermWin *tw, int clipboard)
{
    assert(clipboard == CLIP_SYSTEM);

    /*
     * I always thought pasting was synchronous in Windows; the
     * clipboard access functions certainly _look_ synchronous,
     * unlike the X ones. But in fact it seems that in some
     * situations the contents of the clipboard might not be
     * immediately available, and the clipboard-reading functions
     * may block. This leads to trouble if the application
     * delivering the clipboard data has to get hold of it by -
     * for example - talking over a network connection which is
     * forwarded through this very PuTTY.
     *
     * Hence, we spawn a subthread to read the clipboard, and do
     * our paste when it's finished. The thread will send a
     * message back to our main window when it terminates, and
     * that tells us it's OK to paste.
     */
    DWORD in_threadid; /* required for Win9x */
    CreateThread(NULL, 0, clipboard_read_threadfunc,
                 hwnd, 0, &in_threadid);
}

/*
 * Print a modal (Really Bad) message box and perform a fatal exit.
 */
void modalfatalbox(const char *fmt, ...)
{
    va_list ap;
    char *message, *title;

    va_start(ap, fmt);
    message = dupvprintf(fmt, ap);
    va_end(ap);
    title = dupprintf("%s Fatal Error", appname);
    MessageBox(hwnd, message, title, MB_SYSTEMMODAL | MB_ICONERROR | MB_OK);
    sfree(message);
    sfree(title);
    cleanup_exit(1);
}

/*
 * Print a message box and don't close the connection.
 */
void nonfatal(const char *fmt, ...)
{
    va_list ap;
    char *message, *title;

    va_start(ap, fmt);
    message = dupvprintf(fmt, ap);
    va_end(ap);
    title = dupprintf("%s Error", appname);
    MessageBox(hwnd, message, title, MB_ICONERROR | MB_OK);
    sfree(message);
    sfree(title);
}

static bool flash_window_ex(DWORD dwFlags, UINT uCount, DWORD dwTimeout)
{
    if (p_FlashWindowEx) {
        FLASHWINFO fi;
        fi.cbSize = sizeof(fi);
        fi.hwnd = hwnd;
        fi.dwFlags = dwFlags;
        fi.uCount = uCount;
        fi.dwTimeout = dwTimeout;
        return (*p_FlashWindowEx)(&fi);
    }
    else
        return false; /* shrug */
}

static void flash_window(int mode);
static long next_flash;
static bool flashing = false;

/*
 * Timer for platforms where we must maintain window flashing manually
 * (e.g., Win95).
 */
static void flash_window_timer(void *ctx, unsigned long now)
{
    if (flashing && now == next_flash) {
        flash_window(1);
    }
}

/*
 * Manage window caption / taskbar flashing, if enabled.
 * 0 = stop, 1 = maintain, 2 = start
 */
static void flash_window(int mode)
{
    int beep_ind = conf_get_int(conf, CONF_beep_ind);
    if ((mode == 0) || (beep_ind == B_IND_DISABLED)) {
        /* stop */
        if (flashing) {
            flashing = false;
            if (p_FlashWindowEx)
                flash_window_ex(FLASHW_STOP, 0, 0);
            else
                FlashWindow(hwnd, false);
        }

    } else if (mode == 2) {
        /* start */
        if (!flashing) {
            flashing = true;
            if (p_FlashWindowEx) {
                /* For so-called "steady" mode, we use uCount=2, which
                 * seems to be the traditional number of flashes used
                 * by user notifications (e.g., by Explorer).
                 * uCount=0 appears to enable continuous flashing, per
                 * "flashing" mode, although I haven't seen this
                 * documented. */
                flash_window_ex(FLASHW_ALL | FLASHW_TIMER,
                                (beep_ind == B_IND_FLASH ? 0 : 2),
                                0 /* system cursor blink rate */);
                /* No need to schedule timer */
            } else {
                FlashWindow(hwnd, true);
                next_flash = schedule_timer(450, flash_window_timer, hwnd);
            }
        }

    } else if ((mode == 1) && (beep_ind == B_IND_FLASH)) {
        /* maintain */
        if (flashing && !p_FlashWindowEx) {
            FlashWindow(hwnd, true);    /* toggle */
            next_flash = schedule_timer(450, flash_window_timer, hwnd);
        }
    }
}

/*
 * Beep.
 */
static void wintw_bell(TermWin *tw, int mode)
{
    if (mode == BELL_DEFAULT) {
        /*
         * For MessageBeep style bells, we want to be careful of
         * timing, because they don't have the nice property of
         * PlaySound bells that each one cancels the previous
         * active one. So we limit the rate to one per 50ms or so.
         */
        static long lastbeep = 0;
        long beepdiff;

        beepdiff = GetTickCount() - lastbeep;
        if (beepdiff >= 0 && beepdiff < 50)
            return;
        MessageBeep(MB_OK);
        /*
         * The above MessageBeep call takes time, so we record the
         * time _after_ it finishes rather than before it starts.
         */
        lastbeep = GetTickCount();
    } else if (mode == BELL_WAVEFILE) {
        Filename *bell_wavefile = conf_get_filename(conf, CONF_bell_wavefile);
        if (!p_PlaySound || !p_PlaySound(bell_wavefile->path, NULL,
                         SND_ASYNC | SND_FILENAME)) {
            char *buf, *otherbuf;
            buf = dupprintf(
                "Unable to play sound file\n%s\nUsing default sound instead",
                bell_wavefile->path);
            otherbuf = dupprintf("%s Sound Error", appname);
            MessageBox(hwnd, buf, otherbuf, MB_OK | MB_ICONEXCLAMATION);
            sfree(buf);
            sfree(otherbuf);
            conf_set_int(conf, CONF_beep, BELL_DEFAULT);
        }
    } else if (mode == BELL_PCSPEAKER) {
        static long lastbeep = 0;
        long beepdiff;

        beepdiff = GetTickCount() - lastbeep;
        if (beepdiff >= 0 && beepdiff < 50)
            return;

        /*
         * We must beep in different ways depending on whether this
         * is a 95-series or NT-series OS.
         */
        if (osPlatformId == VER_PLATFORM_WIN32_NT)
            Beep(800, 100);
        else
            MessageBeep(-1);
        lastbeep = GetTickCount();
    }
    /* Otherwise, either visual bell or disabled; do nothing here */
    if (!term->has_focus) {
        flash_window(2);               /* start */
    }
}

/*
 * Minimise or restore the window in response to a server-side
 * request.
 */
static void wintw_set_minimised(TermWin *tw, bool minimised)
{
    if (IsIconic(hwnd)) {
        if (!minimised)
            ShowWindow(hwnd, SW_RESTORE);
    } else {
        if (minimised)
            ShowWindow(hwnd, SW_MINIMIZE);
    }
}

/*
 * Move the window in response to a server-side request.
 */
static void wintw_move(TermWin *tw, int x, int y)
{
    int resize_action = conf_get_int(conf, CONF_resize_action);
    if (resize_action == RESIZE_DISABLED ||
        resize_action == RESIZE_FONT ||
        IsZoomed(hwnd))
       return;

    SetWindowPos(hwnd, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
}

/*
 * Move the window to the top or bottom of the z-order in response
 * to a server-side request.
 */
static void wintw_set_zorder(TermWin *tw, bool top)
{
    if (conf_get_bool(conf, CONF_alwaysontop))
        return;                        /* ignore */
    SetWindowPos(hwnd, top ? HWND_TOP : HWND_BOTTOM, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE);
}

/*
 * Refresh the window in response to a server-side request.
 */
static void wintw_refresh(TermWin *tw)
{
    InvalidateRect(hwnd, NULL, true);
}

/*
 * Maximise or restore the window in response to a server-side
 * request.
 */
static void wintw_set_maximised(TermWin *tw, bool maximised)
{
    if (IsZoomed(hwnd)) {
        if (!maximised)
            ShowWindow(hwnd, SW_RESTORE);
    } else {
        if (maximised)
            ShowWindow(hwnd, SW_MAXIMIZE);
    }
}

/*
 * Report whether the window is iconic, for terminal reports.
 */
static bool wintw_is_minimised(TermWin *tw)
{
    return IsIconic(hwnd);
}

/*
 * Report the window's position, for terminal reports.
 */
static void wintw_get_pos(TermWin *tw, int *x, int *y)
{
    RECT r;
    GetWindowRect(hwnd, &r);
    *x = r.left;
    *y = r.top;
}

/*
 * Report the window's pixel size, for terminal reports.
 */
static void wintw_get_pixels(TermWin *tw, int *x, int *y)
{
    RECT r;
    GetWindowRect(hwnd, &r);
    *x = r.right - r.left;
    *y = r.bottom - r.top;
}

/*
 * Return the window or icon title.
 */
static const char *wintw_get_title(TermWin *tw, bool icon)
{
    return icon ? icon_name : window_name;
}

/*
 * See if we're in full-screen mode.
 */
static bool is_full_screen()
{
    if (!IsZoomed(hwnd))
        return false;
    if (GetWindowLongPtr(hwnd, GWL_STYLE) & WS_CAPTION)
        return false;
    return true;
}

/* Get the rect/size of a full screen window using the nearest available
 * monitor in multimon systems; default to something sensible if only
 * one monitor is present. */
static bool get_fullscreen_rect(RECT * ss)
{
#if defined(MONITOR_DEFAULTTONEAREST) && !defined(NO_MULTIMON)
        HMONITOR mon;
        MONITORINFO mi;
        mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        mi.cbSize = sizeof(mi);
        GetMonitorInfo(mon, &mi);

        /* structure copy */
        *ss = mi.rcMonitor;
        return true;
#else
/* could also use code like this:
        ss->left = ss->top = 0;
        ss->right = GetSystemMetrics(SM_CXSCREEN);
        ss->bottom = GetSystemMetrics(SM_CYSCREEN);
*/
        return GetClientRect(GetDesktopWindow(), ss);
#endif
}


/*
 * Go full-screen. This should only be called when we are already
 * maximised.
 */
static void make_full_screen()
{
    DWORD style;
        RECT ss;

    assert(IsZoomed(hwnd));

        if (is_full_screen())
                return;

    /* Remove the window furniture. */
    style = GetWindowLongPtr(hwnd, GWL_STYLE);
    style &= ~(WS_CAPTION | WS_BORDER | WS_THICKFRAME);
    if (conf_get_bool(conf, CONF_scrollbar_in_fullscreen))
        style |= WS_VSCROLL;
    else
        style &= ~WS_VSCROLL;
    SetWindowLongPtr(hwnd, GWL_STYLE, style);

    /* Resize ourselves to exactly cover the nearest monitor. */
        get_fullscreen_rect(&ss);
    SetWindowPos(hwnd, HWND_TOP, ss.left, ss.top,
                        ss.right - ss.left,
                        ss.bottom - ss.top,
                        SWP_FRAMECHANGED);

    /* We may have changed size as a result */

    reset_window(0);

    /* Tick the menu item in the System and context menus. */
    {
        int i;
        for (i = 0; i < lenof(popup_menus); i++)
            CheckMenuItem(popup_menus[i].menu, IDM_FULLSCREEN, MF_CHECKED);
    }
}

/*
 * Clear the full-screen attributes.
 */
static void clear_full_screen()
{
    DWORD oldstyle, style;

    /* Reinstate the window furniture. */
    style = oldstyle = GetWindowLongPtr(hwnd, GWL_STYLE);
    style |= WS_CAPTION | WS_BORDER;
    if (conf_get_int(conf, CONF_resize_action) == RESIZE_DISABLED)
        style &= ~WS_THICKFRAME;
    else
        style |= WS_THICKFRAME;
    if (conf_get_bool(conf, CONF_scrollbar))
        style |= WS_VSCROLL;
    else
        style &= ~WS_VSCROLL;
    if (style != oldstyle) {
        SetWindowLongPtr(hwnd, GWL_STYLE, style);
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                     SWP_FRAMECHANGED);
    }

    /* Untick the menu item in the System and context menus. */
    {
        int i;
        for (i = 0; i < lenof(popup_menus); i++)
            CheckMenuItem(popup_menus[i].menu, IDM_FULLSCREEN, MF_UNCHECKED);
    }
}

/*
 * Toggle full-screen mode.
 */
static void flip_full_screen()
{
    if (is_full_screen()) {
        ShowWindow(hwnd, SW_RESTORE);
    } else if (IsZoomed(hwnd)) {
        make_full_screen();
    } else {
        SendMessage(hwnd, WM_FULLSCR_ON_MAX, 0, 0);
        ShowWindow(hwnd, SW_MAXIMIZE);
    }
}

static size_t win_seat_output(Seat *seat, bool is_stderr,
                              const void *data, size_t len)
{
    return term_data(term, is_stderr, data, len);
}

static bool win_seat_eof(Seat *seat)
{
    return true;   /* do respond to incoming EOF with outgoing */
}

static int win_seat_get_userpass_input(
    Seat *seat, prompts_t *p, bufchain *input)
{
    int ret;
    ret = cmdline_get_passwd_input(p);
    if (ret == -1)
        ret = term_get_userpass_input(term, p, input);
    return ret;
}

void agent_schedule_callback(void (*callback)(void *, void *, int),
                             void *callback_ctx, void *data, int len)
{
    struct agent_callback *c = snew(struct agent_callback);
    c->callback = callback;
    c->callback_ctx = callback_ctx;
    c->data = data;
    c->len = len;
    PostMessage(hwnd, WM_AGENT_CALLBACK, 0, (LPARAM)c);
}

static bool win_seat_set_trust_status(Seat *seat, bool trusted)
{
    term_set_trust_status(term, trusted);
    return true;
}
