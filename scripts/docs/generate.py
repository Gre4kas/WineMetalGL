#!/usr/bin/env python3
"""Generates WineMetalGL's published claims from committed evidence.

    scripts/docs/generate.py [--check] [--check-provenance]

Inputs (all tracked):
  tests/cts/baseline/glMN-cfgN-*.txt   KHR-GL30 to KHR-GL43 results (scripts/cts/run.py)
  tests/cts/recheck/glMN-cfgN-*.txt    later runs of a baseline's non-Pass cases, which replace its results
  tests/cts/notsupported.json          reason for every NotSupported case
  tests/cts/known-failures.json        reasons for failing cases the core does not fix
  tests/games/corpus.json              game corpus manifest
  tests/games/results.json             last corpus run (scripts/traces/replay.py --json)
  tests/games/perf.json                frame times (scripts/traces/benchmark.py)
  third_party/khronos/gl.xml           the GL 4.3 core command list
  src/core                             advertised extensions, implemented commands

Outputs:
  docs/conformance.md, docs/performance.md, docs/games.md,
  docs/api-coverage.json, and the sections between GENERATED markers in
  README.md, docs/unsupported.md and docs/testing.md.

--check exits non-zero if any output differs from what is committed;
--check-provenance if any evidence was recorded from uncommitted changes.
"""

import argparse
import fnmatch
import glob
import json
import math
import os
import re
import sys
import xml.etree.ElementTree as ET

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts", "cts"))
import results as cts  # noqa: E402

BEGIN = "<!-- BEGIN GENERATED: %s (scripts/docs/generate.py) -->"
END = "<!-- END GENERATED: %s -->"
STATUSES = ["Pass", "Fail", "NotSupported", "InternalError", "Crash", "Timeout", "Missing"]
# Mustpass lists with committed baselines (tests/cts/baseline/<list>-cfgN-<runner>.txt).
SUITES = [("gl30", "KHR-GL30"), ("gl31", "KHR-GL31"), ("gl32", "KHR-GL32"), ("gl33", "KHR-GL33"),
          ("gl40", "KHR-GL40"), ("gl41", "KHR-GL41"), ("gl42", "KHR-GL42"), ("gl43", "KHR-GL43"),
          ("gl46", "KHR-GL46")]
# OpenGL ES mustpass lists (Khronos's gles*-khr-* and dEQP's aosp_mustpass lists), each run in
# configuration 1 of its mustpass.xml under Wine: (list, what the list runs, OpenGL ES version).
ES_SUITES = [
    ("gles2-khr-main", "KHR-GLES2", "2.0"),
    ("gles2-khr-noctx-main", "KHR-NoContext.es2", "2.0"),
    ("gles2-main", "dEQP-GLES2", "2.0"),
    ("gles3-khr-main", "KHR-GLES3", "3.0"),
    ("gles3-main", "dEQP-GLES3", "3.0"),
    ("gles3-rotate-portrait", "dEQP-GLES3, screen rotation 0", "3.0"),
    ("gles3-rotate-landscape", "dEQP-GLES3, screen rotation 90", "3.0"),
    ("gles3-rotate-reverse-portrait", "dEQP-GLES3, screen rotation 180", "3.0"),
    ("gles3-rotate-reverse-landscape", "dEQP-GLES3, screen rotation 270", "3.0"),
    ("gles3-multisample", "dEQP-GLES3, 4 samples", "3.0"),
    ("gles3-565-no-depth-no-stencil", "dEQP-GLES3, RGB565 without depth and stencil", "3.0"),
    ("gles31-khr-main", "KHR-GLES31", "3.1"),
    ("gles31-khr-single", "KHR-Single-GLES31", "3.1"),
    ("gles31-main", "dEQP-GLES31", "3.1"),
    ("gles31-rotate-portrait", "dEQP-GLES31, screen rotation 0", "3.1"),
    ("gles31-rotate-landscape", "dEQP-GLES31, screen rotation 90", "3.1"),
    ("gles31-rotate-reverse-portrait", "dEQP-GLES31, screen rotation 180", "3.1"),
    ("gles31-rotate-reverse-landscape", "dEQP-GLES31, screen rotation 270", "3.1"),
    ("gles31-multisample", "dEQP-GLES31, 4 samples", "3.1"),
    ("gles31-565-no-depth-no-stencil", "dEQP-GLES31, RGB565 without depth and stencil", "3.1"),
    ("gles32-khr-main", "KHR-GLES32", "3.2"),
    ("gles32-khr-noctx-main", "KHR-NoContext.es32", "3.2"),
    ("gles32-khr-single", "KHR-Single-GLES32", "3.2"),
    ("gles32-khr-glesext", "KHR-GLESEXT", "3.2"),
]
ES_VERSIONS = [
    ("2.0", "GLSL ES 1.00, framebuffer objects, the ES 2.0 extensions (float and depth textures, ...)"),
    ("3.0", "GLSL ES 3.00, transform feedback, instancing, integer and sRGB formats, multisample renderbuffers"),
    ("3.1", "Compute shaders, images, storage buffers, indirect draws, separate programs, multisample textures"),
    ("3.2", "Geometry and tessellation shaders, advanced blending, ASTC, texture buffers, debug output"),
]
# The summary of the last cts-runner run over every OpenGL ES configuration (tests/cts/es/).
ES_RUNNER_SUMMARY = "tests/cts/es/cts-runner-es3.txt"
# The highest version contexts get (src/core/driver.mm).
MAX_VERSION = "4.6"
# mustpass.xml configurations that are run. Configurations 5 and 6 repeat the command
# lines of 1 and 2: their only difference, useForFirstEGLConfig, applies to EGL.
CONFIGS = (1, 2, 3, 4)
CONFIGS_NOTE = ("configurations 1 to 4 of `mustpass.xml` (5 and 6 repeat the command lines of 1 and 2; their only "
                "difference, `useForFirstEGLConfig`, applies to EGL)")
