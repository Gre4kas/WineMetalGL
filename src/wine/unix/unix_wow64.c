/* winemetalgl.so for 32-bit callers (wow64): what the generated wow64 calls
 * cannot do by widening and narrowing (tools/gen/wine_gen.py MANUAL_WOW64).
 *
 * - Buffer storage lies below 4 GB (the core allocates it through
 *   wmgl_set_buffer_memory here), so mapped pointers reach the application
 *   as they are, coherent and persistent maps included.
 * - Strings the core returns are copied below 4 GB once.
 * - Sync objects become 32-bit handles.
 * - Arrays of 32-bit pointers are widened; pointers written through
 *   pointers are narrowed. */

#include "wmgl_unix.h"

#ifdef _WIN64

static ULONG_PTR zero_bits;
static pthread_mutex_t wow64_lock = PTHREAD_MUTEX_INITIALIZER;

static void *low_alloc( size_t size )
{
    void *memory = NULL;
    SIZE_T length = size;
    if (NtAllocateVirtualMemory( GetCurrentProcess(), &memory, zero_bits, &length, MEM_COMMIT | MEM_RESERVE,
                                 PAGE_READWRITE ))
        return NULL;
    return memory;
}

static void low_free( void *memory, size_t size )
{
    SIZE_T length = 0;
    NtFreeVirtualMemory( GetCurrentProcess(), &memory, &length, MEM_RELEASE );
}

extern NTSTATUS wmgl_unix_init( void *args );

/* The first call of a 32-bit PE side. */
NTSTATUS wmgl_unix_init_wow64( void *args )
{
    void (*set_buffer_memory)( void *(*)(size_t), void (*)(void *, size_t) );
    NTSTATUS status = wmgl_unix_init( args );
    SYSTEM_BASIC_INFORMATION info;

    if (status || !((struct wmgl_init_params *)args)->ok) return status;
    NtQuerySystemInformation( SystemEmulationBasicInformation, &info, sizeof(info), NULL );
    zero_bits = (ULONG_PTR)info.HighestUserAddress | 0x7fffffff;
    if (!(*(void **)&set_buffer_memory = wmgl_core_symbol( "wmgl_set_buffer_memory" )))
    {
        WMGL_ERR( "this WineMetalGL core cannot place buffers for 32-bit applications (it is older than 3.0.0)\n" );
        ((struct wmgl_init_params *)args)->ok = FALSE;
        return STATUS_SUCCESS;
    }
    set_buffer_memory( low_alloc, low_free );
    return STATUS_SUCCESS;
}

UINT wmgl_wow64_pointer( const void *pointer )
{
    if ((ULONG_PTR)pointer > 0xffffffff)
    {
        WMGL_ERR( "pointer %p is beyond a 32-bit application's reach\n", pointer );
        return 0;
    }
    return PtrToUlong( pointer );
}

/* ---------------------------------------------------------------- strings */

struct string_copy
{
    const char *source;
    char *copy;
};
static struct string_copy *strings;
static unsigned int string_count, string_capacity;

/* The core's strings are constant for its lifetime: one copy each. */
static UINT wow64_string( const GLubyte *source )
{
    struct string_copy *bigger;
    unsigned int i;
    size_t len;
    char *copy;

    if (!source) return 0;
    pthread_mutex_lock( &wow64_lock );
    for (i = 0; i < string_count; i++)
    {
        if (strings[i].source == (const char *)source && !strcmp( strings[i].copy, (const char *)source ))
        {
            pthread_mutex_unlock( &wow64_lock );
            return PtrToUlong( strings[i].copy );
        }
    }
    len = strlen( (const char *)source ) + 1;
    if (!(copy = low_alloc( len )))
    {
        pthread_mutex_unlock( &wow64_lock );
        return 0;
    }
    memcpy( copy, source, len );
    if (string_count == string_capacity)
    {
        string_capacity = string_capacity ? string_capacity * 2 : 64;
        if (!(bigger = realloc( strings, string_capacity * sizeof(*strings) )))
        {
            pthread_mutex_unlock( &wow64_lock );
            return PtrToUlong( copy );
        }
        strings = bigger;
    }
    strings[string_count].source = (const char *)source;
    strings[string_count].copy = copy;
    string_count++;
    pthread_mutex_unlock( &wow64_lock );
    return PtrToUlong( copy );
}

NTSTATUS wow64_manual_glGetString( void *args )
{
    struct glGetString_params32 *params = args;
    params->ret = wow64_string( core.p_glGetString( params->name ) );
    return wmgl_after_call();
}

NTSTATUS wow64_manual_glGetStringi( void *args )
{
    struct glGetStringi_params32 *params = args;
    params->ret = wow64_string( core.p_glGetStringi( params->name, params->index ) );
    return wmgl_after_call();
}

/* ------------------------------------------------------------ sync objects */

static GLsync *syncs;
static unsigned int sync_count;

