/* UpscaleGL: fullscreen programs render smaller than the screen, and the
 * core scales their frames up to it with MetalFX (WINEMETALGL_UPSCALE).
 *
 * A program sees a primary display a preset's fraction of its real size: the
 * display modes, metrics and monitor rectangles it asks user32 and gdi32 for
 * are scaled down, and a mode change to one of them succeeds without
 * changing the real mode. A top-level window that covers the screen the
 * program sees is made to cover the real one, and the program goes on seeing
 * it at the size it asked for: its client and window rectangles, the size
 * and position messages its window procedure gets (through a subclass), and
 * mouse and cursor positions in its client and screen coordinates are
 * scaled. The window's GL surface is the size the program renders at (the
 * render size of wmgl_window_placement), and the core shows it over the
 * whole window.
 *
 * The program sees one display, the primary one: with others listed, a
 * program that picks the display with the most pixels (DOOM (2016) beside a
 * MacBook's own display) would leave the one UpscaleGL scales.
 *
 * The functions are replaced in the import address tables of the program's
 * modules (not Wine's builtin DLLs, nor this one, whose own calls reach the
 * real functions) as they load, and GetProcAddress returns the replacements.
 * A module that reads the display before this DLL loads (a program loading
 * opengl32.dll late) sees the real one.
 */

#include "wmgl_pe.h"

#include <math.h>
#include <psapi.h>

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

static BOOL enabled;
static HMODULE self;
static SRWLOCK lock = SRWLOCK_INIT;

/* What the program does with its windows and the display, in the log
 * (WINEMETALGL_LOG=2): the first few hundred calls. */
static LONG log_budget = 300;
#define LOG_CALL(...) do { if (wmgl_log_max >= 2 && InterlockedDecrement( &log_budget ) >= 0) \
                              WMGL_INFO( "UpscaleGL: " __VA_ARGS__ ); } while (0)

/* ----------------------------------------------------------- the display */

struct mode
{
    DWORD width, height, bits, frequency;
};

static WCHAR primary_device[CCHDEVICENAME];
static HMONITOR primary_monitor;
static LONG real_width, real_height;        /* the real primary display mode */
static LONG desktop_width, desktop_height;  /* what the program sees of it */
static LONG mode_width, mode_height;        /* the mode it set, else the desktop */
/* The modes listed: the preset's size alone, at the refresh rates of the
 * real screen's size. The preset is the resolution: with smaller modes
 * listed, a program that does not find its saved one takes the first it
 * accepts (DOOM (2016) passes over those under 640x480 and took 668x501, a
 * 4:3 frame, where Balanced's 1114x626 was meant). */
static struct mode modes[64];
static unsigned int mode_count;

/* Real coordinates to the program's (the mode it set) and back. */
static LONG seen_x( LONG x ) { return MulDiv( x, mode_width, real_width ); }
static LONG seen_y( LONG y ) { return MulDiv( y, mode_height, real_height ); }
static LONG real_x( LONG x ) { return MulDiv( x, real_width, mode_width ); }
static LONG real_y( LONG y ) { return MulDiv( y, real_height, mode_height ); }

static void seen_point( POINT *p )
{
    p->x = seen_x( p->x );
    p->y = seen_y( p->y );
}

static void real_point( POINT *p )
{
    p->x = real_x( p->x );
    p->y = real_y( p->y );
}

static void seen_rect( RECT *r )
{
    seen_point( (POINT *)&r->left );
    seen_point( (POINT *)&r->right );
}

static void real_rect( RECT *r )
{
    real_point( (POINT *)&r->left );
    real_point( (POINT *)&r->right );
}

static BOOL is_primary( const WCHAR *device )
{
    return !device || !*device || !_wcsicmp( device, primary_device );
}

static BOOL is_primary_a( const char *device )
{
    WCHAR name[CCHDEVICENAME];
    if (!device || !*device) return TRUE;
    if (!MultiByteToWideChar( CP_ACP, 0, device, -1, name, ARRAY_SIZE(name) )) return FALSE;
    return is_primary( name );
}

/* The preset's ratio of the rendered size to the real one, per axis:
 * "quality", "balanced", "performance", "ultra" (or "ultra-performance"), or
 * a percentage ("50%", "50") or fraction ("0.5"); 0 for "off" or what is not
 * understood. */
static double preset_ratio( const char *value )
{
    static const struct { const char *name; double ratio; } presets[] =
    {
        {"quality", 2.0 / 3.0}, {"balanced", 0.58}, {"performance", 0.5},
        {"ultra", 1.0 / 3.0}, {"ultra-performance", 1.0 / 3.0}, {"ultraperformance", 1.0 / 3.0},
    };
    double number;
    unsigned int i;
    char *end;

    if (!value || !*value) return 0;
    for (i = 0; i < ARRAY_SIZE(presets); i++)
        if (!_stricmp( value, presets[i].name )) return presets[i].ratio;
    number = strtod( value, &end );
    if (end == value || (*end && strcmp( end, "%" ))) return 0;
    if (*end == '%' || number > 1.0) number /= 100.0;
    return number >= 0.25 && number < 1.0 ? number : 0;
}

static void add_mode( DWORD width, DWORD height, DWORD bits, DWORD frequency )
{
    unsigned int i;

    for (i = 0; i < mode_count; i++)
        if (modes[i].width == width && modes[i].height == height && modes[i].bits == bits &&
            modes[i].frequency == frequency)
            return;
    if (mode_count == ARRAY_SIZE(modes)) return;
    modes[mode_count].width = width;
    modes[mode_count].height = height;
    modes[mode_count].bits = bits;
    modes[mode_count].frequency = frequency;
    mode_count++;
}

static BOOL has_mode( DWORD width, DWORD height )
{
    unsigned int i;
    for (i = 0; i < mode_count; i++)
        if (modes[i].width == width && modes[i].height == height) return TRUE;
    return FALSE;
}

/* What a program that changes the display mode gets: success, the mode
 * becoming the one it sees (the real mode stays). */