# Lists run in fewer configurations: (configuration, runner) pairs. KHR-GL46, the
# largest list, runs configuration 1 natively and configurations 1 and 3 (an FBO
# surface) under Wine.
SUITE_RUNS = {"gl46": {(1, "native"), (1, "wine"), (3, "wine")}}
SUITE_RUNS.update({suite: {(1, "wine")} for suite, _, _ in ES_SUITES})
# A list that also measures earlier versions: their rows say so.
COVERED_BY = "gl46"
# Conformance totals rows: (version, what the version adds, mustpass list that measures it).
# OpenGL 1.0 to 2.1 are the compatibility profile, which no public Khronos suite measures.
VERSIONS = [
    ("3.0", "GLSL 1.30, framebuffer objects, vertex array objects, transform feedback, integer textures", "gl30"),
    ("3.1", "Instancing, texture buffers, uniform buffers, primitive restart", "gl31"),
    ("3.2", "Profiles, geometry shaders, sync objects, multisample textures, base vertex", "gl32"),
    ("3.3", "GLSL 3.30, sampler objects, timer queries, instanced arrays, dual-source blending", "gl33"),
    ("4.0", "Tessellation, GLSL 4.00, double precision, indirect draws, per-sample shading", "gl40"),
    ("4.1", "Separate shader objects, program binaries, viewport arrays, 64-bit attributes", "gl41"),
    ("4.2", "Image load/store, atomic counters, immutable texture storage", "gl42"),
    ("4.3", "Compute shaders, shader storage buffers, debug output, texture views", "gl43"),
    ("4.4", "Buffer storage, multi-bind, texture clears, enhanced layouts", "covered"),
    ("4.5", "Direct state access, clip control, robustness, cull distances", "covered"),
    ("4.6", "SPIR-V shaders, anisotropic filtering, indirect draw counts", "gl46"),
]
# Extensions each 4.x version promoted to core (the version's "new features" appendix).
PROMOTED = {
    "4.0": ["GL_ARB_draw_buffers_blend", "GL_ARB_draw_indirect", "GL_ARB_gpu_shader5", "GL_ARB_gpu_shader_fp64",
            "GL_ARB_sample_shading", "GL_ARB_shader_subroutine", "GL_ARB_tessellation_shader",
            "GL_ARB_texture_buffer_object_rgb32", "GL_ARB_texture_cube_map_array", "GL_ARB_texture_gather",
            "GL_ARB_texture_query_lod", "GL_ARB_transform_feedback2", "GL_ARB_transform_feedback3"],
    "4.1": ["GL_ARB_ES2_compatibility", "GL_ARB_get_program_binary", "GL_ARB_separate_shader_objects",
            "GL_ARB_shader_precision", "GL_ARB_vertex_attrib_64bit", "GL_ARB_viewport_array"],
    "4.2": ["GL_ARB_base_instance", "GL_ARB_compressed_texture_pixel_storage", "GL_ARB_conservative_depth",
            "GL_ARB_internalformat_query", "GL_ARB_map_buffer_alignment", "GL_ARB_shader_atomic_counters",
            "GL_ARB_shader_image_load_store", "GL_ARB_shading_language_420pack", "GL_ARB_shading_language_packing",
            "GL_ARB_texture_compression_bptc", "GL_ARB_texture_storage", "GL_ARB_transform_feedback_instanced"],
    "4.3": ["GL_ARB_arrays_of_arrays", "GL_ARB_clear_buffer_object", "GL_ARB_compute_shader", "GL_ARB_copy_image",
            "GL_ARB_ES3_compatibility", "GL_ARB_explicit_uniform_location", "GL_ARB_fragment_layer_viewport",
            "GL_ARB_framebuffer_no_attachments", "GL_ARB_internalformat_query2", "GL_ARB_invalidate_subdata",
            "GL_ARB_multi_draw_indirect", "GL_ARB_program_interface_query", "GL_ARB_robust_buffer_access_behavior",
            "GL_ARB_shader_image_size", "GL_ARB_shader_storage_buffer_object", "GL_ARB_stencil_texturing",
            "GL_ARB_texture_buffer_range", "GL_ARB_texture_query_levels", "GL_ARB_texture_storage_multisample",
            "GL_ARB_texture_view", "GL_ARB_vertex_attrib_binding", "GL_KHR_debug"],
    "4.4": ["GL_ARB_buffer_storage", "GL_ARB_clear_texture", "GL_ARB_enhanced_layouts", "GL_ARB_multi_bind",
            "GL_ARB_query_buffer_object", "GL_ARB_texture_mirror_clamp_to_edge", "GL_ARB_texture_stencil8",
            "GL_ARB_vertex_type_10f_11f_11f_rev"],
    "4.5": ["GL_ARB_clip_control", "GL_ARB_conditional_render_inverted", "GL_ARB_cull_distance",
            "GL_ARB_derivative_control", "GL_ARB_direct_state_access", "GL_ARB_ES3_1_compatibility",
            "GL_ARB_get_texture_sub_image", "GL_ARB_shader_texture_image_samples", "GL_ARB_texture_barrier",
            "GL_KHR_context_flush_control", "GL_KHR_robustness"],
    "4.6": ["GL_ARB_gl_spirv", "GL_ARB_indirect_parameters", "GL_ARB_pipeline_statistics_query",
            "GL_ARB_polygon_offset_clamp", "GL_ARB_shader_atomic_counter_ops", "GL_ARB_shader_draw_parameters",
            "GL_ARB_shader_group_vote", "GL_ARB_spirv_extensions", "GL_ARB_texture_filter_anisotropic",
            "GL_ARB_transform_feedback_overflow_query", "GL_KHR_no_error"],
}