static UINT sync_handle( GLsync sync )
{
    unsigned int i;
    GLsync *bigger;

    if (!sync) return 0;
    pthread_mutex_lock( &wow64_lock );
    for (i = 0; i < sync_count; i++)
        if (!syncs[i]) break;
    if (i == sync_count)
    {
        if (!(bigger = realloc( syncs, (sync_count + 64) * sizeof(*syncs) )))
        {
            pthread_mutex_unlock( &wow64_lock );
            return 0;
        }
        memset( bigger + sync_count, 0, 64 * sizeof(*bigger) );
        syncs = bigger;
        sync_count += 64;
    }
    syncs[i] = sync;
    pthread_mutex_unlock( &wow64_lock );
    return i + 1;
}

/* The sync object of a handle; an unknown handle stays an invalid sync. */
static GLsync sync_from_handle( UINT handle, BOOL release )
{
    GLsync sync = (GLsync)(ULONG_PTR)0xdeadbeef00000000ull;  /* rejected by the core's lookup */

    if (!handle) return NULL;
    pthread_mutex_lock( &wow64_lock );
    if (handle <= sync_count && syncs[handle - 1])
    {
        sync = syncs[handle - 1];
        if (release) syncs[handle - 1] = NULL;
    }
    pthread_mutex_unlock( &wow64_lock );
    return sync;
}

NTSTATUS wow64_manual_glFenceSync( void *args )
{
    struct glFenceSync_params32 *params = args;
    params->ret = sync_handle( core.p_glFenceSync( params->condition, params->flags ) );
    return wmgl_after_call();
}

NTSTATUS wow64_manual_glDeleteSync( void *args )
{
    struct glDeleteSync_params32 *params = args;
    GLsync sync = sync_from_handle( params->sync, FALSE );
    core.p_glDeleteSync( sync );
    /* The handle goes once the core accepted the sync (a bad one stays invalid). */
    if (sync && core.p_glIsSync( sync ) == 0) sync_from_handle( params->sync, TRUE );
    return wmgl_after_call();
}

NTSTATUS wow64_manual_glIsSync( void *args )
{
    struct glIsSync_params32 *params = args;
    params->ret = core.p_glIsSync( sync_from_handle( params->sync, FALSE ) );
    return wmgl_after_call();
}

NTSTATUS wow64_manual_glClientWaitSync( void *args )
{
    struct glClientWaitSync_params32 *params = args;
    params->ret = core.p_glClientWaitSync( sync_from_handle( params->sync, FALSE ), params->flags, params->timeout );
    return wmgl_after_call();
}

NTSTATUS wow64_manual_glWaitSync( void *args )
{
    struct glWaitSync_params32 *params = args;
    core.p_glWaitSync( sync_from_handle( params->sync, FALSE ), params->flags, params->timeout );
    return wmgl_after_call();
}

NTSTATUS wow64_manual_glGetSynciv( void *args )
{
    struct glGetSynciv_params32 *params = args;
    core.p_glGetSynciv( sync_from_handle( params->sync, FALSE ), params->pname, params->count,
                        (GLsizei *)ULongToPtr( params->length ), (GLint *)ULongToPtr( params->values ) );
    return wmgl_after_call();
}

/* Object pointer labels name sync objects. */
NTSTATUS wow64_manual_glObjectPtrLabel( void *args )
{
    struct glObjectPtrLabel_params32 *params = args;
    core.p_glObjectPtrLabel( (const void *)sync_from_handle( params->ptr, FALSE ), params->length,
                             (const GLchar *)ULongToPtr( params->label ) );
    return wmgl_after_call();
}

NTSTATUS wow64_manual_glGetObjectPtrLabel( void *args )
{
    struct glGetObjectPtrLabel_params32 *params = args;
    core.p_glGetObjectPtrLabel( (const void *)sync_from_handle( params->ptr, FALSE ), params->bufSize,
                                (GLsizei *)ULongToPtr( params->length ), (GLchar *)ULongToPtr( params->label ) );
    return wmgl_after_call();
}

/* ------------------------------------------------------- pointer arrays */

/* `count` 32-bit pointers at `address`, widened (NULL for a NULL array). */
static void **widen_pointers( UINT address, GLsizei count )
{
    const UINT *source = ULongToPtr( address );
    void **out;
    GLsizei i;

    if (!source || count <= 0) return NULL;
    if (!(out = malloc( count * sizeof(*out) ))) return NULL;
    for (i = 0; i < count; i++) out[i] = ULongToPtr( source[i] );
    return out;
}

NTSTATUS wow64_manual_glShaderSource( void *args )
{
    struct glShaderSource_params32 *params = args;
    void **strings = widen_pointers( params->string, params->count );
    core.p_glShaderSource( params->shader, params->count, (const GLchar *const *)strings,
                           (const GLint *)ULongToPtr( params->length ) );
    free( strings );
    return wmgl_after_call();
}

NTSTATUS wow64_manual_glShaderSourceARB( void *args )
{
    struct glShaderSourceARB_params32 *params = args;
    void **strings = widen_pointers( params->string, params->count );
    core.p_glShaderSourceARB( params->shaderObj, params->count, (const GLcharARB **)strings,
                              (const GLint *)ULongToPtr( params->length ) );
    free( strings );
    return wmgl_after_call();
}

