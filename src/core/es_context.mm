// OpenGL ES contexts (WGL_EXT_create_context_es_profile): their version
// strings and extension list. They run on the desktop core profile, with
// the OpenGL ES rules applied where the two differ (Context::isEs).

#include "core/context.h"

#include <cstdio>

namespace wmgl {

void Context::initEsStrings() {
    const int es = esVersion();
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), "OpenGL ES %d.%d WineMetalGL", config_.esMajor, config_.esMinor);
    versionString_ = buffer;
    // OpenGL ES 2.0's language is GLSL ES 1.00; 3.N's is GLSL ES 3.N0.
    if (es >= 30) std::snprintf(buffer, sizeof(buffer), "OpenGL ES GLSL ES %d.%d0", config_.esMajor, config_.esMinor);
    else std::snprintf(buffer, sizeof(buffer), "OpenGL ES GLSL ES 1.00");
    glslVersionString_ = buffer;

    extensions_.clear();
    auto add = [&](std::initializer_list<const char*> names) {
        for (const char* name : names) extensions_.push_back(name);
    };
    // Extensions to OpenGL ES 2.0 that OpenGL ES 3.0 made core: programs
    // written for ES 2.0 test for them in any context.
    add({"GL_OES_element_index_uint", "GL_OES_packed_depth_stencil", "GL_OES_depth24", "GL_OES_depth32",
         "GL_OES_depth_texture", "GL_OES_rgb8_rgba8", "GL_OES_standard_derivatives", "GL_OES_vertex_array_object",
         "GL_OES_vertex_half_float", "GL_OES_texture_npot", "GL_OES_mapbuffer", "GL_EXT_map_buffer_range",
         "GL_OES_texture_float", "GL_OES_texture_half_float", "GL_OES_texture_float_linear",
         "GL_OES_texture_half_float_linear", "GL_OES_fbo_render_mipmap", "GL_OES_surfaceless_context",
         "GL_OES_get_program_binary", "GL_OES_texture_3D", "GL_OES_required_internalformat",
         "GL_OES_vertex_type_10_10_10_2", "GL_EXT_blend_minmax", "GL_EXT_texture_rg", "GL_EXT_sRGB",
         "GL_EXT_texture_type_2_10_10_10_REV", "GL_EXT_occlusion_query_boolean", "GL_EXT_instanced_arrays",
         "GL_EXT_draw_instanced", "GL_EXT_texture_storage", "GL_EXT_draw_buffers", "GL_EXT_shader_texture_lod",
         "GL_EXT_frag_depth", "GL_EXT_discard_framebuffer", "GL_EXT_multi_draw_arrays", "GL_EXT_unpack_subimage",
         "GL_NV_pack_subimage", "GL_EXT_read_format_bgra", "GL_EXT_texture_format_BGRA8888",
         "GL_EXT_debug_marker", "GL_EXT_debug_label", "GL_EXT_separate_shader_objects"});
    // Formats and compression.
    add({"GL_EXT_color_buffer_half_float", "GL_EXT_texture_compression_s3tc", "GL_EXT_texture_compression_s3tc_srgb",
         "GL_EXT_texture_compression_rgtc", "GL_EXT_texture_compression_bptc", "GL_EXT_texture_filter_anisotropic",
         "GL_OES_compressed_ETC1_RGB8_texture", "GL_EXT_texture_sRGB_decode", "GL_EXT_texture_sRGB_R8",
         "GL_EXT_texture_sRGB_RG8", "GL_EXT_texture_norm16", "GL_EXT_render_snorm", "GL_EXT_texture_mirror_clamp_to_edge",
         "GL_EXT_depth_clamp", "GL_EXT_texture_shadow_lod"});
    if (es >= 30) {
        add({"GL_EXT_color_buffer_float", "GL_EXT_float_blend", "GL_OES_texture_stencil8", "GL_EXT_copy_image",
             "GL_OES_copy_image", "GL_EXT_draw_buffers_indexed", "GL_OES_draw_buffers_indexed",
             "GL_EXT_draw_elements_base_vertex", "GL_OES_draw_elements_base_vertex", "GL_EXT_base_instance",
             "GL_EXT_buffer_storage", "GL_EXT_clip_control", "GL_EXT_clip_cull_distance", "GL_EXT_polygon_offset_clamp",
             "GL_EXT_texture_border_clamp", "GL_OES_texture_border_clamp", "GL_EXT_shader_integer_mix",
             "GL_EXT_disjoint_timer_query", "GL_EXT_robustness", "GL_KHR_robustness",
             "GL_KHR_robust_buffer_access_behavior", "GL_KHR_debug", "GL_KHR_no_error", "GL_KHR_context_flush_control",
             "GL_KHR_parallel_shader_compile", "GL_EXT_texture_view", "GL_OES_texture_view"});
    } else {
        add({"GL_KHR_debug", "GL_EXT_robustness", "GL_KHR_robustness"});
    }
    // The OpenGL ES 3.1 extensions OpenGL ES 3.2 made core (the Android
    // extension pack's), and ones that build on 3.1.
    if (es >= 31)
        add({"GL_EXT_geometry_shader", "GL_OES_geometry_shader", "GL_EXT_tessellation_shader",
             "GL_OES_tessellation_shader", "GL_EXT_gpu_shader5", "GL_OES_gpu_shader5", "GL_EXT_shader_io_blocks",
             "GL_OES_shader_io_blocks", "GL_EXT_texture_buffer", "GL_OES_texture_buffer", "GL_EXT_texture_cube_map_array",
             "GL_OES_texture_cube_map_array", "GL_OES_sample_shading", "GL_OES_sample_variables",
             "GL_OES_shader_multisample_interpolation", "GL_OES_shader_image_atomic",
             "GL_OES_texture_storage_multisample_2d_array", "GL_EXT_primitive_bounding_box",
             "GL_OES_primitive_bounding_box", "GL_OES_viewport_array", "GL_EXT_multi_draw_indirect",
             "GL_EXT_geometry_point_size", "GL_OES_geometry_point_size", "GL_EXT_tessellation_point_size",
             "GL_OES_tessellation_point_size"});
}

}  // namespace wmgl
