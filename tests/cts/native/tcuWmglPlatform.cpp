/* VK-GL-CTS platform port that runs the OpenGL tests natively on macOS
 * against the WineMetalGL core (libwinemetalgl.dylib built with
 * -DWMGL_HOST_ONLY=ON), without Wine. Every context renders to a pbuffer
 * surface, which is the default framebuffer the tests see.
 *
 * Installed into a CTS checkout by tests/cts/native/build.sh.
 */

#include "deUniquePtr.hpp"
#include "gluDefs.hpp"
#include "gluPlatform.hpp"
#include "gluRenderConfig.hpp"
#include "gluRenderContext.hpp"
#include "glwEnums.hpp"
#include "glwFunctions.hpp"
#include "glwInitFunctions.hpp"
#include "tcuCommandLine.hpp"
#include "tcuDefs.hpp"
#include "tcuPlatform.hpp"
#include "tcuRenderTarget.hpp"

#include <vector>

#include "winemetalgl/driver.h"

namespace tcu
{
namespace wmgl
{
namespace
{

class Loader : public glw::FunctionLoader
{
public:
    glw::GenericFuncType get(const char *name) const
    {
        return reinterpret_cast<glw::GenericFuncType>(wmgl_get_proc_address(name));
    }
};

bool matches(int want, int have)
{
    return want == glu::RenderConfig::DONT_CARE || want == have;
}

// 1-based pixel format for the configuration, or 0.
int chooseFormat(const glu::RenderConfig &config, wmgl_pixel_format &chosen)
{
    std::vector<wmgl_pixel_format> formats(512);
    unsigned int onscreen = 0;
    const unsigned int count = wmgl_get_pixel_formats(formats.data(), (unsigned int)formats.size(), &onscreen);
    if (config.id != glu::RenderConfig::DONT_CARE)
    {
        if (config.id < 1 || (unsigned int)config.id > count)
            return 0;
        chosen = formats[config.id - 1];
        return chosen.draw_to_pbuffer ? config.id : 0;
    }
    const bool wantFloat = config.componentType == glu::RenderConfig::COMPONENT_TYPE_FLOAT;
    for (unsigned int i = 0; i < count && i < formats.size(); ++i)
    {
        const wmgl_pixel_format &f = formats[i];
        if (!f.draw_to_pbuffer || f.srgb_capable || (f.float_components != 0) != wantFloat)
            continue;
        // DONT_CARE prefers RGBA8 with depth and stencil and no multisampling.
        const int red = config.redBits == glu::RenderConfig::DONT_CARE ? 8 : config.redBits;
        const int alpha = config.alphaBits == glu::RenderConfig::DONT_CARE ? 8 : config.alphaBits;
        const int depth = config.depthBits == glu::RenderConfig::DONT_CARE ? 24 : config.depthBits;
        const int stencil = config.stencilBits == glu::RenderConfig::DONT_CARE ? 8 : config.stencilBits;
        const int samples = config.numSamples == glu::RenderConfig::DONT_CARE ? 0 : config.numSamples;
        if (f.red_bits == red && matches(config.greenBits, f.green_bits) && matches(config.blueBits, f.blue_bits) &&
            f.alpha_bits == alpha && f.depth_bits == depth && f.stencil_bits == stencil &&
            (f.sample_buffers ? f.samples : 0) == samples)
        {
            chosen = f;
            return (int)i + 1;
        }
    }
    return 0;
}

class RenderContext : public glu::RenderContext
{
public:
    RenderContext(const glu::RenderConfig &config, const tcu::CommandLine &cmdLine, const RenderContext *shared);
    ~RenderContext(void);

    glu::ContextType getType(void) const
    {
        return m_type;
    }
    const glw::Functions &getFunctions(void) const
    {
        return m_functions;
    }
    const tcu::RenderTarget &getRenderTarget(void) const
    {
        return m_renderTarget;
    }
    void postIterate(void)
    {
        wmgl_surface_swap(m_surface);
    }
    glw::GenericFuncType getProcAddress(const char *name) const
    {
        return reinterpret_cast<glw::GenericFuncType>(wmgl_get_proc_address(name));
    }
    void makeCurrent(void)
    {
        wmgl_make_current(m_context, m_surface, m_surface);
    }

