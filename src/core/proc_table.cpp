#include "core/proc_table.h"

#include <cstring>

namespace wmgl {

void* lookupProc(const char* name) {
    if (!name) return nullptr;
    unsigned low = 0, high = kProcTableSize;
    while (low < high) {
        const unsigned mid = (low + high) / 2;
        const int order = std::strcmp(kProcTable[mid].name, name);
        if (order == 0) return kProcTable[mid].proc;
        if (order < 0) low = mid + 1;
        else high = mid;
    }
    return nullptr;
}

}  // namespace wmgl