static LONG change_mode( const DWORD *fields, const DWORD *width, const DWORD *height, DWORD flags )
{
    LONG w = mode_width, h = mode_height;

    if (!fields)
    {
        w = desktop_width;
        h = desktop_height;
    }
    else
    {
        if ((*fields & DM_PELSWIDTH) && *width) w = *width;
        if ((*fields & DM_PELSHEIGHT) && *height) h = *height;
    }
    LOG_CALL( "display mode %ldx%ld asked for (flags %#lx)\n", w, h, flags );
    /* Up to the real size: a program that asks for the screen's own size
     * (a resolution saved before UpscaleGL) gets it, unscaled, on this display
     * rather than a refusal that may send it to another one. */
    if (!has_mode( w, h ) && (w > real_width || h > real_height)) return DISP_CHANGE_BADMODE;
    if (flags & CDS_TEST) return DISP_CHANGE_SUCCESSFUL;
    if (w != mode_width || h != mode_height)
        WMGL_INFO( "UpscaleGL: display mode %ldx%ld (shown at %ldx%ld)\n", w, h, real_width, real_height );
    mode_width = w;
    mode_height = h;
    return DISP_CHANGE_SUCCESSFUL;
}

/* ----------------------------------------------------- the hooked functions */

#define HOOKS \
    X(user32, BOOL, EnumDisplaySettingsA, (LPCSTR device, DWORD index, DEVMODEA *mode)) \
    X(user32, BOOL, EnumDisplaySettingsW, (LPCWSTR device, DWORD index, DEVMODEW *mode)) \
    X(user32, BOOL, EnumDisplaySettingsExA, (LPCSTR device, DWORD index, DEVMODEA *mode, DWORD flags)) \
    X(user32, BOOL, EnumDisplaySettingsExW, (LPCWSTR device, DWORD index, DEVMODEW *mode, DWORD flags)) \
    X(user32, LONG, ChangeDisplaySettingsA, (DEVMODEA *mode, DWORD flags)) \
    X(user32, LONG, ChangeDisplaySettingsW, (DEVMODEW *mode, DWORD flags)) \
    X(user32, LONG, ChangeDisplaySettingsExA, (LPCSTR device, DEVMODEA *mode, HWND hwnd, DWORD flags, void *param)) \
    X(user32, LONG, ChangeDisplaySettingsExW, (LPCWSTR device, DEVMODEW *mode, HWND hwnd, DWORD flags, void *param)) \
    X(user32, int, GetSystemMetrics, (int index)) \
    X(user32, int, GetSystemMetricsForDpi, (int index, UINT dpi)) \
    X(user32, BOOL, GetMonitorInfoA, (HMONITOR monitor, MONITORINFO *info)) \
    X(user32, BOOL, GetMonitorInfoW, (HMONITOR monitor, MONITORINFO *info)) \
    X(user32, BOOL, EnumDisplayMonitors, (HDC hdc, const RECT *clip, MONITORENUMPROC proc, LPARAM param)) \
    X(user32, BOOL, ShowWindow, (HWND hwnd, int command)) \
    X(user32, BOOL, EnumDisplayDevicesA, (LPCSTR device, DWORD index, DISPLAY_DEVICEA *info, DWORD flags)) \
    X(user32, BOOL, EnumDisplayDevicesW, (LPCWSTR device, DWORD index, DISPLAY_DEVICEW *info, DWORD flags)) \
    X(user32, HMONITOR, MonitorFromWindow, (HWND hwnd, DWORD flags)) \
    X(user32, HMONITOR, MonitorFromPoint, (POINT point, DWORD flags)) \
    X(user32, HMONITOR, MonitorFromRect, (const RECT *rect, DWORD flags)) \
    X(gdi32, int, GetDeviceCaps, (HDC hdc, int index)) \
    X(user32, HWND, CreateWindowExA, (DWORD ex_style, LPCSTR class_name, LPCSTR name, DWORD style, int x, int y, \
                                      int width, int height, HWND parent, HMENU menu, HINSTANCE instance, void *param)) \
    X(user32, HWND, CreateWindowExW, (DWORD ex_style, LPCWSTR class_name, LPCWSTR name, DWORD style, int x, int y, \
                                      int width, int height, HWND parent, HMENU menu, HINSTANCE instance, void *param)) \
    X(user32, BOOL, SetWindowPos, (HWND hwnd, HWND after, int x, int y, int cx, int cy, UINT flags)) \
    X(user32, BOOL, MoveWindow, (HWND hwnd, int x, int y, int width, int height, BOOL repaint)) \
    X(user32, BOOL, GetClientRect, (HWND hwnd, RECT *rect)) \
    X(user32, BOOL, GetWindowRect, (HWND hwnd, RECT *rect)) \
    X(user32, BOOL, ClientToScreen, (HWND hwnd, POINT *point)) \
    X(user32, BOOL, ScreenToClient, (HWND hwnd, POINT *point)) \
    X(user32, int, MapWindowPoints, (HWND from, HWND to, POINT *points, UINT count)) \
    X(user32, BOOL, GetCursorPos, (POINT *point)) \
    X(user32, BOOL, SetCursorPos, (int x, int y)) \
    X(user32, BOOL, GetCursorInfo, (CURSORINFO *info)) \
    X(user32, BOOL, ClipCursor, (const RECT *rect)) \
    X(user32, BOOL, GetClipCursor, (RECT *rect)) \
    X(user32, DWORD, GetMessagePos, (void)) \
    X(user32, BOOL, PeekMessageA, (MSG *msg, HWND hwnd, UINT first, UINT last, UINT flags)) \
    X(user32, BOOL, PeekMessageW, (MSG *msg, HWND hwnd, UINT first, UINT last, UINT flags)) \
    X(user32, BOOL, GetMessageA, (MSG *msg, HWND hwnd, UINT first, UINT last)) \
    X(user32, BOOL, GetMessageW, (MSG *msg, HWND hwnd, UINT first, UINT last)) \
    X(user32, LONG, GetWindowLongA, (HWND hwnd, int index)) \
    X(user32, LONG, GetWindowLongW, (HWND hwnd, int index)) \
    X(user32, LONG, SetWindowLongA, (HWND hwnd, int index, LONG value)) \
    X(user32, LONG, SetWindowLongW, (HWND hwnd, int index, LONG value)) \
    X(user32, LONG_PTR, GetWindowLongPtrA, (HWND hwnd, int index)) \
    X(user32, LONG_PTR, GetWindowLongPtrW, (HWND hwnd, int index)) \
    X(user32, LONG_PTR, SetWindowLongPtrA, (HWND hwnd, int index, LONG_PTR value)) \
    X(user32, LONG_PTR, SetWindowLongPtrW, (HWND hwnd, int index, LONG_PTR value)) \
    X(kernel32, FARPROC, GetProcAddress, (HMODULE module, LPCSTR name)) \
    X(kernel32, HMODULE, LoadLibraryA, (LPCSTR name)) \
    X(kernel32, HMODULE, LoadLibraryW, (LPCWSTR name)) \
    X(kernel32, HMODULE, LoadLibraryExA, (LPCSTR name, HANDLE file, DWORD flags)) \
    X(kernel32, HMODULE, LoadLibraryExW, (LPCWSTR name, HANDLE file, DWORD flags))