NTSTATUS wow64_manual_glTransformFeedbackVaryings( void *args )
{
    struct glTransformFeedbackVaryings_params32 *params = args;
    void **names = widen_pointers( params->varyings, params->count );
    core.p_glTransformFeedbackVaryings( params->program, params->count, (const GLchar *const *)names,
                                        params->bufferMode );
    free( names );
    return wmgl_after_call();
}

NTSTATUS wow64_manual_glTransformFeedbackVaryingsEXT( void *args )
{
    struct glTransformFeedbackVaryingsEXT_params32 *params = args;
    void **names = widen_pointers( params->varyings, params->count );
    core.p_glTransformFeedbackVaryingsEXT( params->program, params->count, (const GLchar *const *)names,
                                           params->bufferMode );
    free( names );
    return wmgl_after_call();
}

NTSTATUS wow64_manual_glGetUniformIndices( void *args )
{
    struct glGetUniformIndices_params32 *params = args;
    void **names = widen_pointers( params->uniformNames, params->uniformCount );
    core.p_glGetUniformIndices( params->program, params->uniformCount, (const GLchar *const *)names,
                                (GLuint *)ULongToPtr( params->uniformIndices ) );
    free( names );
    return wmgl_after_call();
}

NTSTATUS wow64_manual_glCreateShaderProgramv( void *args )
{
    struct glCreateShaderProgramv_params32 *params = args;
    void **strings = widen_pointers( params->strings, params->count );
    params->ret = core.p_glCreateShaderProgramv( params->type, params->count, (const GLchar *const *)strings );
    free( strings );
    return wmgl_after_call();
}

NTSTATUS wow64_manual_glMultiDrawElements( void *args )
{
    struct glMultiDrawElements_params32 *params = args;
    void **indices = widen_pointers( params->indices, params->drawcount );
    core.p_glMultiDrawElements( params->mode, (const GLsizei *)ULongToPtr( params->count ), params->type,
                                (const void *const *)indices, params->drawcount );
    free( indices );
    return wmgl_after_call();
}

NTSTATUS wow64_manual_glMultiDrawElementsEXT( void *args )
{
    struct glMultiDrawElementsEXT_params32 *params = args;
    void **indices = widen_pointers( params->indices, params->primcount );
    core.p_glMultiDrawElementsEXT( params->mode, (const GLsizei *)ULongToPtr( params->count ), params->type,
                                   (const void *const *)indices, params->primcount );
    free( indices );
    return wmgl_after_call();
}

NTSTATUS wow64_manual_glMultiDrawElementsBaseVertex( void *args )
{
    struct glMultiDrawElementsBaseVertex_params32 *params = args;
    void **indices = widen_pointers( params->indices, params->drawcount );
    core.p_glMultiDrawElementsBaseVertex( params->mode, (const GLsizei *)ULongToPtr( params->count ), params->type,
                                          (const void *const *)indices, params->drawcount,
                                          (const GLint *)ULongToPtr( params->basevertex ) );
    free( indices );
    return wmgl_after_call();
}

/* ---------------------------------------------- pointers through pointers */

static void narrow_pointer( UINT out, void *pointer )
{
    if (out) *(UINT *)ULongToPtr( out ) = wmgl_wow64_pointer( pointer );
}

NTSTATUS wow64_manual_glGetVertexAttribPointerv( void *args )
{
    struct glGetVertexAttribPointerv_params32 *params = args;
    void *pointer = NULL;
    core.p_glGetVertexAttribPointerv( params->index, params->pname, &pointer );
    narrow_pointer( params->pointer, pointer );
    return wmgl_after_call();
}

NTSTATUS wow64_manual_glGetVertexAttribPointervARB( void *args )
{
    struct glGetVertexAttribPointervARB_params32 *params = args;
    void *pointer = NULL;
    core.p_glGetVertexAttribPointervARB( params->index, params->pname, &pointer );
    narrow_pointer( params->pointer, pointer );
    return wmgl_after_call();
}

NTSTATUS wow64_manual_glGetBufferPointerv( void *args )
{
    struct glGetBufferPointerv_params32 *params = args;
    void *pointer = NULL;
    core.p_glGetBufferPointerv( params->target, params->pname, &pointer );
    narrow_pointer( params->params, pointer );
    return wmgl_after_call();
}

NTSTATUS wow64_manual_glGetBufferPointervARB( void *args )
{
    struct glGetBufferPointervARB_params32 *params = args;
    void *pointer = NULL;
    core.p_glGetBufferPointervARB( params->target, params->pname, &pointer );
    narrow_pointer( params->params, pointer );
    return wmgl_after_call();
}

NTSTATUS wow64_manual_glGetNamedBufferPointerv( void *args )
{
    struct glGetNamedBufferPointerv_params32 *params = args;
    void *pointer = NULL;
    core.p_glGetNamedBufferPointerv( params->buffer, params->pname, &pointer );
    narrow_pointer( params->params, pointer );
    return wmgl_after_call();
}

#endif /* _WIN64 */
