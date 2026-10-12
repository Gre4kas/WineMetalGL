/* What the Wine module itself does, for 32-bit and 64-bit programs:
 * - glBindBuffersRange and glBindVertexBuffers take arrays of GLintptr and
 *   GLsizeiptr (32 bits wide in a 32-bit program): the bound ranges read
 *   back as given. Prints WINEMETALGL_MULTI_BIND_RANGES_OK.
 * - A context current on another thread can be neither made current nor
 *   deleted (ERROR_BUSY), and can be once released. Prints
 *   WINEMETALGL_WGL_CONTEXT_BUSY_OK.
 * - A pbuffer has one DC until it is released; an unknown attribute fails
 *   with ERROR_INVALID_DATA. Prints WINEMETALGL_WGL_PBUFFER_DC_OK.
 * - Child windows drawn to, hidden and destroyed, again and again. Prints
 *   WINEMETALGL_WGL_WINDOW_LIFECYCLE_OK. */
#include <windows.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/wglext.h>
#include <stdio.h>

static PFNGLGENBUFFERSPROC p_glGenBuffers;
static PFNGLBINDBUFFERPROC p_glBindBuffer;
static PFNGLBUFFERDATAPROC p_glBufferData;
static PFNGLBINDBUFFERSRANGEPROC p_glBindBuffersRange;
static PFNGLBINDVERTEXBUFFERSPROC p_glBindVertexBuffers;
static PFNGLGETINTEGER64I_VPROC p_glGetInteger64i_v;
static PFNGLGENVERTEXARRAYSPROC p_glGenVertexArrays;
static PFNGLBINDVERTEXARRAYPROC p_glBindVertexArray;

static HANDLE current, release;

static DWORD WINAPI hold_context( void *arg )
{
    HDC dc = ((HDC *)arg)[0];
    HGLRC context = ((HGLRC *)arg)[1];
    BOOL ok = wglMakeCurrent( dc, context );
    SetEvent( current );
    WaitForSingleObject( release, INFINITE );
    wglMakeCurrent( NULL, NULL );
    return ok ? 0 : 1;
}

static int multi_bind(void)
{
    GLuint buffers[2], vao;
    GLintptr offsets[2] = {256, 512};
    GLsizeiptr sizes[2] = {16, 32};
    GLsizei strides[2] = {8, 12};
    GLint64 value;
    int i;

    p_glGenBuffers( 2, buffers );
    for (i = 0; i < 2; i++)
    {
        p_glBindBuffer( GL_UNIFORM_BUFFER, buffers[i] );
        p_glBufferData( GL_UNIFORM_BUFFER, 1024, NULL, GL_STATIC_DRAW );
    }
    p_glBindBuffersRange( GL_UNIFORM_BUFFER, 1, 2, buffers, offsets, sizes );
    for (i = 0; i < 2; i++)
    {
        value = -1;
        p_glGetInteger64i_v( GL_UNIFORM_BUFFER_START, 1 + i, &value );
        if (value != offsets[i]) { printf( "uniform buffer %d start %lld\n", 1 + i, (long long)value ); return 0; }
        value = -1;
        p_glGetInteger64i_v( GL_UNIFORM_BUFFER_SIZE, 1 + i, &value );
        if (value != sizes[i]) { printf( "uniform buffer %d size %lld\n", 1 + i, (long long)value ); return 0; }
    }
    p_glGenVertexArrays( 1, &vao );
    p_glBindVertexArray( vao );
    p_glBindVertexBuffers( 2, 2, buffers, offsets, strides );
    for (i = 0; i < 2; i++)
    {
        value = -1;
        p_glGetInteger64i_v( GL_VERTEX_BINDING_OFFSET, 2 + i, &value );
        if (value != offsets[i]) { printf( "vertex binding %d offset %lld\n", 2 + i, (long long)value ); return 0; }
    }
    return glGetError() == GL_NO_ERROR;
}

static int busy( HDC dc, HGLRC context )
{
    void *args[2] = {dc, context};
    HANDLE thread;
    DWORD code = 1;

    current = CreateEventA( NULL, TRUE, FALSE, NULL );
    release = CreateEventA( NULL, TRUE, FALSE, NULL );
    thread = CreateThread( NULL, 0, hold_context, args, 0, NULL );
    if (!thread) return 0;
    WaitForSingleObject( current, INFINITE );
    SetLastError( 0xdeadbeef );
    if (wglMakeCurrent( dc, context ) || GetLastError() != ERROR_BUSY)
    {
        printf( "make current: error %lu\n", GetLastError() );
        return 0;
    }
    SetLastError( 0xdeadbeef );
    if (wglDeleteContext( context ) || GetLastError() != ERROR_BUSY)
    {
        printf( "delete: error %lu\n", GetLastError() );
        return 0;
    }
    SetEvent( release );
    WaitForSingleObject( thread, INFINITE );
    GetExitCodeThread( thread, &code );
    CloseHandle( thread );
    if (code) return 0;
    if (!wglMakeCurrent( dc, context ) || !wglMakeCurrent( NULL, NULL )) return 0;
    return wglDeleteContext( context );
}