/* hook_<export>. GetWindowLongPtrA and the like are macros of the headers
 * on i386, where user32 does not export them (their entries stay unused). */
#define X(dll, ret, name, args) static ret WINAPI hook_##name args;
HOOKS
#undef X

struct hook
{
    const char *dll;
    const char *name;
    void *hook;
    void *real;  /* the export's address, as import tables hold it */
};

static struct hook hooks[] =
{
#define X(dll, ret, name, args) {#dll ".dll", #name, (void *)hook_##name, NULL},
    HOOKS
#undef X
};

/* --------------------------------------------- windows covering the screen */

struct scaled_window
{
    HWND hwnd;
    WNDPROC proc;  /* the program's window procedure */
    BOOL unicode;  /* proc takes Unicode messages */
};

static struct scaled_window scaled[16];
static unsigned int scaled_count;

static LRESULT CALLBACK scaled_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam );

static struct scaled_window *find_scaled( HWND hwnd )
{
    unsigned int i;
    for (i = 0; i < scaled_count; i++)
        if (scaled[i].hwnd == hwnd) return &scaled[i];
    return NULL;
}

/* The window covers the screen, or is in a top-level window that does: its
 * coordinates are the program's. */
static BOOL is_scaled( HWND hwnd )
{
    HWND root;
    BOOL found;

    if (!enabled || !hwnd || !(root = GetAncestor( hwnd, GA_ROOT ))) return FALSE;
    AcquireSRWLockShared( &lock );
    found = find_scaled( root ) != NULL;
    ReleaseSRWLockShared( &lock );
    return found;
}

/* Screen coordinates are the program's while a window covers the screen. */
static BOOL screen_scaled(void)
{
    BOOL any;

    if (!enabled) return FALSE;
    AcquireSRWLockShared( &lock );
    any = scaled_count > 0;
    ReleaseSRWLockShared( &lock );
    return any;
}

static void scale_window( HWND hwnd )
{
    struct scaled_window *window;
    BOOL unicode = IsWindowUnicode( hwnd );
    WNDPROC proc = (WNDPROC)(unicode ? GetWindowLongPtrW( hwnd, GWLP_WNDPROC ) : GetWindowLongPtrA( hwnd, GWLP_WNDPROC ));

    AcquireSRWLockExclusive( &lock );
    if (find_scaled( hwnd ) || scaled_count == ARRAY_SIZE(scaled))
    {
        ReleaseSRWLockExclusive( &lock );
        return;
    }
    window = &scaled[scaled_count++];
    window->hwnd = hwnd;
    window->proc = proc;
    window->unicode = unicode;
    ReleaseSRWLockExclusive( &lock );
    /* Outside the lock: the subclass takes it. */
    if (unicode) SetWindowLongPtrW( hwnd, GWLP_WNDPROC, (LONG_PTR)scaled_proc );
    else SetWindowLongPtrA( hwnd, GWLP_WNDPROC, (LONG_PTR)scaled_proc );
    WMGL_INFO( "UpscaleGL: window %p covers the screen: %ldx%ld shown at %ldx%ld\n", hwnd, mode_width, mode_height,
               real_width, real_height );
}

static void forget_scaled( HWND hwnd, BOOL restore )
{
    struct scaled_window *window, gone = {0};

    AcquireSRWLockExclusive( &lock );
    if ((window = find_scaled( hwnd )))
    {
        gone = *window;
        *window = scaled[--scaled_count];
    }
    ReleaseSRWLockExclusive( &lock );
    if (!gone.hwnd) return;
    if (restore && (WNDPROC)GetWindowLongPtrW( hwnd, GWLP_WNDPROC ) == scaled_proc)
    {
        if (gone.unicode) SetWindowLongPtrW( hwnd, GWLP_WNDPROC, (LONG_PTR)gone.proc );
        else SetWindowLongPtrA( hwnd, GWLP_WNDPROC, (LONG_PTR)gone.proc );
    }
    WMGL_INFO( "UpscaleGL: window %p no longer covers the screen\n", hwnd );
}

/* The window procedure of a window that covers the screen: the program's,
 * given sizes and positions in its coordinates. */
static LRESULT CALLBACK scaled_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    struct scaled_window *window, copy = {0};
    WINDOWPOS seen, *pos;
    LRESULT ret;

    AcquireSRWLockShared( &lock );
    if ((window = find_scaled( hwnd ))) copy = *window;
    ReleaseSRWLockShared( &lock );
    if (!copy.proc) return DefWindowProcW( hwnd, msg, wparam, lparam );

#define CALL(l) (copy.unicode ? CallWindowProcW( copy.proc, hwnd, msg, wparam, (l) ) \
                              : CallWindowProcA( copy.proc, hwnd, msg, wparam, (l) ))
    switch (msg)
    {
    case WM_SIZE:
        return CALL( MAKELPARAM( seen_x( LOWORD(lparam) ), seen_y( HIWORD(lparam) ) ) );
    case WM_MOVE:
        return CALL( MAKELPARAM( seen_x( (short)LOWORD(lparam) ), seen_y( (short)HIWORD(lparam) ) ) );
    case WM_WINDOWPOSCHANGING:
    case WM_WINDOWPOSCHANGED:
        if (!(pos = (WINDOWPOS *)lparam)) break;
        seen = *pos;
        seen.x = seen_x( pos->x );
        seen.y = seen_y( pos->y );
        seen.cx = seen_x( pos->cx );
        seen.cy = seen_y( pos->cy );
        ret = CALL( (LPARAM)&seen );
        if (msg == WM_WINDOWPOSCHANGING)
        {
            /* What the program changed, in real coordinates. */
            if (seen.x != seen_x( pos->x )) pos->x = real_x( seen.x );
            if (seen.y != seen_y( pos->y )) pos->y = real_y( seen.y );
            if (seen.cx != seen_x( pos->cx )) pos->cx = real_x( seen.cx );
            if (seen.cy != seen_y( pos->cy )) pos->cy = real_y( seen.cy );
            pos->flags = seen.flags;
            pos->hwndInsertAfter = seen.hwndInsertAfter;
        }
        return ret;
    case WM_NCDESTROY:
        ret = CALL( lparam );
        forget_scaled( hwnd, FALSE );
        return ret;
    }
    return CALL( lparam );
#undef CALL
}

