// GL 4.0 shader subroutines (GL_ARB_shader_subroutine).
//
// glslang does not implement subroutines, so they are lowered in the GLSL
// source before it parses the shader:
//
//   subroutine vec4 Shade(vec4 c);              (a subroutine type: removed)
//   subroutine(Shade) vec4 red(vec4 c) { .. }   (the function stays, without
//                                                the subroutine qualifier)
//   subroutine uniform Shade shade;             becomes
//   uniform int wmgl_su<S>_shade;               (the selected function's index)
//   shade(color)                                becomes
//   wmgl_sc<S>_shade(color)                     a dispatcher that switches on
//                                               the index and calls the function
//
// <S> is the stage number, so stages with subroutine uniforms of the same
// name keep separate selections. The selection uniforms are hidden from the
// GL interface; the draw path writes the context's glUniformSubroutinesuiv
// state into them (DrawState).
#pragma once

#include <string>
#include <vector>

#include "core/shader/compiler.h"

namespace wmgl::shader {

struct SubroutineLowering {
    bool ok = true;
    std::string source;              // lowered GLSL (the input when it has no subroutines)
    std::string log;                 // compile errors
    SubroutineStageInfo info;
};

// Lowers the subroutines of one shader object of `stage`.
SubroutineLowering lowerSubroutines(Stage stage, const std::string& source);

// The hidden default-block uniform holding uniform `name`'s selection.
std::string subroutineSelectionUniform(Stage stage, const std::string& name);
bool isSubroutineSelectionUniform(const std::string& name);

}  // namespace wmgl::shader
