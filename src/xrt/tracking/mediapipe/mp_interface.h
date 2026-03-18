// Copyright 2024, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Public interface for MediaPipe-based hand tracking sync processor.
 * @author Ankit
 * @ingroup aux_tracking
 */
#pragma once

#include "tracking/t_hand_tracking.h"

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * @brief Create a MediaPipe-based hand tracking sync processor.
 *
 * Spawns mp.py as a subprocess and communicates with it via stdin/stdout pipes.
 * Frames are sent as raw RGB bytes; joint results are received as JSON.
 *
 * @param script_path  Absolute path to mp.py.  If NULL, the path is derived
 *                     from the environment variable XRT_MP_SCRIPT_PATH, or
 *                     falls back to the compile-time default.
 *
 * @ingroup aux_tracking
 */
struct t_hand_tracking_sync *
t_hand_tracking_sync_mediapipe_create(const char *script_path);

#ifdef __cplusplus
} // extern "C"
#endif