# Features each 3.x version made core: every context offers them.
CORE_FEATURES = {
    "3.0": ["GLSL 1.30", "Framebuffer objects", "Vertex array objects", "Transform feedback", "Integer textures",
            "Floating-point textures and color buffers", "Depth float buffers", "Half-float pixels and vertices",
            "Buffer range mapping", "Red and red-green textures", "RGTC compressed textures", "Texture arrays",
            "Packed float and shared exponent formats", "sRGB framebuffers", "Conditional rendering",
            "Per-buffer color masks and blending"],
    "3.1": ["GLSL 1.40", "Instanced draws", "Texture buffers", "Uniform buffers", "Primitive restart",
            "Buffer copies", "Rectangle textures", "Signed normalized textures"],
    "3.2": ["GLSL 1.50", "Core and compatibility profiles", "Geometry shaders", "Sync objects",
            "Multisample textures", "Base vertex draws", "Provoking vertex control", "Seamless cube maps",
            "Depth clamping", "Fragment coordinate conventions"],
    "3.3": ["GLSL 3.30", "Sampler objects", "Timer queries", "Instanced arrays", "Dual-source blending",
            "Explicit attribute locations", "Boolean occlusion queries", "Shader bit encoding",
            "RGB10_A2UI textures", "Texture swizzles", "Packed 2_10_10_10 vertex attributes"],
}
# Optional features WineMetalGL does not offer, by the version whose feature they extend: the
# NotSupported cases of the KHR-GL lists (tests/cts/notsupported.json). (version, feature, reason)
GL_NOT_OFFERED = [
    ("3.2", "More than 4 samples", "`GL_MAX_SAMPLES` is 4, the most every color and depth format takes on Apple GPUs"),
    ("4.2", "Multisample images (`GL_MAX_IMAGE_SAMPLES` above 0)",
     "Metal shaders cannot write multisample textures; 0 is the GL minimum"),
    ("4.6", "Sparse textures and buffers (`GL_ARB_sparse_texture`, `GL_ARB_sparse_texture2`, "
     "`GL_ARB_sparse_texture_clamp`, `GL_ARB_sparse_buffer`)", "Not advertised"),
    ("4.6", "Advanced blend equations (`GL_KHR_blend_equation_advanced`)",
     "Not advertised in OpenGL contexts (OpenGL ES contexts have it)"),
    ("4.6", "Shader ballot and 64-bit integers (`GL_ARB_shader_ballot`, `GL_ARB_gpu_shader_int64`)", "Not advertised"),
    ("4.6", "Min/max texture filtering (`GL_ARB_texture_filter_minmax`)", "Not advertised"),
    ("4.6", "Post-depth coverage (`GL_ARB_post_depth_coverage`)",
     "Metal has no post-depth-test sample coverage input"),
    ("4.6", "Fragment shading rate (`GL_EXT_fragment_shading_rate`)", "Not advertised"),
    ("4.6", "ASTC compressed textures (`GL_KHR_texture_compression_astc_ldr`)",
     "Not advertised in OpenGL contexts (OpenGL ES contexts have it)"),
]
# Names for the extensions PROMOTED lists, for the feature tables.
FEATURE_NAMES = {
    "GL_ARB_draw_buffers_blend": "Per-buffer blend equations", "GL_ARB_draw_indirect": "Indirect draws",
    "GL_ARB_gpu_shader5": "GLSL 4.00 shader features (gpu_shader5)", "GL_ARB_gpu_shader_fp64": "Double precision",
    "GL_ARB_sample_shading": "Per-sample shading", "GL_ARB_shader_subroutine": "Shader subroutines",
    "GL_ARB_tessellation_shader": "Tessellation shaders", "GL_ARB_texture_buffer_object_rgb32": "RGB32 texture buffers",
    "GL_ARB_texture_cube_map_array": "Cube map arrays", "GL_ARB_texture_gather": "Texture gather",
    "GL_ARB_texture_query_lod": "Texture LOD queries", "GL_ARB_transform_feedback2": "Transform feedback objects",
    "GL_ARB_transform_feedback3": "Multiple transform feedback streams",
    "GL_ARB_ES2_compatibility": "OpenGL ES 2.0 compatibility", "GL_ARB_get_program_binary": "Program binaries",
    "GL_ARB_separate_shader_objects": "Separate shader objects", "GL_ARB_shader_precision": "Shader precision rules",
    "GL_ARB_vertex_attrib_64bit": "64-bit vertex attributes", "GL_ARB_viewport_array": "Viewport arrays",
    "GL_ARB_base_instance": "Base instance draws",
    "GL_ARB_compressed_texture_pixel_storage": "Compressed texture pixel storage",
    "GL_ARB_conservative_depth": "Conservative depth", "GL_ARB_internalformat_query": "Internal format queries",
    "GL_ARB_map_buffer_alignment": "Mapped buffer alignment", "GL_ARB_shader_atomic_counters": "Atomic counters",
    "GL_ARB_shader_image_load_store": "Image load/store", "GL_ARB_shading_language_420pack": "GLSL 4.20 syntax (420pack)",
    "GL_ARB_shading_language_packing": "Shader packing functions",
    "GL_ARB_texture_compression_bptc": "BPTC compressed textures", "GL_ARB_texture_storage": "Immutable texture storage",
    "GL_ARB_transform_feedback_instanced": "Instanced transform feedback draws",
    "GL_ARB_arrays_of_arrays": "Arrays of arrays", "GL_ARB_clear_buffer_object": "Buffer clears",
    "GL_ARB_compute_shader": "Compute shaders", "GL_ARB_copy_image": "Image copies",
    "GL_ARB_ES3_compatibility": "OpenGL ES 3.0 compatibility",
    "GL_ARB_explicit_uniform_location": "Explicit uniform locations",
    "GL_ARB_fragment_layer_viewport": "Layer and viewport index in fragment shaders",
    "GL_ARB_framebuffer_no_attachments": "Framebuffers without attachments",
    "GL_ARB_internalformat_query2": "Internal format queries 2", "GL_ARB_invalidate_subdata": "Data invalidation",
    "GL_ARB_multi_draw_indirect": "Multi-draw indirect", "GL_ARB_program_interface_query": "Program interface queries",
    "GL_ARB_robust_buffer_access_behavior": "Robust buffer access behavior",
    "GL_ARB_shader_image_size": "Image size queries", "GL_ARB_shader_storage_buffer_object": "Shader storage buffers",
    "GL_ARB_stencil_texturing": "Stencil texturing", "GL_ARB_texture_buffer_range": "Texture buffer ranges",
    "GL_ARB_texture_query_levels": "Texture level queries",
    "GL_ARB_texture_storage_multisample": "Immutable multisample textures", "GL_ARB_texture_view": "Texture views",
    "GL_ARB_vertex_attrib_binding": "Vertex attribute bindings", "GL_KHR_debug": "Debug output",
    "GL_ARB_buffer_storage": "Buffer storage", "GL_ARB_clear_texture": "Texture clears",
    "GL_ARB_enhanced_layouts": "Enhanced layouts", "GL_ARB_multi_bind": "Multi-bind",
    "GL_ARB_query_buffer_object": "Query buffer objects",
    "GL_ARB_texture_mirror_clamp_to_edge": "Mirror clamp to edge", "GL_ARB_texture_stencil8": "Stencil-only textures",
    "GL_ARB_vertex_type_10f_11f_11f_rev": "Packed 10/11-bit float vertex attributes",
    "GL_ARB_clip_control": "Clip control", "GL_ARB_conditional_render_inverted": "Inverted conditional rendering",
    "GL_ARB_cull_distance": "Cull distances", "GL_ARB_derivative_control": "Coarse and fine derivatives",
    "GL_ARB_direct_state_access": "Direct state access", "GL_ARB_ES3_1_compatibility": "OpenGL ES 3.1 compatibility",
    "GL_ARB_get_texture_sub_image": "Texture sub-image reads",
    "GL_ARB_shader_texture_image_samples": "Texture and image sample counts in shaders",
    "GL_ARB_texture_barrier": "Texture barriers", "GL_KHR_context_flush_control": "Context flush control",
    "GL_KHR_robustness": "Robustness",
    "GL_ARB_gl_spirv": "SPIR-V shaders", "GL_ARB_indirect_parameters": "Indirect draw counts",
    "GL_ARB_pipeline_statistics_query": "Pipeline statistics queries",
    "GL_ARB_polygon_offset_clamp": "Polygon offset clamp", "GL_ARB_shader_atomic_counter_ops": "Atomic counter operations",
    "GL_ARB_shader_draw_parameters": "Draw parameters in shaders", "GL_ARB_shader_group_vote": "Shader group votes",
    "GL_ARB_spirv_extensions": "SPIR-V extensions", "GL_ARB_texture_filter_anisotropic": "Anisotropic filtering",
    "GL_ARB_transform_feedback_overflow_query": "Transform feedback overflow queries",
    "GL_KHR_no_error": "No-error contexts",
}
# OpenGL ES features by version: (feature, Yes / Partly / No, what is missing).
# Hand-maintained from the ES results in tests/cts/baseline and docs/unsupported.md#opengl-es.
ES_FEATURES = {
    "2.0": [
        ("GLSL ES 1.00 vertex and fragment shaders", "Yes", None),
        ("Framebuffer objects", "Yes", None),
        ("Vertex buffer objects", "Yes", None),
        ("Cube maps and non-power-of-two textures", "Yes", None),
        ("Depth, stencil and packed depth-stencil buffers", "Yes", None),
        ("Float, half-float and depth textures (extensions)", "Yes", None),
        ("3D textures, vertex array objects, instancing (extensions)", "Yes", None),
        ("Color attachments through `GL_NV_fbo_color_attachments`", "No",
         "Not advertised; `GL_EXT_draw_buffers` gives several color attachments"),
        ("Separate depth and stencil attachments", "No",
         "`FRAMEBUFFER_UNSUPPORTED`: Metal keeps depth and stencil in one texture (allowed by OpenGL ES)"),
        ("Lines wider than 1", "No", "Optional; Metal draws 1-pixel lines only"),
    ],
    "3.0": [
        ("GLSL ES 3.00", "Yes", None),
        ("Transform feedback", "Yes", None),
        ("Instanced draws", "Yes", None),
        ("Uniform buffers", "Yes", None),
        ("Integer textures and attributes", "Yes", None),
        ("sRGB textures and framebuffers", "Yes", None),
        ("Multisample renderbuffers (up to 4 samples)", "Yes", None),
        ("ETC2 and EAC compressed textures", "Yes", None),
        ("3D and 2D array textures", "Yes", None),
        ("Sampler objects", "Yes", None),
        ("Occlusion queries", "Yes", None),
        ("Sync objects", "Yes", None),
        ("Multiple render targets", "Yes", None),
        ("Primitive restart", "Yes", None),
        ("Program binaries", "Yes", None),
        ("Texture swizzles", "Yes", None),
        ("Immutable texture storage", "Yes", None),
        ("Pixel buffer objects", "Yes", None),
        ("More than 4 samples", "No", "Optional; Apple GPUs render with 1, 2 or 4 samples"),
        ("Multiview (`GL_OVR_multiview`)", "No", "Not advertised"),
        ("Multisampled render to texture (`GL_EXT_multisampled_render_to_texture`)", "No", "Not advertised"),
    ],
    "3.1": [
        ("GLSL ES 3.10", "Yes", None),
        ("Compute shaders", "Yes", None),
        ("Image load/store", "Yes", None),
        ("Shader storage buffers", "Yes", None),
        ("Atomic counters", "Partly", "Arrays of atomic counters passed to functions do not compile"),
        ("Indirect draws and dispatches", "Yes", None),
        ("Separate shader programs", "Yes", None),
        ("Multisample textures", "Yes", None),
        ("Vertex attribute bindings", "Yes", None),
        ("Program interface queries", "Yes", None),
        ("Arrays of matrices as stage inputs and outputs", "No",
         "Metal stage inputs and outputs cannot hold them (SPIRV-Cross); such programs do not link"),
        ("Texture gather", "Yes", None),
        ("Stencil texturing", "Yes", None),
        ("Framebuffers without attachments", "Yes", None),
    ],
    "3.2": [
        ("GLSL ES 3.20", "Yes", None),
        ("Geometry shaders", "Yes", None),
        ("Tessellation shaders", "Yes", None),
        ("Advanced blend equations", "Yes", None),
        ("ASTC LDR compressed textures", "Yes", None),
        ("ASTC HDR and 3D compressed textures", "No", "Not advertised; LDR ASTC only"),
        ("Texture buffers", "Yes", None),
        ("Cube map arrays", "Yes", None),
        ("Debug output", "Yes", None),
        ("Robustness", "Yes", None),
        ("Per-sample shading", "Yes", None),
        ("Multisample 2D array textures", "Yes", None),
        ("Primitive bounding box", "Yes", None),
        ("Per-buffer blending", "Yes", None),
        ("Image copies", "Yes", None),
        ("Border clamping", "Yes", None),
        ("Base vertex draws", "Yes", None),
    ],
}



