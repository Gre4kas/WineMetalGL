#include "core/util/thread_qos.h"

#include <pthread/qos.h>

#include <cstdlib>

#include "core/util/log.h"

namespace wmgl {

void raiseThreadQos(const char* role) {
    static thread_local bool raised = false;
    if (raised) return;
    raised = true;
    static const bool enabled = [] {
        const char* v = std::getenv("WINEMETALGL_THREAD_QOS");
        return !v || v[0] != '0';
    }();
    if (!enabled) return;
    const qos_class_t before = qos_class_self();
    if (before == QOS_CLASS_USER_INTERACTIVE) return;
    const int error = pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    WMGL_INFO(Driver, "%s thread QoS 0x%x -> user-interactive%s", role, static_cast<unsigned>(before), error ? " failed" : "");
}

}  // namespace wmgl
