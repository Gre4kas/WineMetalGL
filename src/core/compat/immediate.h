// Immediate mode (glBegin/glEnd, GL 3.3 compatibility section 2.6) and the
// vertex batches display lists record.
//
// A batch keeps, per vertex, the value of every attribute slot (generic and
// conventional, compat/fixed_state.h). Slots set between Begin and End are
// "varying" and drawn from the per-vertex data; the others keep their
// current value. glEnd draws the batch as a vertex array draw (quads,
// quad strips and polygons go through the draw path's index generation).
//
// A batch compiled into a display list does not know the current values at
// the time it is called: a slot first set at vertex k takes, for vertices
// before k, the current value when the list executes (firstSet).
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

#include "core/compat/fixed_state.h"
#include "core/gl/types.h"
#include "core/util/ref.h"

namespace wmgl {

class Context;
class DisplayList;
class VertexArray;

struct ImmediateBatch {
    using Value = std::array<uint32_t, 4>;  // raw bits of 4 floats or integers
    GLenum mode = GL_POINTS;
    uint32_t varying = 0;    // slots set inside Begin/End (bit per slot)
    uint32_t known = 0;      // slots whose value before the first set is known
    std::array<GLenum, kAttribSlotCount> types{};       // GL_FLOAT, GL_INT or GL_UNSIGNED_INT
    std::array<uint32_t, kAttribSlotCount> firstSet{};  // vertex where an unknown slot was first set
    std::array<Value, kAttribSlotCount> current{};
    std::vector<Value> data;  // vertexCount() * kAttribSlotCount values

    size_t vertexCount() const { return data.size() / kAttribSlotCount; }
    void begin(GLenum primitive, const Context* live);  // live: current values are known
    void set(int slot, const Value& value, GLenum type);
    void vertex(const Value& position);
};

struct ImmediateState {
    bool active = false;  // between glBegin and glEnd
    ImmediateBatch batch;
    RefPtr<VertexArray> vao;          // draws batches (client-memory arrays)
    std::vector<ImmediateBatch::Value> scratch;
};

struct ListCompiler {
    GLuint name = 0;
    GLenum mode = GL_COMPILE;  // or GL_COMPILE_AND_EXECUTE
    RefPtr<DisplayList> list;
    bool inBegin = false;  // a compiled glBegin without its glEnd yet
    ImmediateBatch batch;  // its vertices, drawn as one batch at glEnd
    // The primitive is recorded command by command instead (another command
    // was compiled inside it, or the list ends before its glEnd): the
    // batch so far replays through immediate mode (compat::splitBatch).
    bool split = false;
};

namespace compat {

ImmediateState& immediate(Context& ctx);

// Sets current attribute `slot` (generic index or conventional slot) to
// the raw `value` of `type`, inside or outside Begin/End, compiling it into
// the display list being built. Generic attribute 0 inside Begin/End (and
// gl_Vertex) emit a vertex instead (emitVertex).
void setAttribute(Context& ctx, int slot, const ImmediateBatch::Value& value, GLenum type);
void setAttribute(Context& ctx, int slot, float x, float y, float z, float w);
void emitVertex(Context& ctx, float x, float y, float z, float w);

// Draws a batch; `batch` from a display list takes unknown values from the
// current state first.
void drawBatch(Context& ctx, const ImmediateBatch& batch);

// Display list compilation: whether a command being compiled must also run
// now (always true when no list is being compiled), and recording.
bool listExecutes(const Context& ctx);
bool listCompiling(const Context& ctx);
void recordCommand(Context& ctx, std::function<void(Context&)> command);
// Ends batching of a compiled glBegin (see ListCompiler::split).
void splitBatch(ListCompiler& lc);

// A vertex array draw compiled into a display list: the arrays are read now
// (GL 3.3 compatibility section 5.5.1) into a recorded batch. `indices` are
// the vertices in draw order. Returns whether the draw must also execute.
bool compileArrayDraw(Context& ctx, GLenum mode, const std::vector<uint32_t>& indices);

}  // namespace compat
}  // namespace wmgl