def rel(*parts):
    return os.path.join(ROOT, *parts)


def load_baselines(suite):
    """{(config, runner): (header dict, results)} for one mustpass list's baselines."""
    out = {}
    for path in sorted(glob.glob(rel("tests", "cts", "baseline", "%s-cfg*-*.txt" % suite))):
        name = os.path.basename(path)[:-4]
        m = re.match(re.escape(suite) + r"-cfg(\d)-(.+)$", name)
        if not m or m.group(2) == "oldcore":
            continue
        header, results = cts.load(path)
        meta = {"file": os.path.relpath(path, ROOT), "config": int(m.group(1)), "runner": m.group(2)}
        if meta["config"] not in CONFIGS:
            raise SystemExit("%s: configuration %d is not run (%s)" % (meta["file"], meta["config"], CONFIGS_NOTE))
        if suite in SUITE_RUNS and (meta["config"], meta["runner"]) not in SUITE_RUNS[suite]:
            raise SystemExit("%s: %s is not run in configuration %d %s" % (meta["file"], suite, meta["config"],
                                                                          meta["runner"]))
        for line in header:
            if line.startswith("# mustpass"):
                meta["command_line"] = line.split(":", 1)[1].strip()
            elif line.startswith("# commit"):
                meta["commit"] = line.split(",")[0].replace("# commit", "").strip()
                seconds = re.search(r"(\d+) s$", line.strip())
                if seconds:
                    meta["seconds"] = int(seconds.group(1))
            elif line.startswith("# cts"):
                meta["cts"] = line.replace("# cts", "").strip()
        # A recheck (same list, configuration and runner) reruns the cases that were not Pass at a
        # later commit; its results replace the baseline's for those cases.
        recheck = rel("tests", "cts", "recheck", os.path.basename(path))
        if os.path.exists(recheck):
            recheck_header, rechecked = cts.load(recheck)
            unknown = [c for c in rechecked if c not in results]
            if unknown:
                raise SystemExit("%s: cases not in %s:\n  %s" % (os.path.relpath(recheck, ROOT), meta["file"],
                                                                  "\n  ".join(unknown[:10])))
            not_rechecked = [c for c, status in results.items() if status != "Pass" and c not in rechecked]
            if not_rechecked:
                raise SystemExit("%s: non-Pass cases of %s not rechecked:\n  %s" % (
                    os.path.relpath(recheck, ROOT), meta["file"], "\n  ".join(not_rechecked[:10])))
            results.update(rechecked)
            meta["recheck"] = {"file": os.path.relpath(recheck, ROOT), "cases": len(rechecked)}
            for line in recheck_header:
                if line.startswith("# commit"):
                    meta["recheck"]["commit"] = line.split(",")[0].replace("# commit", "").strip()
        out[(meta["config"], meta["runner"])] = (meta, results)
    return out


def totals(results):
    counts = {s: 0 for s in STATUSES}
    for status in results.values():
        key = "Pass" if status in cts.PASSING else status
        counts[key] = counts.get(key, 0) + 1
    return counts


def applicable(results):
    """(passing, applicable) for one run: applicable cases are those not NotSupported."""
    t = totals(results)
    return t["Pass"], len(results) - t["NotSupported"]


def percent(passed, total):
    """Rounded down, so a run with one failure never shows as 100%."""
    if passed == total:
        return "100%"
    return "%.2f%%" % (math.floor(10000.0 * passed / total) / 100)


def share(part, whole):
    """Rounded up, so a small share never shows as 0%."""
    if part == 0:
        return "0%"
    if part == whole:
        return "100%"
    return "%.2f%%" % (math.ceil(10000.0 * part / whole) / 100)


def lacking(results, kind_of):
    """(cases NotSupported because the core does not offer what they test, cases any
    implementation can run): test-design NotSupported cases are skipped everywhere."""
    missing = design = 0
    for case, status in results.items():
        if status != "NotSupported":
            continue
        if kind_of(case) == "test-design":
            design += 1
        else:
            missing += 1
    return missing, len(results) - design


def duration(seconds):
    return "%d s" % seconds if seconds < 120 else "%.1f min" % (seconds / 60.0)


def classify_not_supported(suites):
    audit = json.load(open(rel("tests", "cts", "notsupported.json")))
    entries = audit["entries"]
    per_entry = {e["pattern"]: set() for e in entries}
    unexplained = []
    for suite, baselines in suites.items():
        for (config, runner), (_, results) in baselines.items():
            for case, status in results.items():
                if status != "NotSupported":
                    continue
                match = next((e for e in entries if fnmatch.fnmatchcase(case, e["pattern"])), None)
                if match:
                    per_entry[match["pattern"]].add(case)
                else:
                    unexplained.append("%s (%s config %d, %s)" % (case, suite, config, runner))
    return entries, per_entry, unexplained


def classify_failures(suites):
    """{suite: {case: {"runs": [(config, runner, status)], "reason": str or None}}} for every
    result that neither passes nor is NotSupported, with its tests/cts/known-failures.json reason."""
    known = json.load(open(rel("tests", "cts", "known-failures.json")))["entries"]
    used = set()
    out = {}
    for suite, baselines in suites.items():
        for (config, runner), (_, results) in sorted(baselines.items()):
            for case, status in sorted(results.items()):
                if status in cts.PASSING or status == "NotSupported":
                    continue
                entry = out.setdefault(suite, {}).setdefault(case, {"runs": [], "reason": None})
                entry["runs"].append((config, runner, status))
                match = next((e for e in known if fnmatch.fnmatchcase(case, e["pattern"])), None)
                if match:
                    entry["reason"] = match["reason"]
                    used.add(match["pattern"])
    stale = [e["pattern"] for e in known if e["pattern"] not in used]
    if stale:
        raise SystemExit("tests/cts/known-failures.json entries that match no failing result:\n  " + "\n  ".join(stale))
    return out


def describe_runs(runs):
    """'configurations 3, 4 under Wine and natively' style summary of failing runs."""
    by_runner = {}
    for config, runner, status in runs:
        by_runner.setdefault(runner, []).append((config, status))
    parts = []
    for runner in sorted(by_runner):
        items = by_runner[runner]
        statuses = sorted({s for _, s in items})
        parts.append("%s %s in configuration%s %s" % (
            "/".join(statuses), "under Wine" if runner == "wine" else "natively", "s" if len(items) > 1 else "",
            ", ".join(str(c) for c, _ in items)))
    return "; ".join(parts)


def core_commands():
    """Core profile commands up to MAX_VERSION from the Khronos registry."""
    tree = ET.parse(rel("third_party", "khronos", "gl.xml"))
    commands, removed = set(), set()
    for feature in tree.getroot().iter("feature"):
        if feature.get("api") != "gl" or float(feature.get("number")) > float(MAX_VERSION):
            continue
        for require in feature.iter("require"):
            if require.get("profile") in (None, "core"):
                commands.update(c.get("name") for c in require.iter("command"))
        for remove in feature.iter("remove"):
            if remove.get("profile") == "core":
                removed.update(c.get("name") for c in remove.iter("command"))
    return sorted(commands - removed)