/* x, y, cx, cy (in the program's coordinates) cover the screen it sees. */
static BOOL covers_screen( int x, int y, int cx, int cy )
{
    if (x == CW_USEDEFAULT || cx == CW_USEDEFAULT) return FALSE;
    return x <= 0 && y <= 0 && x + cx >= mode_width && y + cy >= mode_height;
}

/* A top-level window the program has not placed through us (maximized, say)
 * that covers the real screen. */
static void check_covering( HWND hwnd )
{
    DWORD process = 0;
    RECT rect;

    if (!enabled || !hwnd || GetAncestor( hwnd, GA_ROOT ) != hwnd || is_scaled( hwnd )) return;
    if ((GetWindowLongW( hwnd, GWL_STYLE ) & WS_CHILD) || !IsWindowVisible( hwnd )) return;
    GetWindowThreadProcessId( hwnd, &process );
    if (process != GetCurrentProcessId() || !GetWindowRect( hwnd, &rect )) return;
    if (rect.left <= 0 && rect.top <= 0 && rect.right >= real_width && rect.bottom >= real_height) scale_window( hwnd );
}

/* A window placed (x, y, cx, cy in the program's coordinates; flags
 * SetWindowPos's): to real coordinates when it covers the screen or is in a
 * window that does. */
static void place_window( HWND hwnd, int *x, int *y, int *cx, int *cy, UINT flags )
{
    BOOL child, size = !(flags & SWP_NOSIZE), move = !(flags & SWP_NOMOVE);
    RECT rect;
    int px = *x, py = *y;

    if (!enabled || !hwnd || (!size && !move)) return;
    child = (GetWindowLongW( hwnd, GWL_STYLE ) & WS_CHILD) != 0;
    if (!child) LOG_CALL( "place %p at %d,%d %dx%d flags %#x\n", hwnd, *x, *y, *cx, *cy, flags );
    if (child)
    {
        if (!is_scaled( hwnd )) return;
    }
    else if (size)
    {
        if (!move && hook_GetWindowRect( hwnd, &rect ))
        {
            px = rect.left;
            py = rect.top;
        }
        if (covers_screen( px, py, *cx, *cy )) scale_window( hwnd );
        else if (is_scaled( hwnd )) forget_scaled( hwnd, TRUE );  /* back to a window */
    }
    if (!is_scaled( hwnd )) return;
    if (move)
    {
        *x = real_x( *x );
        *y = real_y( *y );
    }
    if (size)
    {
        *cx = real_x( *cx );
        *cy = real_y( *cy );
    }
}

/* -------------------------------------------------------- display hooks */

static BOOL WINAPI hook_EnumDisplaySettingsExW( LPCWSTR device, DWORD index, DEVMODEW *mode, DWORD flags )
{
    if (!enabled) return EnumDisplaySettingsExW( device, index, mode, flags );
    if (!is_primary( device )) return FALSE;  /* one display */
    if (index == ENUM_CURRENT_SETTINGS || index == ENUM_REGISTRY_SETTINGS)
    {
        if (!EnumDisplaySettingsExW( device, index, mode, flags )) return FALSE;
        mode->dmPelsWidth = index == ENUM_CURRENT_SETTINGS ? mode_width : desktop_width;
        mode->dmPelsHeight = index == ENUM_CURRENT_SETTINGS ? mode_height : desktop_height;
        return TRUE;
    }
    if (index >= mode_count || !EnumDisplaySettingsExW( device, ENUM_CURRENT_SETTINGS, mode, flags )) return FALSE;
    mode->dmPelsWidth = modes[index].width;
    mode->dmPelsHeight = modes[index].height;
    mode->dmBitsPerPel = modes[index].bits;
    mode->dmDisplayFrequency = modes[index].frequency;
    mode->dmFields |= DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
    return TRUE;
}

static BOOL WINAPI hook_EnumDisplaySettingsExA( LPCSTR device, DWORD index, DEVMODEA *mode, DWORD flags )
{
    if (!enabled) return EnumDisplaySettingsExA( device, index, mode, flags );
    if (!is_primary_a( device )) return FALSE;  /* one display */
    if (index == ENUM_CURRENT_SETTINGS || index == ENUM_REGISTRY_SETTINGS)
    {
        if (!EnumDisplaySettingsExA( device, index, mode, flags )) return FALSE;
        mode->dmPelsWidth = index == ENUM_CURRENT_SETTINGS ? mode_width : desktop_width;
        mode->dmPelsHeight = index == ENUM_CURRENT_SETTINGS ? mode_height : desktop_height;
        return TRUE;
    }
    if (index >= mode_count || !EnumDisplaySettingsExA( device, ENUM_CURRENT_SETTINGS, mode, flags )) return FALSE;
    mode->dmPelsWidth = modes[index].width;
    mode->dmPelsHeight = modes[index].height;
    mode->dmBitsPerPel = modes[index].bits;
    mode->dmDisplayFrequency = modes[index].frequency;
    mode->dmFields |= DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
    return TRUE;
}

static BOOL WINAPI hook_EnumDisplaySettingsW( LPCWSTR device, DWORD index, DEVMODEW *mode )
{
    return hook_EnumDisplaySettingsExW( device, index, mode, 0 );
}

static BOOL WINAPI hook_EnumDisplaySettingsA( LPCSTR device, DWORD index, DEVMODEA *mode )
{
    return hook_EnumDisplaySettingsExA( device, index, mode, 0 );
}

static LONG WINAPI hook_ChangeDisplaySettingsExW( LPCWSTR device, DEVMODEW *mode, HWND hwnd, DWORD flags, void *param )
{
    if (!enabled) return ChangeDisplaySettingsExW( device, mode, hwnd, flags, param );
    if (!is_primary( device ))
    {
        LOG_CALL( "display mode change on %ls refused\n", device );
        return DISP_CHANGE_BADMODE;  /* one display */
    }
    return mode ? change_mode( &mode->dmFields, &mode->dmPelsWidth, &mode->dmPelsHeight, flags )
                : change_mode( NULL, NULL, NULL, flags );
}

static LONG WINAPI hook_ChangeDisplaySettingsExA( LPCSTR device, DEVMODEA *mode, HWND hwnd, DWORD flags, void *param )
{
    if (!enabled) return ChangeDisplaySettingsExA( device, mode, hwnd, flags, param );
    if (!is_primary_a( device ))
    {
        LOG_CALL( "display mode change on %s refused\n", device );
        return DISP_CHANGE_BADMODE;  /* one display */
    }
    return mode ? change_mode( &mode->dmFields, &mode->dmPelsWidth, &mode->dmPelsHeight, flags )
                : change_mode( NULL, NULL, NULL, flags );
}

