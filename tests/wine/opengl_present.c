/* Window presents with swap interval 0 (the mailbox presenter) and 1, across
 * a resize, and the back buffer still reads back between them. Run with
 * WINEMETALGL_METALFX=2 it covers MetalFX upscaling on both present paths.
 * Prints WINEMETALGL_PRESENT_INTERVALS_OK. */
#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>

typedef BOOL (WINAPI *SwapInterval)(int);

/* Clears to a colour that changes each frame, presents, and checks the
 * last frame read back before its swap. */
static int frames( HDC dc, int count, int width, int height )
{
    unsigned char pixel[4];
    int i;

    glViewport( 0, 0, width, height );
    for (i = 0; i < count; i++)
    {
        glClearColor( (i & 1) ? 1.0f : 0.0f, 0.5f, (float)i / count, 1.0f );
        glClear( GL_COLOR_BUFFER_BIT );
        if (i == count - 1)
        {
            glReadPixels( width - 1, height - 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel );
            if (pixel[1] < 126 || pixel[1] > 129 || pixel[3] != 255)
            {
                printf( "readback %u %u %u %u\n", pixel[0], pixel[1], pixel[2], pixel[3] );
                return 0;
            }
        }
        if (!SwapBuffers( dc ))
        {
            printf( "SwapBuffers error=%lu\n", GetLastError() );
            return 0;
        }
    }
    return glGetError() == GL_NO_ERROR;
}

int main(void)
{
    PIXELFORMATDESCRIPTOR pfd = {sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32};
    SwapInterval swap_interval;
    HGLRC context;
    HWND window;
    HDC dc;

    window = CreateWindowA( "STATIC", "WineMetalGL present", WS_POPUP, 0, 0, 64, 64, NULL, NULL, NULL, NULL );
    if (!window) return 1;
    dc = GetDC( window );
    if (!SetPixelFormat( dc, ChoosePixelFormat( dc, &pfd ), &pfd )) return 2;
    if (!(context = wglCreateContext( dc )) || !wglMakeCurrent( dc, context )) return 3;
    if (!(swap_interval = (SwapInterval)wglGetProcAddress( "wglSwapIntervalEXT" ))) return 4;

    if (!swap_interval( 0 ) || !frames( dc, 60, 64, 64 )) return 5;
    SetWindowPos( window, NULL, 0, 0, 96, 48, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE );
    if (!frames( dc, 30, 96, 48 )) return 6;
    if (!swap_interval( 1 ) || !frames( dc, 10, 96, 48 )) return 7;
    SetWindowPos( window, NULL, 0, 0, 40, 72, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE );
    if (!frames( dc, 10, 40, 72 )) return 8;
    if (!swap_interval( 0 ) || !frames( dc, 30, 40, 72 )) return 9;

    printf( "WINEMETALGL_PRESENT_INTERVALS_OK\n" );
    wglMakeCurrent( NULL, NULL );
    wglDeleteContext( context );
    ReleaseDC( window, dc );
    DestroyWindow( window );
    return 0;
}