def implemented_commands():
    """Commands with an implementation in src/core (the generated weak stubs aside)."""
    names = set()
    pattern = re.compile(r"^[A-Za-z_][\w:<>\*\s&]*?\b([A-Z][A-Za-z0-9_]*)\(Context& ctx", re.M)
    # Families defined through macros in api/program.mm.
    uniform_v = re.compile(r"^WMGL_UNIFORM_V\((\w+),", re.M)
    uniform_matrix = re.compile(r"^WMGL_UNIFORM_MATRIX\((\w+),", re.M)
    double_v = re.compile(r"^WMGL_UNIFORM_DOUBLE_V\((\w+)\)", re.M)
    double_matrix = re.compile(r"^WMGL_UNIFORM_DOUBLE_MATRIX\((\w+)\)", re.M)
    for path in glob.glob(rel("src", "core", "**", "*.mm"), recursive=True) + glob.glob(
            rel("src", "core", "**", "*.cpp"), recursive=True):
        text = open(path).read()
        names.update("gl" + m.group(1) for m in pattern.finditer(text))
        names.update("glProgramUniform" + m.group(1) for m in double_v.finditer(text))
        names.update("glProgramUniformMatrix" + m.group(1) + "dv" for m in double_matrix.finditer(text))
        for prefix in ("glUniform", "glProgramUniform"):
            names.update(prefix + m.group(1) for m in uniform_v.finditer(text))
            for suffix in ("fv", "dv"):
                names.update(prefix + "Matrix" + m.group(1) + suffix for m in uniform_matrix.finditer(text))
    return names


def extension_commands():
    """{extension: [commands]} and {command: alias target} from the Khronos registry."""
    root = ET.parse(rel("third_party", "khronos", "gl.xml")).getroot()
    aliases = {}
    for command in root.iter("command"):
        proto = command.find("proto")
        alias = command.find("alias")
        if proto is not None and alias is not None:
            aliases[proto.findtext("name")] = alias.get("name")
    extensions = {}
    for ext in root.iter("extension"):
        if "gl" not in (ext.get("supported") or "").split("|") and "glcore" not in (ext.get("supported") or "").split("|"):
            continue
        commands = []
        for require in ext.iter("require"):
            if require.get("api") not in (None, "gl"):
                continue
            # Blocks marked "Supported only if GL_X is supported" count only when X is advertised.
            condition = re.search(r"only if (GL_\w+)", require.get("comment") or "")
            commands += [(c.get("name"), condition.group(1) if condition else None) for c in require.iter("command")]
        extensions[ext.get("name")] = commands
    return extensions, aliases


def advertised_extensions():
    text = open(rel("src", "core", "context.mm")).read()
    start = text.index("void Context::initStrings()")
    body = text[start:text.index("\n}\n", start)]
    body = "\n".join(line.split("//")[0] for line in body.splitlines())  # ignore comments
    return re.findall(r'"((?:GL|WGL)_\w+)"', body)


def games():
    manifest = json.load(open(rel("tests", "games", "corpus.json")))
    path = rel("tests", "games", "results.json")
    record = json.load(open(path)) if os.path.exists(path) else {"entries": []}
    by_name = {e["name"]: e for e in record["entries"]}
    return manifest["entries"], by_name, record


def es_runner_summary():
    """The last cts-runner run over every OpenGL ES configuration (ES_RUNNER_SUMMARY): its header
    dict and one (log file, {status: count}) per run, or None without one."""
    path = rel(ES_RUNNER_SUMMARY)
    if not os.path.exists(path):
        return None
    meta, runs = {}, []
    for line in open(path):
        line = line.rstrip("\n")
        if line.startswith("# "):
            key, _, value = line[2:].partition(" ")
            meta[key] = value
        elif line.strip():
            name, *counts = line.split()
            runs.append((name, {k: int(v) for k, v in (c.split("=") for c in counts)}))
    return meta, runs


