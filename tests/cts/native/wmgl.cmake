# VK-GL-CTS target for the WineMetalGL native platform (tests/cts/native).
# Pass -DWMGL_SOURCE_DIR=<WineMetalGL checkout> -DWMGL_LIBRARY=<libwinemetalgl.dylib>.

message("*** Using WineMetalGL native target")

set(DEQP_TARGET_NAME "WineMetalGL")
set(DEQP_SUPPORT_GLES2 OFF)
set(DEQP_SUPPORT_GLES3 OFF)
set(DEQP_SUPPORT_EGL OFF)
set(DEQP_SUPPORT_OPENGL ON)
set(DEQP_SUPPORT_VULKAN OFF)

set(TCUTIL_PLATFORM_SRCS "${WMGL_SOURCE_DIR}/tests/cts/native/tcuWmglPlatform.cpp")
include_directories("${WMGL_SOURCE_DIR}/include")
set(DEQP_PLATFORM_LIBRARIES "${WMGL_LIBRARY}")
