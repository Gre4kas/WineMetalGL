// Quality-of-service class for the threads that render and present.
#pragma once

namespace wmgl {

// Asks for the user-interactive QoS class for the calling thread, once per
// thread: what a native game's render thread has, so macOS keeps it on
// performance cores while the game's worker threads load every core (Wine
// threads run at the default class; DOOM (2016), issue #36).
// WINEMETALGL_THREAD_QOS=0 leaves threads as they are.
void raiseThreadQos(const char* role);

}  // namespace wmgl
