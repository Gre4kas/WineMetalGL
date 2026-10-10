// Display lists (GL 3.3 compatibility profile section 5.5).
//
// A list is the sequence of commands compiled into it. The generated entry
// points record list-compilable commands as closures with their arguments
// (pointer arguments copied, as GL requires: lists hold values, not
// pointers); Begin/End geometry and vertex array draws are recorded as one
// batch that draws in a single call (compat/immediate.h). Lists are shared
// between contexts like textures.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "core/gl/types.h"
#include "core/state.h"
#include "core/util/ref.h"

namespace wmgl {

class Context;

class DisplayList : public RefCounted {
public:
    std::vector<std::function<void(Context&)>> commands;
};

namespace list {

// Helpers for the generated recorders (gl_list.gen.cpp).

// Values a COMPSIZE(pname) parameter array holds (glLightfv, glFogfv, ...).
int pnameCount(GLenum pname);

// Client pixels of a compiled command, copied with the unpack state in
// effect, which is restored around the call when the list executes.
struct Pixels {
    std::vector<uint8_t> bytes;
    PixelStore unpack;
    bool null = true;
    const void* data() const { return null ? nullptr : bytes.data(); }
};
// `threeD`: a three-dimensional image (IMAGE_HEIGHT and SKIP_IMAGES apply).
Pixels capturePixels(Context& ctx, const void* data, GLenum format, GLenum type, GLsizei width, GLsizei height,
                     GLsizei depth, bool threeD);
Pixels captureBitmap(Context& ctx, const void* data, GLsizei width, GLsizei height);
Pixels captureBytes(Context& ctx, const void* data, GLsizei size);
struct UnpackScope {
    UnpackScope(Context& ctx, const PixelStore& unpack);
    ~UnpackScope();
    Context& ctx;
    PixelStore saved;
    RefPtr<Buffer> savedBuffer;
};

// Bytes of glCallLists' name array.
size_t callListsBytes(GLsizei n, GLenum type);

// Appends to the list being compiled; returns whether the command also runs now.
bool record(Context& ctx, std::function<void(Context&)> command);

}  // namespace list
}  // namespace wmgl