static LONG WINAPI hook_ChangeDisplaySettingsW( DEVMODEW *mode, DWORD flags )
{
    return hook_ChangeDisplaySettingsExW( NULL, mode, NULL, flags, NULL );
}

static LONG WINAPI hook_ChangeDisplaySettingsA( DEVMODEA *mode, DWORD flags )
{
    return hook_ChangeDisplaySettingsExA( NULL, mode, NULL, flags, NULL );
}

static int seen_metric( int index, int value )
{
    switch (index)
    {
    case SM_CXSCREEN:
        return mode_width;
    case SM_CYSCREEN:
        return mode_height;
    /* The primary display is the only one (see "One display"). */
    case SM_XVIRTUALSCREEN:
    case SM_YVIRTUALSCREEN:
        return 0;
    case SM_CXVIRTUALSCREEN:
        return mode_width;
    case SM_CYVIRTUALSCREEN:
        return mode_height;
    case SM_CMONITORS:
        return 1;
    case SM_CXFULLSCREEN:
    case SM_CXMAXIMIZED:
    case SM_CXMAXTRACK:
        return seen_x( value );
    case SM_CYFULLSCREEN:
    case SM_CYMAXIMIZED:
    case SM_CYMAXTRACK:
        return seen_y( value );
    }
    return value;
}

static int WINAPI hook_GetSystemMetrics( int index )
{
    int value = GetSystemMetrics( index );
    return enabled ? seen_metric( index, value ) : value;
}

static int WINAPI hook_GetSystemMetricsForDpi( int index, UINT dpi )
{
    int (WINAPI *real)( int, UINT ) = NULL;
    unsigned int i;
    int value;

    for (i = 0; i < ARRAY_SIZE(hooks) && !real; i++)
        if (hooks[i].hook == (void *)hook_GetSystemMetricsForDpi) real = (int (WINAPI *)( int, UINT ))hooks[i].real;
    value = real ? real( index, dpi ) : GetSystemMetrics( index );
    return enabled ? seen_metric( index, value ) : value;
}

static void seen_monitor( MONITORINFO *info )
{
    if (info->rcMonitor.left != 0 || info->rcMonitor.top != 0 || info->rcMonitor.right != real_width ||
        info->rcMonitor.bottom != real_height)
        return;  /* not the primary display */
    info->rcMonitor.right = mode_width;
    info->rcMonitor.bottom = mode_height;
    seen_rect( &info->rcWork );
}

static BOOL WINAPI hook_GetMonitorInfoW( HMONITOR monitor, MONITORINFO *info )
{
    if (!GetMonitorInfoW( monitor, info )) return FALSE;
    if (enabled) seen_monitor( info );
    return TRUE;
}

static BOOL WINAPI hook_GetMonitorInfoA( HMONITOR monitor, MONITORINFO *info )
{
    if (!GetMonitorInfoA( monitor, info )) return FALSE;
    if (enabled) seen_monitor( info );
    return TRUE;
}

struct monitor_enum
{
    MONITORENUMPROC proc;
    LPARAM param;
};

static BOOL CALLBACK seen_monitor_enum( HMONITOR monitor, HDC hdc, RECT *rect, LPARAM param )
{
    const struct monitor_enum *program = (const struct monitor_enum *)param;
    RECT seen = *rect;

    if (monitor != primary_monitor) return TRUE;  /* one display */
    if (seen.left == 0 && seen.top == 0 && seen.right == real_width && seen.bottom == real_height)
    {
        seen.right = mode_width;
        seen.bottom = mode_height;
    }
    return program->proc( monitor, hdc, &seen, program->param );
}

static BOOL WINAPI hook_EnumDisplayMonitors( HDC hdc, const RECT *clip, MONITORENUMPROC proc, LPARAM param )
{
    struct monitor_enum program = {proc, param};
    if (!enabled || !proc) return EnumDisplayMonitors( hdc, clip, proc, param );
    return EnumDisplayMonitors( hdc, clip, seen_monitor_enum, (LPARAM)&program );
}

/* Adapters: the primary one alone; a monitor of another: none. */
static BOOL WINAPI hook_EnumDisplayDevicesW( LPCWSTR device, DWORD index, DISPLAY_DEVICEW *info, DWORD flags )
{
    DWORD i;

    if (!enabled) return EnumDisplayDevicesW( device, index, info, flags );
    if (device) return is_primary( device ) && EnumDisplayDevicesW( device, index, info, flags );
    if (index) return FALSE;
    for (i = 0; EnumDisplayDevicesW( NULL, i, info, flags ); i++)
        if (info->StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) return TRUE;
    return FALSE;
}

static BOOL WINAPI hook_EnumDisplayDevicesA( LPCSTR device, DWORD index, DISPLAY_DEVICEA *info, DWORD flags )
{
    DWORD i;

    if (!enabled) return EnumDisplayDevicesA( device, index, info, flags );
    if (device) return is_primary_a( device ) && EnumDisplayDevicesA( device, index, info, flags );
    if (index) return FALSE;
    for (i = 0; EnumDisplayDevicesA( NULL, i, info, flags ); i++)
        if (info->StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) return TRUE;
    return FALSE;
}

/* Every window, point and rectangle is on the primary display. */
static HMONITOR WINAPI hook_MonitorFromWindow( HWND hwnd, DWORD flags )
{
    HMONITOR monitor = MonitorFromWindow( hwnd, flags );
    return enabled && monitor ? primary_monitor : monitor;
}

static HMONITOR WINAPI hook_MonitorFromPoint( POINT point, DWORD flags )
{
    HMONITOR monitor = MonitorFromPoint( point, flags );
    return enabled && (monitor || flags != MONITOR_DEFAULTTONULL) ? primary_monitor : monitor;
}

static HMONITOR WINAPI hook_MonitorFromRect( const RECT *rect, DWORD flags )
{
    HMONITOR monitor = MonitorFromRect( rect, flags );
    return enabled && monitor ? primary_monitor : monitor;
}

static int WINAPI hook_GetDeviceCaps( HDC hdc, int index )
{
    int value = GetDeviceCaps( hdc, index );
    if (!enabled || GetDeviceCaps( hdc, TECHNOLOGY ) != DT_RASDISPLAY) return value;
    switch (index)
    {
    case HORZRES:
    case DESKTOPHORZRES:
        return value == real_width ? mode_width : value;
    case VERTRES:
    case DESKTOPVERTRES:
        return value == real_height ? mode_height : value;
    }
    return value;
}

/* --------------------------------------------------------- window hooks */

