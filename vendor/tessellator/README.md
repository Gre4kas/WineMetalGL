# Reference tessellator

`tessellator.cpp` and `tessellator.hpp` are Microsoft's D3D11 reference
tessellator (MIT, `LICENSE.txt`) as carried in Mesa's
`src/gallium/auxiliary/tessellator` at commit
`65c2dc5a634b6ee31979546ec5eb5212d6ffe721`. WineMetalGL uses it for GL
tessellation (`src/core/tess/tessellator.h`), as Mesa's draw module does.

Local change (marked `WineMetalGL:`): Mesa's `util/macros.h` include is
replaced by the one macro the file uses.