    wmgl_context *handle(void) const
    {
        return m_context;
    }

private:
    glu::ContextType m_type;
    wmgl_context *m_context = nullptr;
    wmgl_surface *m_surface = nullptr;
    glw::Functions m_functions;
    tcu::RenderTarget m_renderTarget;
};

RenderContext::RenderContext(const glu::RenderConfig &config, const tcu::CommandLine &cmdLine,
                             const RenderContext *shared)
    : m_type(config.type)
    , m_renderTarget(0, 0, tcu::PixelFormat(0, 0, 0, 0), 0, 0, 0)
{
    (void)cmdLine;
    if (wmgl_initialize(WMGL_DRIVER_ABI_VERSION) != 0)
        throw tcu::NotSupportedError("WineMetalGL failed to initialise");
    if (glu::isContextTypeES(config.type))
        throw tcu::NotSupportedError("OpenGL ES is not supported");
    wmgl_pixel_format pf;
    const int format = chooseFormat(config, pf);
    if (!format)
        throw tcu::NotSupportedError("No matching WineMetalGL pixel format");

    const int width  = config.width == glu::RenderConfig::DONT_CARE ? 256 : config.width;
    const int height = config.height == glu::RenderConfig::DONT_CARE ? 256 : config.height;
    m_surface        = wmgl_surface_create_pbuffer(format, width, height);
    if (!m_surface)
        throw tcu::ResourceError("Failed to create pbuffer");

    const glu::ApiType api = config.type.getAPI();
    std::vector<int> attribs;
    attribs.push_back(0x2091); // WGL_CONTEXT_MAJOR_VERSION_ARB
    attribs.push_back(api.getMajorVersion());
    attribs.push_back(0x2092); // WGL_CONTEXT_MINOR_VERSION_ARB
    attribs.push_back(api.getMinorVersion());
    if (api.getMajorVersion() > 3 || (api.getMajorVersion() == 3 && api.getMinorVersion() >= 2))
    {
        attribs.push_back(0x9126); // WGL_CONTEXT_PROFILE_MASK_ARB
        attribs.push_back(api.getProfile() == glu::PROFILE_COMPATIBILITY ? 2 : 1);
    }
    int flags                       = 0;
    const glu::ContextFlags ctxFlags = config.type.getFlags();
    if (ctxFlags & glu::CONTEXT_DEBUG)
        flags |= 0x0001; // WGL_CONTEXT_DEBUG_BIT_ARB
    if (ctxFlags & glu::CONTEXT_FORWARD_COMPATIBLE)
        flags |= 0x0002; // WGL_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB
    if (ctxFlags & glu::CONTEXT_ROBUST)
        flags |= 0x0004; // WGL_CONTEXT_ROBUST_ACCESS_BIT_ARB
    if (flags)
    {
        attribs.push_back(0x2094); // WGL_CONTEXT_FLAGS_ARB
        attribs.push_back(flags);
    }
    if (config.resetNotificationStrategy != glu::RESET_NOTIFICATION_STRATEGY_NOT_SPECIFIED)
    {
        attribs.push_back(0x8256); // WGL_CONTEXT_RESET_NOTIFICATION_STRATEGY_ARB
        attribs.push_back(config.resetNotificationStrategy == glu::RESET_NOTIFICATION_STRATEGY_LOSE_CONTEXT_ON_RESET ?
                              0x8252 /* LOSE_CONTEXT_ON_RESET */ :
                              0x8261 /* NO_RESET_NOTIFICATION */);
    }
    if (ctxFlags & glu::CONTEXT_NO_ERROR)
    {
        attribs.push_back(0x31B3); // WGL_CONTEXT_OPENGL_NO_ERROR_ARB
        attribs.push_back(1);
    }
    attribs.push_back(0);
    int error = 0;
    m_context = wmgl_context_create(format, shared ? shared->handle() : nullptr, attribs.data(), &error);
    if (!m_context)
    {
        wmgl_surface_destroy(m_surface);
        throw tcu::NotSupportedError("WineMetalGL cannot create the requested context");
    }
    if (!wmgl_make_current(m_context, m_surface, m_surface))
    {
        wmgl_context_destroy(m_context);
        wmgl_surface_destroy(m_surface);
        throw tcu::ResourceError("wmgl_make_current failed");
    }
    Loader loader;
    glu::initFunctions(&m_functions, &loader, api);
    m_renderTarget = tcu::RenderTarget(width, height,
                                       tcu::PixelFormat(pf.red_bits, pf.green_bits, pf.blue_bits, pf.alpha_bits),
                                       pf.depth_bits, pf.stencil_bits, pf.sample_buffers ? pf.samples : 0);
}

RenderContext::~RenderContext(void)
{
    wmgl_make_current(nullptr, nullptr, nullptr);
    if (m_context)
        wmgl_context_destroy(m_context);
    if (m_surface)
        wmgl_surface_destroy(m_surface);
}

class ContextFactory : public glu::ContextFactory
{
public:
    ContextFactory(void) : glu::ContextFactory("wmgl", "WineMetalGL core on a pbuffer")
    {
    }
    glu::RenderContext *createContext(const glu::RenderConfig &config, const tcu::CommandLine &cmdLine,
                                      const glu::RenderContext *shared) const
    {
        return new RenderContext(config, cmdLine, static_cast<const RenderContext *>(shared));
    }
};

class GLPlatform : public glu::Platform
{
public:
    GLPlatform(void)
    {
        m_contextFactoryRegistry.registerFactory(new ContextFactory());
    }
};

class Platform : public tcu::Platform
{
public:
    const glu::Platform &getGLPlatform(void) const
    {
        return m_glPlatform;
    }

private:
    GLPlatform m_glPlatform;
};

} // namespace
} // namespace wmgl
} // namespace tcu

tcu::Platform *createPlatform(void)
{
    return new tcu::wmgl::Platform();
}