static HWND WINAPI hook_CreateWindowExW( DWORD ex_style, LPCWSTR class_name, LPCWSTR name, DWORD style, int x, int y,
                                         int width, int height, HWND parent, HMENU menu, HINSTANCE instance, void *param )
{
    BOOL in_scaled = enabled && (style & WS_CHILD) && is_scaled( parent );
    HWND hwnd;

    if (in_scaled && x != CW_USEDEFAULT && width != CW_USEDEFAULT)
    {
        x = real_x( x );
        y = real_y( y );
        width = real_x( width );
        height = real_y( height );
    }
    hwnd = CreateWindowExW( ex_style, class_name, name, style, x, y, width, height, parent, menu, instance, param );
    if (enabled && hwnd && !(style & WS_CHILD))
        LOG_CALL( "window %p created at %d,%d %dx%d style %#lx ex %#lx\n", hwnd, x, y, width, height, style, ex_style );
    /* Created at the size the program asked for, then over the real screen,
     * which the subclass shows it as that size. */
    if (hwnd && enabled && !(style & WS_CHILD) && covers_screen( x, y, width, height ))
    {
        scale_window( hwnd );
        SetWindowPos( hwnd, NULL, real_x( x ), real_y( y ), real_x( width ), real_y( height ),
                      SWP_NOZORDER | SWP_NOACTIVATE );
    }
    return hwnd;
}

static HWND WINAPI hook_CreateWindowExA( DWORD ex_style, LPCSTR class_name, LPCSTR name, DWORD style, int x, int y,
                                         int width, int height, HWND parent, HMENU menu, HINSTANCE instance, void *param )
{
    BOOL in_scaled = enabled && (style & WS_CHILD) && is_scaled( parent );
    HWND hwnd;

    if (in_scaled && x != CW_USEDEFAULT && width != CW_USEDEFAULT)
    {
        x = real_x( x );
        y = real_y( y );
        width = real_x( width );
        height = real_y( height );
    }
    hwnd = CreateWindowExA( ex_style, class_name, name, style, x, y, width, height, parent, menu, instance, param );
    if (enabled && hwnd && !(style & WS_CHILD))
        LOG_CALL( "window %p created at %d,%d %dx%d style %#lx ex %#lx\n", hwnd, x, y, width, height, style, ex_style );
    if (hwnd && enabled && !(style & WS_CHILD) && covers_screen( x, y, width, height ))
    {
        scale_window( hwnd );
        SetWindowPos( hwnd, NULL, real_x( x ), real_y( y ), real_x( width ), real_y( height ),
                      SWP_NOZORDER | SWP_NOACTIVATE );
    }
    return hwnd;
}

static BOOL WINAPI hook_SetWindowPos( HWND hwnd, HWND after, int x, int y, int cx, int cy, UINT flags )
{
    place_window( hwnd, &x, &y, &cx, &cy, flags );
    return SetWindowPos( hwnd, after, x, y, cx, cy, flags );
}

static BOOL WINAPI hook_ShowWindow( HWND hwnd, int command )
{
    BOOL ret = ShowWindow( hwnd, command );
    if (enabled && hwnd && GetAncestor( hwnd, GA_ROOT ) == hwnd)
    {
        LOG_CALL( "ShowWindow %p %d\n", hwnd, command );
        check_covering( hwnd );
    }
    return ret;
}

static BOOL WINAPI hook_MoveWindow( HWND hwnd, int x, int y, int width, int height, BOOL repaint )
{
    place_window( hwnd, &x, &y, &width, &height, 0 );
    return MoveWindow( hwnd, x, y, width, height, repaint );
}

static BOOL WINAPI hook_GetClientRect( HWND hwnd, RECT *rect )
{
    if (!GetClientRect( hwnd, rect )) return FALSE;
    check_covering( hwnd );
    if (is_scaled( hwnd ))
    {
        rect->right = seen_x( rect->right );
        rect->bottom = seen_y( rect->bottom );
    }
    return TRUE;
}

static BOOL WINAPI hook_GetWindowRect( HWND hwnd, RECT *rect )
{
    if (!GetWindowRect( hwnd, rect )) return FALSE;
    check_covering( hwnd );
    if (is_scaled( hwnd )) seen_rect( rect );
    return TRUE;
}

static BOOL WINAPI hook_ClientToScreen( HWND hwnd, POINT *point )
{
    BOOL ret, scale = point && is_scaled( hwnd );
    if (scale) real_point( point );
    ret = ClientToScreen( hwnd, point );
    if (scale) seen_point( point );
    return ret;
}

static BOOL WINAPI hook_ScreenToClient( HWND hwnd, POINT *point )
{
    BOOL ret, scale = point && is_scaled( hwnd );
    if (scale) real_point( point );
    ret = ScreenToClient( hwnd, point );
    if (scale) seen_point( point );
    return ret;
}

static int WINAPI hook_MapWindowPoints( HWND from, HWND to, POINT *points, UINT count )
{
    BOOL scale = points && screen_scaled() && (!from || is_scaled( from )) && (!to || is_scaled( to ));
    UINT i;
    int ret;

    if (!scale) return MapWindowPoints( from, to, points, count );
    for (i = 0; i < count; i++) real_point( &points[i] );
    ret = MapWindowPoints( from, to, points, count );
    for (i = 0; i < count; i++) seen_point( &points[i] );
    return MAKELONG( seen_x( (short)LOWORD(ret) ), seen_y( (short)HIWORD(ret) ) );
}

/* --------------------------------------------------------- cursor hooks */

static BOOL WINAPI hook_GetCursorPos( POINT *point )
{
    if (!GetCursorPos( point )) return FALSE;
    if (screen_scaled()) seen_point( point );
    return TRUE;
}

static BOOL WINAPI hook_SetCursorPos( int x, int y )
{
    if (screen_scaled())
    {
        x = real_x( x );
        y = real_y( y );
    }
    return SetCursorPos( x, y );
}

static BOOL WINAPI hook_GetCursorInfo( CURSORINFO *info )
{
    if (!GetCursorInfo( info )) return FALSE;
    if (screen_scaled()) seen_point( &info->ptScreenPos );
    return TRUE;
}

static BOOL WINAPI hook_ClipCursor( const RECT *rect )
{
    RECT real;
    if (!rect || !screen_scaled()) return ClipCursor( rect );
    real = *rect;
    real_rect( &real );
    return ClipCursor( &real );
}

static BOOL WINAPI hook_GetClipCursor( RECT *rect )
{
    if (!GetClipCursor( rect )) return FALSE;
    if (screen_scaled()) seen_rect( rect );
    return TRUE;
}

