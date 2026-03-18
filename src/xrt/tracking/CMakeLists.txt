# Copyright 2022, Collabora, Ltd.
# SPDX-License-Identifier: BSL-1.0

if(XRT_MODULE_MERCURY_HANDTRACKING)
	add_subdirectory(hand)
endif()

# MediaPipe hand tracking sync (always built alongside the HT driver)
if(XRT_BUILD_DRIVER_HANDTRACKING)
	add_subdirectory(mediapipe)
endif()
