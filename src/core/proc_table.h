#pragma once

namespace wmgl {

struct ProcEntry {
    const char* name;
    void* proc;
};

// Generated in gl_entry.gen.cpp, sorted by name.
extern const ProcEntry kProcTable[];
extern const unsigned kProcTableSize;

// Entry point for a GL command name, or nullptr if WineMetalGL does not export it.
void* lookupProc(const char* name);

}  // namespace wmgl