static DWORD WINAPI hook_GetMessagePos( void )
{
    DWORD pos = GetMessagePos();
    if (!screen_scaled()) return pos;
    return MAKELONG( seen_x( (short)LOWORD(pos) ), seen_y( (short)HIWORD(pos) ) );
}

/* A message the program takes from its queue: mouse positions in its
 * coordinates. */
static void seen_message( MSG *msg )
{
    BOOL screen = msg->message == WM_MOUSEWHEEL || msg->message == WM_MOUSEHWHEEL;  /* screen coordinates */
    BOOL client = !screen && msg->message >= WM_MOUSEFIRST && msg->message <= WM_MOUSELAST;
    BOOL nonclient = msg->message >= WM_NCMOUSEMOVE && msg->message <= WM_NCXBUTTONDBLCLK;  /* screen coordinates */

    if (!screen_scaled()) return;
    seen_point( &msg->pt );
    if (screen || ((client || nonclient) && is_scaled( msg->hwnd )))
        msg->lParam = MAKELPARAM( seen_x( (short)LOWORD(msg->lParam) ), seen_y( (short)HIWORD(msg->lParam) ) );
}

static BOOL WINAPI hook_PeekMessageW( MSG *msg, HWND hwnd, UINT first, UINT last, UINT flags )
{
    BOOL ret = PeekMessageW( msg, hwnd, first, last, flags );
    if (ret) seen_message( msg );
    return ret;
}

static BOOL WINAPI hook_PeekMessageA( MSG *msg, HWND hwnd, UINT first, UINT last, UINT flags )
{
    BOOL ret = PeekMessageA( msg, hwnd, first, last, flags );
    if (ret) seen_message( msg );
    return ret;
}

static BOOL WINAPI hook_GetMessageW( MSG *msg, HWND hwnd, UINT first, UINT last )
{
    BOOL ret = GetMessageW( msg, hwnd, first, last );
    if (ret && ret != -1) seen_message( msg );
    return ret;
}

static BOOL WINAPI hook_GetMessageA( MSG *msg, HWND hwnd, UINT first, UINT last )
{
    BOOL ret = GetMessageA( msg, hwnd, first, last );
    if (ret && ret != -1) seen_message( msg );
    return ret;
}

/* ------------------------------------------- the subclass, kept out of sight */

/* The program's window procedure of a window that covers the screen,
 * where the program asks for or sets GWLP_WNDPROC. */
static BOOL program_proc( HWND hwnd, int index, LONG_PTR *proc )
{
    struct scaled_window *window;

    if (!enabled || index != GWLP_WNDPROC) return FALSE;
    AcquireSRWLockShared( &lock );
    if ((window = find_scaled( hwnd ))) *proc = (LONG_PTR)window->proc;
    ReleaseSRWLockShared( &lock );
    return window != NULL;
}

static BOOL set_program_proc( HWND hwnd, int index, LONG_PTR proc, BOOL unicode, LONG_PTR *old )
{
    struct scaled_window *window;

    if (!enabled || index != GWLP_WNDPROC) return FALSE;
    AcquireSRWLockExclusive( &lock );
    if ((window = find_scaled( hwnd )))
    {
        *old = (LONG_PTR)window->proc;
        window->proc = (WNDPROC)proc;
        window->unicode = unicode;
    }
    ReleaseSRWLockExclusive( &lock );
    return window != NULL;
}

static LONG_PTR WINAPI hook_GetWindowLongPtrW( HWND hwnd, int index )
{
    LONG_PTR proc;
    return program_proc( hwnd, index, &proc ) ? proc : GetWindowLongPtrW( hwnd, index );
}

static LONG_PTR WINAPI hook_GetWindowLongPtrA( HWND hwnd, int index )
{
    LONG_PTR proc;
    return program_proc( hwnd, index, &proc ) ? proc : GetWindowLongPtrA( hwnd, index );
}

static LONG_PTR WINAPI hook_SetWindowLongPtrW( HWND hwnd, int index, LONG_PTR value )
{
    LONG_PTR old;
    return set_program_proc( hwnd, index, value, TRUE, &old ) ? old : SetWindowLongPtrW( hwnd, index, value );
}

static LONG_PTR WINAPI hook_SetWindowLongPtrA( HWND hwnd, int index, LONG_PTR value )
{
    LONG_PTR old;
    return set_program_proc( hwnd, index, value, FALSE, &old ) ? old : SetWindowLongPtrA( hwnd, index, value );
}

static LONG WINAPI hook_GetWindowLongW( HWND hwnd, int index )
{
    LONG_PTR proc;
    return program_proc( hwnd, index, &proc ) ? (LONG)proc : GetWindowLongW( hwnd, index );
}

static LONG WINAPI hook_GetWindowLongA( HWND hwnd, int index )
{
    LONG_PTR proc;
    return program_proc( hwnd, index, &proc ) ? (LONG)proc : GetWindowLongA( hwnd, index );
}

static LONG WINAPI hook_SetWindowLongW( HWND hwnd, int index, LONG value )
{
    LONG_PTR old;
    return set_program_proc( hwnd, index, value, TRUE, &old ) ? (LONG)old : SetWindowLongW( hwnd, index, value );
}

static LONG WINAPI hook_SetWindowLongA( HWND hwnd, int index, LONG value )
{
    LONG_PTR old;
    return set_program_proc( hwnd, index, value, FALSE, &old ) ? (LONG)old : SetWindowLongA( hwnd, index, value );
}

/* ------------------------------------------------------ import patching */

static SRWLOCK patch_lock = SRWLOCK_INIT;
static HMODULE patched[1024];
static unsigned int patched_count;

static BOOL is_wine_builtin( HMODULE module )
{
    return !memcmp( (const char *)module + 0x40, "Wine builtin DLL", 16 ) ||
           !memcmp( (const char *)module + 0x40, "Wine placeholder DLL", 20 );
}

