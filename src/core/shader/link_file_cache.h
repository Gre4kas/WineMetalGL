// Link results kept on disk between runs (shader::link): a program linked
// again from the same inputs in a later run reads its GLSL -> MSL
// translation instead of compiling and translating the shaders again (load
// times; Metal's own cache then skips the MSL compile).
//
// One file per link key under WINEMETALGL_SHADER_CACHE (a directory; "0":
// off), else ~/Library/Caches/WineMetalGL/shaders. A file holds its full key
// (read back and compared: no collisions) and is valid only for the build
// that wrote it (the core's file identity and the format version are part
// of the name).
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/shader/compiler.h"

namespace wmgl::shader {

// The result stored for `key`, or null.
std::shared_ptr<LinkResult> loadLinkResult(const std::string& key);
// Stores a successful result for `key`.
void storeLinkResult(const std::string& key, const LinkResult& result);

// The byte form of a result (tests: a round trip gives the same bytes).
std::vector<uint8_t> serializeLinkResult(const LinkResult& result);
bool deserializeLinkResult(const std::vector<uint8_t>& bytes, LinkResult& result);

}  // namespace wmgl::shader
