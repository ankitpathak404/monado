# Copyright 2024, Collabora, Ltd.
# SPDX-License-Identifier: BSL-1.0

# Build the MediaPipe hand tracking sync library.
# This replaces Mercury as the t_hand_tracking_sync backend when the
# XRT_BUILD_DRIVER_HT driver is enabled and XRT_FEATURE_HAND_TRACKING_MEDIAPIPE
# is ON.

add_library(t_ht_mediapipe STATIC mp_sync.c)

target_link_libraries(
	t_ht_mediapipe
	PUBLIC
		aux-includes
		xrt-external-cjson
	PRIVATE
		aux_math
		aux_os
		aux_util
)

target_include_directories(t_ht_mediapipe INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