static unsigned int patch_module( HMODULE module )
{
    const BYTE *base = (const BYTE *)module;
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    const IMAGE_NT_HEADERS *nt;
    const IMAGE_DATA_DIRECTORY *dir;
    const IMAGE_IMPORT_DESCRIPTOR *desc;
    IMAGE_THUNK_DATA *thunk;
    unsigned int i, count = 0;
    DWORD old;

    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    nt = (const IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR_MAGIC) return 0;
    dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir->VirtualAddress || !dir->Size) return 0;
    for (desc = (const IMAGE_IMPORT_DESCRIPTOR *)(base + dir->VirtualAddress); desc->Name; desc++)
    {
        for (thunk = (IMAGE_THUNK_DATA *)(base + desc->FirstThunk); thunk->u1.Function; thunk++)
        {
            for (i = 0; i < ARRAY_SIZE(hooks); i++)
            {
                if (!hooks[i].real || (void *)thunk->u1.Function != hooks[i].real) continue;
                if (VirtualProtect( &thunk->u1.Function, sizeof(thunk->u1.Function), PAGE_READWRITE, &old ))
                {
                    thunk->u1.Function = (ULONG_PTR)hooks[i].hook;
                    VirtualProtect( &thunk->u1.Function, sizeof(thunk->u1.Function), old, &old );
                    count++;
                }
                break;
            }
        }
    }
    return count;
}

/* Modules loaded since the last call: the program's get the hooks. */
static void patch_modules(void)
{
    HMODULE modules[1024];
    DWORD needed = 0;
    unsigned int i, j, count;

    if (!K32EnumProcessModules( GetCurrentProcess(), modules, sizeof(modules), &needed )) return;
    count = needed / sizeof(HMODULE) < ARRAY_SIZE(modules) ? needed / sizeof(HMODULE) : ARRAY_SIZE(modules);
    AcquireSRWLockExclusive( &patch_lock );
    for (i = 0; i < count; i++)
    {
        for (j = 0; j < patched_count && patched[j] != modules[i]; j++);
        if (j < patched_count) continue;
        if (patched_count < ARRAY_SIZE(patched)) patched[patched_count++] = modules[i];
        if (modules[i] == self || is_wine_builtin( modules[i] )) continue;
        if ((j = patch_module( modules[i] )))
        {
            WCHAR path[MAX_PATH];
            if (wmgl_log_max >= 2 && GetModuleFileNameW( modules[i], path, ARRAY_SIZE(path) ))
                WMGL_INFO( "UpscaleGL: %u imports of %ls\n", j, path );
        }
    }
    ReleaseSRWLockExclusive( &patch_lock );
}

static FARPROC WINAPI hook_GetProcAddress( HMODULE module, LPCSTR name )
{
    FARPROC proc = GetProcAddress( module, name );
    unsigned int i;

    if (!enabled || !proc) return proc;
    for (i = 0; i < ARRAY_SIZE(hooks); i++)
        if (hooks[i].real == (void *)proc) return (FARPROC)hooks[i].hook;
    return proc;
}

static HMODULE WINAPI hook_LoadLibraryA( LPCSTR name )
{
    HMODULE module = LoadLibraryA( name );
    if (module && enabled) patch_modules();
    return module;
}

static HMODULE WINAPI hook_LoadLibraryW( LPCWSTR name )
{
    HMODULE module = LoadLibraryW( name );
    if (module && enabled) patch_modules();
    return module;
}

static HMODULE WINAPI hook_LoadLibraryExA( LPCSTR name, HANDLE file, DWORD flags )
{
    HMODULE module = LoadLibraryExA( name, file, flags );
    if (module && enabled) patch_modules();
    return module;
}

static HMODULE WINAPI hook_LoadLibraryExW( LPCWSTR name, HANDLE file, DWORD flags )
{
    HMODULE module = LoadLibraryExW( name, file, flags );
    if (module && enabled) patch_modules();
    return module;
}

/* ------------------------------------------------------------- the module */

void upscalegl_init( HMODULE module )
{
    const char *setting = getenv( "WINEMETALGL_UPSCALE" );
    double ratio = preset_ratio( setting );
    MONITORINFOEXW monitor;
    DEVMODEW mode;
    POINT origin = {0, 0};
    unsigned int i;

    if (setting && *setting && _stricmp( setting, "off" ) && _stricmp( setting, "0" ) && ratio <= 0)
        WMGL_WARN( "UpscaleGL: WINEMETALGL_UPSCALE=%s is not off, quality, balanced, performance, ultra or a "
                   "percentage from 25 to 99; off\n", setting );
    if (ratio <= 0) return;
    memset( &mode, 0, sizeof(mode) );
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsW( NULL, ENUM_CURRENT_SETTINGS, &mode ) || !mode.dmPelsWidth || !mode.dmPelsHeight)
    {
        WMGL_WARN( "UpscaleGL: no display mode; off\n" );
        return;
    }
    memset( &monitor, 0, sizeof(monitor) );
    monitor.cbSize = sizeof(monitor);
    primary_monitor = MonitorFromPoint( origin, MONITOR_DEFAULTTOPRIMARY );
    if (GetMonitorInfoW( primary_monitor, (MONITORINFO *)&monitor ))
        lstrcpynW( primary_device, monitor.szDevice, ARRAY_SIZE(primary_device) );
    self = module;
    real_width = mode.dmPelsWidth;
    real_height = mode.dmPelsHeight;
    desktop_width = mode_width = max( 1, (LONG)lround( real_width * ratio ) );
    desktop_height = mode_height = max( 1, (LONG)lround( real_height * ratio ) );
    for (i = 0; i < 4096 && mode_count < ARRAY_SIZE(modes); i++)
    {
        DEVMODEW other;
        memset( &other, 0, sizeof(other) );
        other.dmSize = sizeof(other);
        if (!EnumDisplaySettingsW( NULL, i, &other )) break;
        if (other.dmPelsWidth == (DWORD)real_width && other.dmPelsHeight == (DWORD)real_height)
            add_mode( desktop_width, desktop_height, other.dmBitsPerPel, other.dmDisplayFrequency );
    }
    if (!has_mode( desktop_width, desktop_height ))
        add_mode( desktop_width, desktop_height, mode.dmBitsPerPel, mode.dmDisplayFrequency );
    for (i = 0; i < ARRAY_SIZE(hooks); i++)
    {
        HMODULE dll = GetModuleHandleA( hooks[i].dll );
        hooks[i].real = dll ? (void *)GetProcAddress( dll, hooks[i].name ) : NULL;
    }
    enabled = TRUE;
    patch_modules();
    WMGL_INFO( "UpscaleGL: %s (%.0f%%): the %ldx%ld display is shown to programs as %ldx%ld, %u modes\n", setting,
               ratio * 100.0, real_width, real_height, desktop_width, desktop_height, mode_count );
}

BOOL upscalegl_render_size( HWND hwnd, int width, int height, int *render_width, int *render_height )
{
    if (!is_scaled( hwnd )) return FALSE;
    *render_width = max( 1, seen_x( width ) );
    *render_height = max( 1, seen_y( height ) );
    return TRUE;
}
