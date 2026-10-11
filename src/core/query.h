// Query objects, conditional rendering and the draw-path hooks they need
// (GL 3.3 sections 2.14, 2.15 and 4.1.6).
#pragma once

#import <Metal/Metal.h>

#include "core/gl/types.h"

namespace wmgl {

class Context;
class Query;

// Index of a query target in ContextState::activeQueries.
enum QueryIndex {
    kQuerySamplesPassed,
    kQueryAnySamplesPassed,
    kQueryPrimitivesGenerated,
    kQueryXfbPrimitivesWritten,
    kQueryTimeElapsed,
    kQueryAnySamplesPassedConservative,
    kQueryIndexCount,
    // GL 4.0 indexed queries: PRIMITIVES_GENERATED and
    // TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN of vertex streams 1-3.
    kQueryStreamFirst = kQueryIndexCount,
    kQuerySlotCount = kQueryStreamFirst + 6
};
constexpr int kMaxVertexStreams = 4;
int queryTargetIndex(Context& ctx, GLenum target);  // -1 if not a query target
// The activeQueries slot of `target` for vertex stream `stream` (-1: invalid).
int queryTargetIndex(Context& ctx, GLenum target, GLuint stream);

// Draw-path hooks. queriesBeforeDraw points the encoder's visibility counter
// at the active occlusion queries; queriesSuspendVisibility stops counting
// for internal draws (clears); queriesCountPrimitives feeds PRIMITIVES_GENERATED.
// Before a draw opens its pass: when the command buffer's occlusion slots
// are used up, flushes it so the next pass gets a fresh visibility buffer.
void queriesPrepareVisibility(Context& ctx);
void queriesBeforeDraw(Context& ctx, id<MTLRenderCommandEncoder> encoder);
void queriesSuspendVisibility(Context& ctx, id<MTLRenderCommandEncoder> encoder);
void queriesCountPrimitives(Context& ctx, GLenum mode, GLsizei count, GLsizei instances);
uint64_t primitiveCount(GLenum mode, GLsizei count);

// False when conditional rendering discards the current command.
bool conditionalRenderAllows(Context& ctx);

// Makes the query's result final if possible; with `wait`, blocks until it is.
bool queryResultReady(Context& ctx, Query& query, bool wait);

}  // namespace wmgl
