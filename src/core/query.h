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
    // GL 4.6 / ARB_transform_feedback_overflow_query: TRANSFORM_FEEDBACK_OVERFLOW,
    // and TRANSFORM_FEEDBACK_STREAM_OVERFLOW of vertex stream 0.
    kQueryXfbOverflow,
    kQueryXfbStreamOverflow,
    // GL 4.6 / ARB_pipeline_statistics_query: kQueryStatisticsFirst + PipelineStatistic.
    kQueryStatisticsFirst,
    kQueryIndexCount = kQueryStatisticsFirst + 11,
    // GL 4.0 indexed queries: PRIMITIVES_GENERATED,
    // TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN and (GL 4.6)
    // TRANSFORM_FEEDBACK_STREAM_OVERFLOW of vertex streams 1-3.
    kQueryStreamFirst = kQueryIndexCount,
    kQuerySlotCount = kQueryStreamFirst + 9
};
constexpr int kMaxVertexStreams = 4;
enum PipelineStatistic {
    kStatVerticesSubmitted,
    kStatPrimitivesSubmitted,
    kStatVertexShaderInvocations,
    kStatTessControlShaderPatches,
    kStatTessEvaluationShaderInvocations,
    kStatGeometryShaderInvocations,
    kStatGeometryShaderPrimitivesEmitted,
    kStatFragmentShaderInvocations,
    kStatComputeShaderInvocations,
    kStatClippingInputPrimitives,
    kStatClippingOutputPrimitives,
    kStatCount
};
static_assert(kQueryStatisticsFirst + kStatCount == kQueryIndexCount);
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
// Transform feedback of vertex stream `stream` dropped primitives for lack of
// buffer space: sets the active overflow queries.
void queriesNoteXfbOverflow(Context& ctx, GLuint stream);

// Pipeline statistics (GL 4.6) counted on the CPU from what the draw paths
// do; FRAGMENT_SHADER_INVOCATIONS is counted by the fragment functions.
bool queriesCountingStatistics(Context& ctx);  // any statistics query is active
bool queriesCounting(Context& ctx, PipelineStatistic statistic);
void queriesCountStatistic(Context& ctx, PipelineStatistic statistic, uint64_t n);
// One restart-delimited run of a draw command's vertices:
// VERTICES_SUBMITTED, PRIMITIVES_SUBMITTED, VERTEX_SHADER_INVOCATIONS and,
// with `clipped` (its primitives reach the clipper unchanged: no tessellation
// or geometry shader), CLIPPING_INPUT/OUTPUT_PRIMITIVES. Metal does not
// report what clipping does, so clipping output counts its input.
void queriesCountSubmitted(Context& ctx, GLenum mode, GLsizei vertices, GLsizei instances, bool clipped);
// FRAGMENT_SHADER_INVOCATIONS: the counter the current command buffer's
// draws increment, or false with no query active.
bool queriesFragmentCounter(Context& ctx, __strong id<MTLBuffer>& buffer, NSUInteger& offset);
uint64_t primitiveCount(GLenum mode, GLsizei count);

// False when conditional rendering discards the current command.
bool conditionalRenderAllows(Context& ctx);

// Makes the query's result final if possible; with `wait`, blocks until it is.
bool queryResultReady(Context& ctx, Query& query, bool wait);

}  // namespace wmgl