def render(suites, es_suites):
    every = dict(suites, **es_suites)
    entries, per_entry, unexplained = classify_not_supported(every)
    if unexplained:
        raise SystemExit("NotSupported cases without a reason in tests/cts/notsupported.json:\n  " +
                         "\n  ".join(sorted(unexplained)[:50]))
    failures = classify_failures(every)
    old = cts.load(rel("tests", "cts", "baseline", "gl33-cfg1-oldcore.txt"))[1]
    core = core_commands()
    implemented = implemented_commands()
    missing = [c for c in core if c not in implemented]
    extensions = advertised_extensions()
    ext_commands, aliases = extension_commands()
    incomplete = {}
    for ext in extensions:
        if ext.startswith("WGL_"):
            continue
        absent = [c for c, condition in ext_commands.get(ext, [])
                  if (condition is None or condition in extensions) and c not in implemented and aliases.get(c) not in implemented]
        if ext not in ext_commands:
            absent = ["(not in gl.xml)"]
        if absent:
            incomplete[ext] = absent
    if incomplete:
        raise SystemExit("advertised extensions with unimplemented commands:\n  " +
                         "\n  ".join("%s: %s" % (k, ", ".join(v)) for k, v in sorted(incomplete.items())))
    game_entries, game_results, game_record = games()

    # Kinds of NotSupported entries, for the unsupported share.
    def kind_of(case):
        return next(e["kind"] for e in entries if fnmatch.fnmatchcase(case, e["pattern"]))

    # ---- Conformance totals: (version, conformance %, features, unsupported %) per OpenGL and OpenGL ES version.
    gl_rows = []
    commits = sorted({meta.get("commit", "?") for b in suites.values() for meta, _ in b.values()})
    rechecks = sorted({(suite, meta["recheck"].get("commit", "?")) for suite, b in suites.items()
                       for meta, _ in b.values() if "recheck" in meta})
    for version, adds, suite in VERSIONS:
        if version < "3.0":
            continue
        baselines = suites.get(suite) if suite and suite != "covered" else None
        if suite == "covered":
            conformance_cell, unsupported_cell = "covered by %s" % dict(SUITES)[COVERED_BY], "-"
        elif baselines:
            runs = sorted(baselines)
            rates = {k: applicable(baselines[k][1]) for k in runs}
            worst = min(runs, key=lambda k: (rates[k][0] / max(rates[k][1], 1), k))
            shares = [lacking(baselines[k][1], kind_of) for k in runs]
            most = max(shares, key=lambda x: x[0] / max(x[1], 1))
            conformance_cell, unsupported_cell = percent(*rates[worst]), share(*most)
        elif version > MAX_VERSION:
            conformance_cell, unsupported_cell = "-", "100%"
        else:
            conformance_cell, unsupported_cell = "not measured", "not measured"
        gl_rows.append((version, conformance_cell, adds, unsupported_cell))
    es_rows, es_commits = [], set()
    for version, adds in ES_VERSIONS:
        # The KHR-Single lists test only GL_KHR_shader_subgroup; like the desktop
        # totals, the totals leave them out (the run table lists them).
        counted = [suite for suite, _, v in ES_SUITES if v == version and not suite.endswith("-khr-single")]
        runs = [results for suite in counted for _, results in es_suites[suite].values()]
        es_commits.update(meta.get("commit", "?") for suite in counted for meta, _ in es_suites[suite].values())
        if not runs:
            es_rows.append((version, "not measured", adds, "not measured"))
            continue
        # The version's lists together (different lists, not configurations of one).
        passed = sum(applicable(r)[0] for r in runs)
        total = sum(applicable(r)[1] for r in runs)
        offered_not = sum(lacking(r, kind_of)[0] for r in runs)
        runnable = sum(lacking(r, kind_of)[1] for r in runs)
        es_rows.append((version, percent(passed, total), adds, share(offered_not, runnable)))
    es_summary = es_runner_summary()
    gl_measure = (
        "**Conformance %%**: passing cases over applicable cases (cases that are not NotSupported) of the "
        "version's Khronos CTS mustpass list (`KHR-GL30` to `KHR-GL43`, `KHR-GL46`), in the worst of its runs: "
        "%d for `KHR-GL30` to `KHR-GL43` (configurations %s of `mustpass.xml`, each under Wine and natively), "
        "3 for `KHR-GL46` (configuration 1 natively, configurations 1 and 3 under Wine), at commit %s. "
        "OpenGL 4.4 and 4.5 have no list of their own: `KHR-GL46` runs their tests." % (
            2 * len(CONFIGS), "%d to %d" % (CONFIGS[0], CONFIGS[-1]), ", ".join(commits)))
    gl_recheck = None
    if rechecks:
        packages = [package for suite, package in SUITES if any(s == suite for s, _ in rechecks)]
        gl_recheck = ("The cases that were not Pass in %s were run again at commit %s, in the same configurations "
                      "under Wine and natively (`tests/cts/recheck/`); those results replace the baselines' for "
                      "those cases." % (", ".join("`%s`" % p for p in packages),
                                        ", ".join(sorted({c for _, c in rechecks}))))
    es_measure = (
        "**GLES**: passing cases over applicable cases of all of the version's OpenGL ES mustpass lists "
        "together, Khronos's (`KHR-GLES2` to `KHR-GLES32`, `KHR-GLESEXT`, `KHR-NoContext`; not `KHR-Single`, which "
        "tests only subgroup operations) and "
        "dEQP's (`dEQP-GLES2` to `dEQP-GLES31`, with their rotation, multisample and RGB565 lists), in "
        "configuration 1 under Wine at commit %s; Unsupported %% likewise." % (
            ", ".join(sorted(es_commits)) or "-") +
        (" `cts-runner --type=es3` runs the OpenGL ES 2.0 and 3.0 lists in every configuration it "
         "selects ([OpenGL ES](#opengl-es))." if es_summary else ""))
    unsupported_measure = (
        "**Unsupported %**: cases the CTS reports NotSupported because WineMetalGL does not offer what they test "
        "(an extension it does not advertise, or a limit above the minimum), over the cases any implementation "
        "can run: [Unsupported](unsupported.md#conformance-notes).")

    # ---- conformance.md
    md = ["# WineMetalGL conformance", "",
          "Khronos CTS mustpass lists (`glNN-main.txt`) in %s. " % CONFIGS_NOTE +
          
          "`native` runs the core directly through its driver ABI (`tests/cts/native`); `wine` runs "
          "`glcts.exe` in a WoW64 prefix: `KHR-GL46` and the rechecks on stock Wine 11.0 with the Wine module, "
          "`KHR-GL30` to `KHR-GL43` on MetalSharp's Wine 11.17 with the 2.x patches. `KHR-GL46` runs "
          "configuration 1 natively and configurations 1 and 3 under Wine. Applicable cases are those that are not "
          "NotSupported.", ""]
    md += ["## Conformance totals", "", "| OpenGL | Conformance % | Features | Unsupported % |", "|---|---:|---|---:|"]
    md += ["| %s | %s | %s | %s |" % row for row in gl_rows]
    md += ["", "| GLES | Conformance % | Features | Unsupported % |", "|---|---:|---|---:|"]
    md += ["| %s | %s | %s | %s |" % row for row in es_rows]
    md += ["", gl_measure, ""] + ([gl_recheck, ""] if gl_recheck else []) + [es_measure, "", unsupported_measure, ""]
    md += ["## Features", "",
           "OpenGL 3.x features are core in every context. The OpenGL 4.x features are the extensions each version "
           "promoted to core, supported when the context advertises them. The rows marked No are optional: the "
           "CTS cases that test them are NotSupported, which is what Unsupported % counts.", "",
           "| OpenGL | Feature | Supported | Unsupported |", "|---|---|---|---|"]
    for version, _, _ in VERSIONS:
        for name in CORE_FEATURES.get(version, []):
            md.append("| %s | %s | Yes | - |" % (version, name))
        for ext in PROMOTED.get(version, []):
            advertised = ext in extensions
            md.append("| %s | %s (`%s`) | %s | %s |" % (version, FEATURE_NAMES[ext], ext,
                                                         "Yes" if advertised else "No",
                                                         "-" if advertised else "Not advertised"))
        for _, name, reason in (row for row in GL_NOT_OFFERED if row[0] == version):
            md.append("| %s | %s | No | %s |" % (version, name, reason))
    md += ["", "| GLES | Feature | Supported | Unsupported |", "|---|---|---|---|"]
    for version, _ in ES_VERSIONS:
        for name, status, gap in ES_FEATURES[version]:
            md.append("| %s | %s | %s | %s |" % (version, name, status, gap or "-"))
    md += ["", "What OpenGL ES contexts leave out, and why: [Unsupported](unsupported.md#opengl-es).", ""]
    for suite, package in SUITES:
        baselines = suites[suite]
        if not baselines:
            continue
        md += ["## %s" % package, "",
               "| Config | Runner | Pass | Fail | NotSupported | InternalError | Crash | Timeout | Missing | Other | Cases | Applicable passing | Time | Commit | File |",
               "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---|"]
        for (config, runner) in sorted(baselines):
            meta, results = baselines[(config, runner)]
            t = totals(results)
            other = sum(n for status, n in t.items() if status not in STATUSES)
            md.append("| %d | %s | %d | %d | %d | %d | %d | %d | %d | %d | %d | %s | %s | %s | `%s` |" % (
                config, runner, t["Pass"], t["Fail"], t["NotSupported"], t["InternalError"], t["Crash"], t["Timeout"],
                t["Missing"], other, len(results), percent(*applicable(results)),
                duration(meta["seconds"]) if "seconds" in meta else "?", meta.get("commit", "?"), meta["file"]))
        revisions = sorted({meta.get("cts", "not recorded") for meta, _ in baselines.values()})
        md += ["", "Command lines: `scripts/cts/run.py --mustpass %s-main.txt --config N` reads them from "
               "`mustpass.xml`. CTS build: %s." % (suite, ", ".join(revisions))]
        failing = failures.get(suite, {})
        if failing:
            md += ["", "Cases that neither pass nor are NotSupported:", "", "| Case | Runs | Reason |", "|---|---|---|"]
            for case, f in sorted(failing.items()):
                md.append("| `%s` | %s | %s |" % (case, describe_runs(f["runs"]),
                                                  f["reason"] or "Not yet explained."))
        if suite == "gl33" and (1, "native") in baselines:
            new = baselines[(1, "native")][1]
            gained = sum(1 for c, s in new.items() if s in cts.PASSING and old.get(c) not in cts.PASSING)
            lost = sum(1 for c, s in old.items() if s in cts.PASSING and new.get(c) not in cts.PASSING)
            md += ["", "Against the legacy implementation: `tests/cts/baseline/gl33-cfg1-oldcore.txt` is the last "
                   "result of the legacy (removed) implementation, configuration 1: %d Pass. The core passes %d "
                   "cases it failed and fails %d cases it passed." % (totals(old)["Pass"], gained, lost)]
        md.append("")
    if any(es_suites.values()):
        md += ["## OpenGL ES", "",
               "OpenGL ES contexts (`WGL_EXT_create_context_es2_profile`; OpenGL ES 3.2) run the OpenGL ES mustpass "
               "lists, Khronos's (`gles*-khr-*`) and dEQP's (`aosp_mustpass`), each in configuration 1 of its "
               "`mustpass.xml` under Wine: `scripts/cts/run.py --mustpass LIST.txt --config 1`.", ""]
        summary = es_runner_summary()
        if summary:
            meta, runs = summary
            # cts-runner's Failed counts every result that is not Pass, NotSupported or a warning.
            failed = [name for name, counts in runs if counts.get("Failed")]
            cases = sum(counts.get("Executed", 0) for _, counts in runs)
            md += ["`cts-runner --type=es3`, which runs every OpenGL ES 2.0 and 3.0 list in each configuration it "
                   "selects (%s), at commit %s: %d runs, %s cases, %s. The runs are listed in `%s`." % (
                       meta.get("configs", "?"), meta.get("commit", "?"), len(runs), "{:,}".format(cases),
                       "no failing case (Fail, Crash, Timeout, InternalError) in any run" if not failed else
                       "failing cases in %s" % ", ".join("`%s`" % f for f in failed), ES_RUNNER_SUMMARY), ""]
        md += ["| List | Runs | OpenGL ES | Pass | Fail | NotSupported | InternalError | Crash | Timeout | Missing | Other | Cases | Applicable passing | Time | Commit | File |",
               "|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---|"]
        for suite, what, version in ES_SUITES:
            for (config, runner), (meta, results) in sorted(es_suites[suite].items()):
                t = totals(results)
                other = sum(n for status, n in t.items() if status not in STATUSES)
                md.append("| `%s` | %s | %s | %d | %d | %d | %d | %d | %d | %d | %d | %d | %s | %s | %s | `%s` |" % (
                    suite, what, version, t["Pass"], t["Fail"], t["NotSupported"], t["InternalError"], t["Crash"],
                    t["Timeout"], t["Missing"], other, len(results), percent(*applicable(results)),
                    duration(meta["seconds"]) if "seconds" in meta else "?", meta.get("commit", "?"), meta["file"]))
        revisions = sorted({meta.get("cts", "not recorded") for b in es_suites.values() for meta, _ in b.values()})
        md += ["", "CTS build: %s." % ", ".join(revisions)]
        for suite, what, version in ES_SUITES:
            failing = failures.get(suite, {})
            if not failing:
                continue
            md += ["", "### `%s`: cases that neither pass nor are NotSupported" % suite, ""]
            if len(failing) <= 40:
                md += ["| Case | Result | Reason |", "|---|---|---|"]
                for case, f in sorted(failing.items()):
                    md.append("| `%s` | %s | %s |" % (case, "/".join(sorted({st for _, _, st in f["runs"]})),
                                                      f["reason"] or "Not yet explained."))
            else:
                # Long lists by test group: the case path without its last two components.
                groups = {}
                for case, f in sorted(failing.items()):
                    group = ".".join(case.split(".")[:-2]) if case.count(".") > 2 else case
                    g = groups.setdefault(group, {"cases": 0, "reasons": set()})
                    g["cases"] += 1
                    g["reasons"].add(f["reason"] or "Not yet explained.")
                md += ["%d cases, by test group:" % len(failing), "", "| Group | Cases | Reason |", "|---|---:|---|"]
                for group, g in sorted(groups.items()):
                    md.append("| `%s` | %d | %s |" % (group, g["cases"], " ".join(sorted(g["reasons"]))))
        md.append("")
    md += ["NotSupported cases, and the reason for each: [Unsupported](unsupported.md#cts-cases-reported-notsupported).",
           "", "## API", "",
           "GL %s core profile commands in the Khronos registry: %d. Implemented: %d. Missing: %s." % (
               MAX_VERSION, len(core), len(core) - len(missing), ", ".join("`%s`" % m for m in missing) if missing else "none"),
           "", "Advertised extensions: %s." % ", ".join("`%s`" % e for e in extensions), "",
           "The machine-readable form is `docs/api-coverage.json`.", ""]
    conformance = "\n".join(md)

    coverage = json.dumps({
        "project": "WineMetalGL",
        "generated_by": "scripts/docs/generate.py",
        "context": {"versions": "up to %s" % MAX_VERSION, "profiles": ["core", "compatibility"]},
        "host": {"os": "macOS", "minimum": "15.0", "architecture": "x86_64"},
        "core_commands_version": MAX_VERSION,
        "core_commands": len(core),
        "implemented": len(core) - len(missing),
        "missing": missing,
        "extensions": extensions,
    }, indent=1) + "\n"

    # ---- games.md
    g = ["# Game corpus", "",
         "Generated by `scripts/docs/generate.py` from `tests/games/corpus.json` and the last run recorded in "
         "`tests/games/results.json` (commit %s, %s); do not edit." % (
             game_record.get("commit", "?"), game_record.get("date", "?")), "",
         "Each entry renders frames that are compared with reference frames from Apple's OpenGL "
         "(`scripts/traces/replay.py`). See `tests/games/README.md` for the tiers, and "
         "[Performance](performance.md) for their frame times.", "",
         "| Entry | Tier | Kind | Result | Differing pixels | Min PSNR | Notes |", "|---|---|---|---|---:|---:|---|"]
    for e in game_entries:
        r = game_results.get(e["name"])
        g.append("| %s | %s | %s | %s | %s | %s | %s |" % (
            e["name"], e.get("tier", ""), e["kind"], r["status"] if r else "not run",
            "%.4f%%" % (r["worst_fraction"] * 100) if r else "", "%.1f dB" % r["min_psnr_db"] if r else "",
            e.get("notes", "").replace("|", "/")))
    games_md = "\n".join(g) + "\n"

    # ---- performance.md (W24), from scripts/traces/benchmark.py.
    perf_path = rel("tests", "games", "perf.json")
    perf = json.load(open(perf_path)) if os.path.exists(perf_path) else {"entries": []}
    budget = perf.get("budget", {})
    entries_perf = perf.get("entries", [])
    compared = [p for p in entries_perf if p.get("apple") and (p.get("new") or {}).get("avg_ms")]
    pm = ["# Performance", "",
          "Generated by `scripts/docs/generate.py` from `tests/games/perf.json` (commit %s, %s); do not edit." % (
              perf.get("commit", "?"), perf.get("date", "?")), "",
          "Frame times of the [game corpus](games.md) entries, recorded by `scripts/traces/benchmark.py`: the median "
          "of %s runs of %s frames each, vsync off, after a warm-up. The entries are small demos and one trace, so "
          "the times measure the per-frame cost of the OpenGL implementation and Wine around it more than GPU "
          "throughput. Apple's OpenGL is the same program with `WINEMETALGL=0`." % (
              perf.get("runs", "?"), perf.get("frames", "?")), ""]
    if compared:
        speedups = sorted(p["apple"]["avg_ms"] / p["new"]["avg_ms"] for p in compared)
        pm += ["Where both run an entry, WineMetalGL's average frame time is %.1f to %.1f times shorter than "
               "Apple's OpenGL's (%d entries)." % (speedups[0], speedups[-1], len(compared)), ""]
    pm += ["| Entry | Tier | WineMetalGL avg / p95 / max (ms) | Apple's OpenGL avg / p95 / max (ms) | Faster by | Budget |",
           "|---|---|---|---|---:|---|"]

    def triple(t):
        return " / ".join("%.3f" % t[k] if t and t.get(k) is not None else "-" for k in ("avg_ms", "p95_ms", "max_ms"))

    for p in entries_perf:
        new, apple = p.get("new") or {}, p.get("apple") or {}
        faster = "%.2fx" % (apple["avg_ms"] / new["avg_ms"]) if apple.get("avg_ms") and new.get("avg_ms") else "-"
        pm.append("| %s | %s | %s | %s | %s | %s |" % (
            p["name"], p["tier"], triple(new), triple(apple) if apple else "-", faster,
            p["status"] + (" (" + "; ".join(p["problems"]) + ")" if p.get("problems") else "")))
    untimed = [p for p in entries_perf if p.get("apple_note")]
    if untimed:
        pm += ["", "Without an Apple's OpenGL time: %s." % "; ".join(
            "`%s` (%s)" % (p["name"], p["apple_note"]) for p in untimed)]
    pm += ["", "Budgets: at most %.1fx Apple's OpenGL's average where it runs the entry, at least 60 fps for Tier B, "
           "no frame over %.0f ms, and no more than %.0f%% slower than the previous baseline." % (
               budget.get("ratio", 1.2), budget.get("hitch_ms", 50), 100 * (budget.get("regression", 1.1) - 1)), ""]
    performance = "\n".join(pm)

    blocks = {}

    # ---- docs/unsupported.md: OpenGL 4.x and the NotSupported audit.
    u = ["| Version | Features | Promoted extensions advertised | Not advertised |", "|---|---|---|---|"]
    for version, adds, _ in VERSIONS:
        if version <= "3.3":
            continue
        available = [e for e in PROMOTED[version] if e in extensions]
        absent = [e for e in PROMOTED[version] if e not in extensions]
        u.append("| %s | %s | %s | %s |" % (
            version, adds, "%s (%d of %d)" % (", ".join("`%s`" % e for e in available), len(available),
                                              len(PROMOTED[version])) if available else "none (0 of %d)" % len(PROMOTED[version]),
            ", ".join("`%s`" % e for e in absent) or "none"))
    blocks[("docs/unsupported.md", "gl4")] = "\n".join(u)
    n = ["Every NotSupported case in a committed result matches one entry of `tests/cts/notsupported.json`. "
         "`extension` and `version` cases test something WineMetalGL does not offer; `test-design` cases are "
         "skipped by the test itself on every implementation.", "",
         "| Pattern | Kind | %s | Reason |" % " | ".join(package for _, package in SUITES),
         "|---|---|%s|---|" % "|".join("---:" for _ in SUITES)]
    for e in entries:
        counts = []
        for suite, _ in SUITES:
            prefix = dict(SUITES)[suite] + "."
            counts.append(str(sum(1 for c in per_entry[e["pattern"]] if c.startswith(prefix))))
        # Entries only OpenGL ES cases match are in the table below.
        if any(c != "0" for c in counts) or not per_entry[e["pattern"]]:
            n.append("| `%s` | %s | %s | %s |" % (e["pattern"], e["kind"], " | ".join(counts), e["reason"]))
    if any(es_suites.values()):
        # OpenGL ES: cases NotSupported in any list of a version, by version.
        es_cases = {version: set() for version, _ in ES_VERSIONS}
        for suite, _, version in ES_SUITES:
            for _, results in es_suites[suite].values():
                es_cases[version].update(c for c, st in results.items() if st == "NotSupported")
        n += ["", "OpenGL ES lists (cases NotSupported in any list of the version):", "",
              "| Pattern | Kind | %s | Reason |" % " | ".join("ES %s" % v for v, _ in ES_VERSIONS),
              "|---|---|%s|---|" % "|".join("---:" for _ in ES_VERSIONS)]
        for e in entries:
            counts = [sum(1 for c in es_cases[v] if fnmatch.fnmatchcase(c, e["pattern"])) for v, _ in ES_VERSIONS]
            if any(counts):
                n.append("| `%s` | %s | %s | %s |" % (e["pattern"], e["kind"], " | ".join(str(c) for c in counts),
                                                       e["reason"]))
    blocks[("docs/unsupported.md", "notsupported")] = "\n".join(n)

    # ---- docs/testing.md: CTS run times from the result headers.
    t = ["| Mustpass list | Cases | Under Wine, per run | Natively, per run | All runs |",
         "|---|---:|---:|---:|---:|"]
    for suite, package in SUITES:
        baselines = suites[suite]
        times = {runner: [meta["seconds"] for (c, r), (meta, _) in baselines.items() if r == runner and "seconds" in meta]
                 for runner in ("wine", "native")}
        if not baselines:
            continue
        cases = max(len(results) for _, results in baselines.values())

        def span(values):
            if not values:
                return "-"
            lo, hi = min(values), max(values)
            return duration(lo) if lo == hi or duration(lo) == duration(hi) else "%s to %s" % (duration(lo), duration(hi))

        total = sum(times["wine"]) + sum(times["native"])
        t.append("| `%s` | %s | %s | %s | %s |" % (package, "{:,}".format(cases), span(times["wine"]),
                                                  span(times["native"]), duration(total) if total else "-"))
    for suite, _, _ in ES_SUITES:
        for (config, runner), (meta, results) in sorted(es_suites[suite].items()):
            seconds = meta.get("seconds")
            t.append("| `%s` | %s | %s | - | %s |" % (suite, "{:,}".format(len(results)),
                                                    duration(seconds) if seconds else "?",
                                                    duration(seconds) if seconds else "?"))
    blocks[("docs/testing.md", "cts-times")] = "\n".join(t)

    # ---- README.md: conformance totals (the full tables are in docs/conformance.md).
    lines = ["## Conformance totals", "", "| OpenGL | Conformance % | Unsupported % |", "|---|---:|---:|"]
    lines += ["| %s | %s | %s |" % (version, conformance_cell, unsupported_cell)
              for version, conformance_cell, _, unsupported_cell in gl_rows]
    lines += ["", "| GLES | Conformance % | Unsupported % |", "|---|---:|---:|"]
    lines += ["| %s | %s | %s |" % (version, conformance_cell, unsupported_cell)
              for version, conformance_cell, _, unsupported_cell in es_rows]
    lines += ["",
              "**Conformance %**: passing cases over applicable cases (cases that are not NotSupported) of the "
              "version's Khronos CTS mustpass lists (OpenGL: the worst of its runs; GLES: its lists together). "
              "**Unsupported %**: cases the CTS "
              "reports NotSupported because WineMetalGL does not offer what they test. Each version's features, "
              "what is supported and every run: [Conformance results](docs/conformance.md#conformance-totals).",
              "",
              "OpenGL 1.0 to 2.1 are implemented by WineMetalGL itself, as its compatibility profile: the "
              "fixed-function pipeline, immediate mode, display lists and legacy GLSL, alongside every 3.x feature. "
              "No public Khronos suite measures them; the Wine fixtures, GPU tests and game corpus exercise them, and "
              "[Unsupported](docs/unsupported.md#compatibility-profile-opengl-10-to-21) lists what is not implemented.",
              ""]
    blocks[("README.md", "totals")] = "\n".join(lines)

    # ---- docs/unsupported.md: what the README totals' Unsupported % counts, and the failing cases.
    notes = ["**Unsupported %** ([conformance totals](conformance.md#conformance-totals)): cases the CTS reports NotSupported because "
             "WineMetalGL does not offer what they test (an extension it does not advertise, or a limit above the "
             "GL minimum), over the cases any implementation can run. Contexts are OpenGL " + MAX_VERSION +
             ", the newest version; the unsupported cases are optional extensions ([OpenGL 4.x](#opengl-4x))."]
    failing = [(suite, case, f) for suite, _ in SUITES for case, f in sorted(failures.get(suite, {}).items())]
    if failing:
        explained = all(f["reason"] for _, case, f in failing)
        notes += ["", "Failing cases: %s. %s" % (
            ", ".join("`%s`" % case for _, case, _ in failing),
            "[Conformance results](conformance.md) gives the reason for each." if explained else
            "[Conformance results](conformance.md) lists them; some are not yet explained.")]
    blocks[("docs/unsupported.md", "totals-notes")] = "\n".join(notes)
    return {"docs/conformance.md": conformance, "docs/api-coverage.json": coverage, "docs/games.md": games_md,
            "docs/performance.md": performance}, blocks


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--check-provenance", action="store_true",
                        help="fail if any evidence file was recorded from a tree with uncommitted changes")
    args = parser.parse_args()
    suites = {suite: load_baselines(suite) for suite, _ in SUITES}
    es_suites = {suite: load_baselines(suite) for suite, _, _ in ES_SUITES}
    if args.check_provenance:
        # Evidence recorded from a tree with uncommitted changes does not describe a commit.
        every = list(suites.values()) + list(es_suites.values())
        recorded = [(meta["file"], meta.get("commit", "")) for b in every for meta, _ in b.values()]
        recorded += [(meta["recheck"]["file"], meta["recheck"].get("commit", "")) for b in every
                     for meta, _ in b.values() if "recheck" in meta]
        summary = es_runner_summary()
        if summary:
            recorded.append((ES_RUNNER_SUMMARY, summary[0].get("commit", "")))
        for name in ("results.json", "perf.json"):
            path = rel("tests", "games", name)
            if os.path.exists(path):
                record = json.load(open(path))
                recorded.append((os.path.relpath(path, ROOT), record.get("commit", "")))
                recorded += [("%s: %s" % (os.path.relpath(path, ROOT), e["name"]), e["commit"])
                             for e in record.get("entries", []) if "commit" in e]
        dirty = ["%s (%s)" % (f, c) for f, c in recorded if c.endswith("-dirty") or "uncommitted" in c]
        if dirty:
            raise SystemExit("evidence recorded from uncommitted trees:\n  " + "\n  ".join(dirty))
    outputs, blocks = render(suites, es_suites)
    for (path, name), content in sorted(blocks.items()):
        text = outputs.get(path) or open(rel(path)).read()
        begin, end = BEGIN % name, END % name
        if begin not in text or end not in text:
            raise SystemExit("%s has no generated %s section" % (path, name))
        outputs[path] = text[:text.index(begin)] + begin + "\n" + content + "\n" + end + text[text.index(end) + len(end):]
    stale = []
    for path, text in outputs.items():
        full = rel(path)
        current = open(full).read() if os.path.exists(full) else None
        if current == text:
            continue
        if args.check:
            stale.append(path)
        else:
            with open(full, "w") as handle:
                handle.write(text)
            print("wrote %s" % path)
    if stale:
        raise SystemExit("generated files are out of date (run scripts/docs/generate.py): " + ", ".join(stale))


if __name__ == "__main__":
    main()