static int pbuffer_dc( HDC dc )
{
    PFNWGLCREATEPBUFFERARBPROC create = (void *)wglGetProcAddress( "wglCreatePbufferARB" );
    PFNWGLGETPBUFFERDCARBPROC get_dc = (void *)wglGetProcAddress( "wglGetPbufferDCARB" );
    PFNWGLRELEASEPBUFFERDCARBPROC release_dc = (void *)wglGetProcAddress( "wglReleasePbufferDCARB" );
    PFNWGLQUERYPBUFFERARBPROC query = (void *)wglGetProcAddress( "wglQueryPbufferARB" );
    PFNWGLDESTROYPBUFFERARBPROC destroy = (void *)wglGetProcAddress( "wglDestroyPbufferARB" );
    PFNWGLCHOOSEPIXELFORMATARBPROC choose = (void *)wglGetProcAddress( "wglChoosePixelFormatARB" );
    const int wanted[] = {WGL_DRAW_TO_PBUFFER_ARB, 1, 0};
    HPBUFFERARB pbuffer;
    HDC first, second;
    int value = 0, format = 0;
    UINT count = 0;

    if (!create || !get_dc || !release_dc || !query || !destroy || !choose) { printf( "no WGL_ARB_pbuffer\n" ); return 0; }
    if (!choose( dc, wanted, NULL, 1, &format, &count ) || !count) { printf( "no pbuffer format\n" ); return 0; }
    if (!(pbuffer = create( dc, format, 16, 16, NULL ))) { printf( "no pbuffer: error %lu\n", GetLastError() ); return 0; }
    first = get_dc( pbuffer );
    second = get_dc( pbuffer );
    if (!first || second != first) { printf( "pbuffer DCs %p %p\n", first, second ); return 0; }
    if (!query( pbuffer, WGL_PBUFFER_WIDTH_ARB, &value ) || value != 16) { printf( "width %d\n", value ); return 0; }
    SetLastError( 0xdeadbeef );
    if (query( pbuffer, 0x1234, &value ) || GetLastError() != ERROR_INVALID_DATA)
    {
        printf( "query: error %lu\n", GetLastError() );
        return 0;
    }
    if (!release_dc( pbuffer, first )) { printf( "release DC: error %lu\n", GetLastError() ); return 0; }
    return destroy( pbuffer );
}

/* Child windows drawn to, hidden, destroyed and made again: each new one
 * gets a drawable, and destroyed ones are let go of. */
static int window_lifecycle( HWND parent, HGLRC context, const PIXELFORMATDESCRIPTOR *pfd )
{
    int round;

    for (round = 0; round < 8; round++)
    {
        HWND child = CreateWindowA( "STATIC", "child", WS_CHILD | WS_VISIBLE, 8, 8, 32, 32, parent, NULL, NULL, NULL );
        HDC dc;

        if (!child) return 0;
        dc = GetDC( child );
        if (!SetPixelFormat( dc, ChoosePixelFormat( dc, pfd ), pfd ) || !wglMakeCurrent( dc, context ))
        {
            printf( "round %d: make current: error %lu\n", round, GetLastError() );
            return 0;
        }
        glClearColor( 0, 1, 0, 1 );
        glClear( GL_COLOR_BUFFER_BIT );
        if (!SwapBuffers( dc )) return 0;
        ShowWindow( child, SW_HIDE );
        glClear( GL_COLOR_BUFFER_BIT );
        if (!SwapBuffers( dc )) return 0;
        wglMakeCurrent( NULL, NULL );
        ReleaseDC( child, dc );
        DestroyWindow( child );
    }
    return 1;
}

int main(void)
{
    PIXELFORMATDESCRIPTOR pfd = {sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
                                 PFD_TYPE_RGBA, 32};
    HWND hwnd = CreateWindowA( "STATIC", "wmgl", WS_POPUP, 0, 0, 64, 64, NULL, NULL, NULL, NULL );
    HDC dc;
    HGLRC context, other;

    if (!hwnd) return 11;
    dc = GetDC( hwnd );
    if (!SetPixelFormat( dc, ChoosePixelFormat( dc, &pfd ), &pfd )) return 12;
    if (!(context = wglCreateContext( dc )) || !wglMakeCurrent( dc, context )) return 13;
#define LOAD(name) if (!(*(PROC *)&p_##name = wglGetProcAddress( #name ))) { printf( "missing %s\n", #name ); return 14; }
    LOAD(glGenBuffers) LOAD(glBindBuffer) LOAD(glBufferData) LOAD(glBindBuffersRange) LOAD(glBindVertexBuffers)
    LOAD(glGetInteger64i_v) LOAD(glGenVertexArrays) LOAD(glBindVertexArray)
    printf( "%u-bit\n", (unsigned)sizeof(void *) * 8 );
    if (!multi_bind()) return 15;
    printf( "WINEMETALGL_MULTI_BIND_RANGES_OK\n" );
    if (!pbuffer_dc( dc )) return 16;
    printf( "WINEMETALGL_WGL_PBUFFER_DC_OK\n" );
    wglMakeCurrent( NULL, NULL );
    if (!(other = wglCreateContext( dc )) || !busy( dc, other )) return 17;
    printf( "WINEMETALGL_WGL_CONTEXT_BUSY_OK\n" );
    if (!window_lifecycle( hwnd, context, &pfd )) return 18;
    printf( "WINEMETALGL_WGL_WINDOW_LIFECYCLE_OK\n" );
    wglDeleteContext( context );
    ReleaseDC( hwnd, dc );
    DestroyWindow( hwnd );
    return 0;
}
