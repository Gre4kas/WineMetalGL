/* GL calls the Wine module batches (wmgl_batch: commands that return
 * nothing and take values) keep GL's order and semantics:
 * - an error a batched call raises is what glGetError then returns;
 * - glUniform4fv's array is read when called (the batch keeps a copy), and
 *   an array too large for a batch is passed directly;
 * - more calls than a batch holds run in order;
 * - a vertex pointer to client memory, set by a batched call, is read by
 *   the draw that follows.
 * Prints WINEMETALGL_BATCHED_CALLS_OK. */
#include <windows.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <stdio.h>
#include <string.h>

#define FUNCS(X) X(PFNGLCREATESHADERPROC, glCreateShader) X(PFNGLSHADERSOURCEPROC, glShaderSource) \
    X(PFNGLCOMPILESHADERPROC, glCompileShader) X(PFNGLCREATEPROGRAMPROC, glCreateProgram) \
    X(PFNGLATTACHSHADERPROC, glAttachShader) X(PFNGLLINKPROGRAMPROC, glLinkProgram) \
    X(PFNGLUSEPROGRAMPROC, glUseProgram) X(PFNGLGETPROGRAMIVPROC, glGetProgramiv) \
    X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation) X(PFNGLUNIFORM4FVPROC, glUniform4fv) \
    X(PFNGLUNIFORM1IPROC, glUniform1i)
#define DECLARE(type, name) static type p_##name;
FUNCS(DECLARE)
#define LOAD(type, name) if (!(p_##name = (type)wglGetProcAddress( #name ))) { printf( "missing %s\n", #name ); return 1; }

static LRESULT CALLBACK proc( HWND h, UINT m, WPARAM w, LPARAM l ) { return DefWindowProcA( h, m, w, l ); }

/* The colour at the window's centre after drawing a full-window triangle. */
static void centre( unsigned char rgba[4] )
{
    glReadPixels( 16, 16, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba );
}

static int close_to( const unsigned char *got, int r, int g, int b )
{
    return abs( got[0] - r ) <= 2 && abs( got[1] - g ) <= 2 && abs( got[2] - b ) <= 2;
}

