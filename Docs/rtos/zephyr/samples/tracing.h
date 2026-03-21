/*
 * Orbuculum Zephyr Tracing Wrapper
 *
 * Place this file at: src/zephyr/tracing/tracing.h
 * It shadows Zephyr's <zephyr/tracing/tracing.h> and applies our overrides.
 *
 * In CMakeLists.txt, add src/ BEFORE Zephyr's include paths:
 *
 *   target_include_directories(kernel BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
 *   target_include_directories(zephyr BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
 *
 * See Docs/rtos/zephyr/zephyr-object-tracking.md for full documentation.
 */

#ifndef ORBUCULUM_TRACING_WRAPPER_H
#define ORBUCULUM_TRACING_WRAPPER_H

/* Include the real Zephyr tracing.h first (fully) */
#include_next <zephyr/tracing/tracing.h>

/* Now override the blocking hooks with our DWT comp2 writes */
#include "orbuculum_obj_trace.h"

#endif /* ORBUCULUM_TRACING_WRAPPER_H */