int main(void)
{
    static const char *vs = "#version 120\nvoid main() { gl_Position = vec4(gl_Vertex.xy, 0.0, 1.0); }\n";
    static const char *fs = "#version 120\nuniform vec4 colors[3];\nuniform int pick;\n"
                            "void main() { gl_FragColor = colors[pick]; }\n";
    static const char *big_fs = "#version 120\nuniform vec4 table[900];\nuniform int pick;\n"
                                "void main() { gl_FragColor = table[pick]; }\n";
    static float tri[] = {-1, -1, 3, -1, -1, 3};
    PIXELFORMATDESCRIPTOR pfd = {sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32};
    WNDCLASSA wc = {0};
    float colors[12] = {1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1};
    static float table[1100 * 4];  /* more than the array: GL sets its 900 elements */
    unsigned char rgba[4];
    GLboolean mask[4];
    GLint linked = 0, colors_location, pick_location;
    GLuint program, big;
    HWND hwnd;
    HDC dc;
    HGLRC context;
    int i;

    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleA( NULL );
    wc.lpszClassName = "wmgl";
    wc.style = CS_OWNDC;
    RegisterClassA( &wc );
    hwnd = CreateWindowA( "wmgl", "wmgl", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, NULL, NULL, wc.hInstance, NULL );
    dc = GetDC( hwnd );
    SetPixelFormat( dc, ChoosePixelFormat( dc, &pfd ), &pfd );
    if (!(context = wglCreateContext( dc )) || !wglMakeCurrent( dc, context )) { printf( "no context\n" ); return 1; }
    FUNCS(LOAD)
    glViewport( 0, 0, 32, 32 );

    /* 1. An error raised by a batched call. */
    glEnable( 0xffff );
    if (glGetError() != GL_INVALID_ENUM) { printf( "batched error lost\n" ); return 2; }

    /* 2. Uniform arrays are read when glUniform4fv is called. */
    {
        GLuint v = p_glCreateShader( GL_VERTEX_SHADER ), f = p_glCreateShader( GL_FRAGMENT_SHADER );
        p_glShaderSource( v, 1, &vs, NULL ); p_glCompileShader( v );
        p_glShaderSource( f, 1, &fs, NULL ); p_glCompileShader( f );
        program = p_glCreateProgram();
        p_glAttachShader( program, v ); p_glAttachShader( program, f ); p_glLinkProgram( program );
        p_glGetProgramiv( program, GL_LINK_STATUS, &linked );
        if (!linked) { printf( "link failed\n" ); return 3; }
    }
    /* Locations first: calls that return a value run the batch. */
    colors_location = p_glGetUniformLocation( program, "colors" );
    pick_location = p_glGetUniformLocation( program, "pick" );
    p_glUseProgram( program );
    glEnableClientState( GL_VERTEX_ARRAY );
    glVertexPointer( 2, GL_FLOAT, 0, tri );  /* client memory, read by the draw */
    p_glUniform4fv( colors_location, 3, colors );
    p_glUniform1i( pick_location, 1 );
    memset( colors, 0, sizeof(colors) );  /* after the call: GL has read them, batched or not */
    glDrawArrays( GL_TRIANGLES, 0, 3 );
    centre( rgba );
    if (!close_to( rgba, 0, 255, 0 )) { printf( "uniform array %u %u %u\n", rgba[0], rgba[1], rgba[2] ); return 4; }

    /* 3. A uniform array larger than a batch. */
    {
        GLuint v = p_glCreateShader( GL_VERTEX_SHADER ), f = p_glCreateShader( GL_FRAGMENT_SHADER );
        p_glShaderSource( v, 1, &vs, NULL ); p_glCompileShader( v );
        p_glShaderSource( f, 1, &big_fs, NULL ); p_glCompileShader( f );
        big = p_glCreateProgram();
        p_glAttachShader( big, v ); p_glAttachShader( big, f ); p_glLinkProgram( big );
        p_glGetProgramiv( big, GL_LINK_STATUS, &linked );
        if (!linked) { printf( "big link failed\n" ); return 5; }
    }
    p_glUseProgram( big );
    table[800 * 4 + 2] = 1.0f;  /* table[800]: blue */
    table[800 * 4 + 3] = 1.0f;
    p_glUniform4fv( p_glGetUniformLocation( big, "table" ), 1100, table );  /* 17600 bytes: past a batch */
    p_glUniform1i( p_glGetUniformLocation( big, "pick" ), 800 );
    glDrawArrays( GL_TRIANGLES, 0, 3 );
    centre( rgba );
    if (!close_to( rgba, 0, 0, 255 )) { printf( "large uniform array %u %u %u\n", rgba[0], rgba[1], rgba[2] ); return 6; }

    /* 4. More batched calls than a batch holds, in order. */
    for (i = 0; i < 5001; i++) glColorMask( i & 1, (i >> 1) & 1, GL_TRUE, GL_FALSE );
    glGetBooleanv( GL_COLOR_WRITEMASK, mask );  /* call 5000: (0, 0, 1, 0) */
    if (mask[0] || mask[1] || !mask[2] || mask[3]) { printf( "mask %u %u %u %u\n", mask[0], mask[1], mask[2], mask[3] ); return 7; }
    glColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );

    if (glGetError() != GL_NO_ERROR) { printf( "error %#x\n", glGetError() ); return 8; }
    printf( "WINEMETALGL_BATCHED_CALLS_OK\n" );
    wglMakeCurrent( NULL, NULL );
    wglDeleteContext( context );
    ReleaseDC( hwnd, dc );
    DestroyWindow( hwnd );
    return 0;
}
