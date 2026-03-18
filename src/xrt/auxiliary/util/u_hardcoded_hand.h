// Copyright 2022-2024, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Hardcoded hand pose utility with real-time MediaPipe webcam bypass.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author Ankit (Enhanced with Real-time MediaPipe)
 * @ingroup aux_util
 */

#pragma once

#include "xrt/xrt_defines.h"
#include "math/m_api.h"
#include "os/os_time.h"
#include "util/u_misc.h"
#include "util/u_logging.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <errno.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MP_WEBCAM_SCRIPT_PATH "/home/ankit/monado/src/xrt/tracking/mediapipe/mp.py"

struct __attribute__((packed)) MonadoHandData {
    uint8_t left_active;
    uint8_t right_active;
    float left_world[21][3];
    float right_world[21][3];
    float left_screen[21][3];
    float right_screen[21][3];
    uint32_t width;
    uint32_t height;
};

struct u_mp_client {
    pid_t pid;
    FILE *to_py;
    FILE *from_py;
    int shm_fd;
    struct MonadoHandData *shared_data;
    struct xrt_hand_joint_set left;
    struct xrt_hand_joint_set right;
};

static struct u_mp_client *g_mp_client = NULL;

static inline struct xrt_vec3
u_vec3_lerp(struct xrt_vec3 a, struct xrt_vec3 b, float t)
{
	return (struct xrt_vec3){
	    a.x + (b.x - a.x) * t,
	    a.y + (b.y - a.y) * t,
	    a.z + (b.z - a.z) * t,
	};
}

static inline void
u_mp21_to_xrt26(const struct xrt_vec3 mp[21], const struct xrt_vec3 mp_screen[21], struct xrt_hand_joint_set *out, bool is_right)
{
	memset(out, 0, sizeof(*out));
	const uint64_t pos_flags = XRT_SPACE_RELATION_POSITION_VALID_BIT | XRT_SPACE_RELATION_POSITION_TRACKED_BIT;
    const float radius = 0.015f;

    // Estimate depth using wrist (0) to middle knuckle (9) distance.
    float world_scale = 0.09f;

    float aspect_ratio = 1080.0f / 1920.0f;
    float dx_s = mp_screen[9].x - mp_screen[0].x;
    float dy_s = (mp_screen[9].y - mp_screen[0].y) * aspect_ratio;
    float screen_scale = sqrtf(dx_s*dx_s + dy_s*dy_s);
    if (screen_scale < 0.001f) screen_scale = 0.1f; // fallback

    // Approximate focal length for a standard webcam
    float focal_length = 0.35f; 
    
    // Calculate the absolute depth in meters
    float depth = (world_scale / screen_scale) * focal_length;

    float wrist_screen_x = mp_screen[0].x;
    float wrist_screen_y = mp_screen[0].y;

    // --- MANUAL OFFSETS ---
    // Tweak these values if the virtual hand is consistently off-center from the real hand.
    // Positive manual_offset_x = shifts hand right
    // Positive manual_offset_y = shifts hand up
    float manual_offset_x = 0.0f; 
    float manual_offset_y = 0.9f; // Shifted slightly down as an example, tune as needed
    // ----------------------

    // Convert pixel coordinate to world coordinate at distance `depth`.
    float offset_x = -(wrist_screen_x - 0.5f) * (depth / focal_length) + manual_offset_x;
    float offset_y = -(wrist_screen_y - 0.5f) * (depth / focal_length) * aspect_ratio + manual_offset_y;
    
    // In StereoKit, head is roughly at Z=0 looking down -Z. Depth pushes into -Z.
    float offset_z = -depth;

    struct xrt_vec3 offset = {offset_x, offset_y, offset_z};

    // MediaPipe's world_landmarks distance scales with depth, we need to normalize 
    // it so the 3D hand mesh doesn't physically shrink when further away.
    float dx_w = mp[9].x - mp[0].x;
    float dy_w = mp[9].y - mp[0].y;
    float dz_w = mp[9].z - mp[0].z;
    float mp_world_scale = sqrtf(dx_w*dx_w + dy_w*dy_w + dz_w*dz_w);
    if (mp_world_scale < 0.001f) mp_world_scale = 0.09f;
    float scale_correction = world_scale / mp_world_scale;

    static int frame_count = 0;
    if (frame_count++ % 30 == 0) {
        U_LOG_E("Hand Depth Debug: screen_scale=%.4f, depth=%.4f, offset_x=%.4f, offset_y=%.4f, offset_z=%.4f, scale_correction=%.2f", 
                screen_scale, depth, offset_x, offset_y, offset_z, scale_correction);
    }

#define SET_JOINT(idx, vec)                                                \
	do {                                                                   \
        float rel_x = ((vec).x - mp[0].x) * scale_correction; \
        float rel_y = ((vec).y - mp[0].y) * scale_correction; \
        float rel_z = ((vec).z - mp[0].z) * scale_correction; \
		out->values.hand_joint_set_default[idx].relation.pose.position.x = rel_x + offset.x; \
        out->values.hand_joint_set_default[idx].relation.pose.position.y = -rel_y + offset.y; \
        out->values.hand_joint_set_default[idx].relation.pose.position.z = -rel_z + offset.z; \
		out->values.hand_joint_set_default[idx].relation.pose.orientation = (struct xrt_quat)XRT_QUAT_IDENTITY; \
		out->values.hand_joint_set_default[idx].relation.relation_flags = (enum xrt_space_relation_flags)pos_flags; \
		out->values.hand_joint_set_default[idx].radius = radius; \
	} while (0)

	SET_JOINT(XRT_HAND_JOINT_WRIST, mp[0]);
	SET_JOINT(XRT_HAND_JOINT_THUMB_METACARPAL, mp[1]);
	SET_JOINT(XRT_HAND_JOINT_THUMB_PROXIMAL,   mp[2]);
	SET_JOINT(XRT_HAND_JOINT_THUMB_DISTAL,     mp[3]);
	SET_JOINT(XRT_HAND_JOINT_THUMB_TIP,        mp[4]);
	SET_JOINT(XRT_HAND_JOINT_INDEX_METACARPAL,   u_vec3_lerp(mp[0], mp[5], 0.5f));
	SET_JOINT(XRT_HAND_JOINT_INDEX_PROXIMAL,     mp[5]);
	SET_JOINT(XRT_HAND_JOINT_INDEX_INTERMEDIATE, mp[6]);
	SET_JOINT(XRT_HAND_JOINT_INDEX_DISTAL,       mp[7]);
	SET_JOINT(XRT_HAND_JOINT_INDEX_TIP,          mp[8]);
	SET_JOINT(XRT_HAND_JOINT_MIDDLE_METACARPAL,   u_vec3_lerp(mp[0], mp[9], 0.5f));
	SET_JOINT(XRT_HAND_JOINT_MIDDLE_PROXIMAL,     mp[9]);
	SET_JOINT(XRT_HAND_JOINT_MIDDLE_INTERMEDIATE, mp[10]);
	SET_JOINT(XRT_HAND_JOINT_MIDDLE_DISTAL,       mp[11]);
	SET_JOINT(XRT_HAND_JOINT_MIDDLE_TIP,          mp[12]);
	SET_JOINT(XRT_HAND_JOINT_RING_METACARPAL,   u_vec3_lerp(mp[0], mp[13], 0.5f));
	SET_JOINT(XRT_HAND_JOINT_RING_PROXIMAL,     mp[13]);
	SET_JOINT(XRT_HAND_JOINT_RING_INTERMEDIATE, mp[14]);
	SET_JOINT(XRT_HAND_JOINT_RING_DISTAL,       mp[15]);
	SET_JOINT(XRT_HAND_JOINT_RING_TIP,          mp[16]);
	SET_JOINT(XRT_HAND_JOINT_LITTLE_METACARPAL,   u_vec3_lerp(mp[0], mp[17], 0.5f));
	SET_JOINT(XRT_HAND_JOINT_LITTLE_PROXIMAL,     mp[17]);
	SET_JOINT(XRT_HAND_JOINT_LITTLE_INTERMEDIATE, mp[18]);
	SET_JOINT(XRT_HAND_JOINT_LITTLE_DISTAL,       mp[19]);
	SET_JOINT(XRT_HAND_JOINT_LITTLE_TIP,          mp[20]);

	struct xrt_vec3 palm_pos = {
	    (mp[0].x + mp[5].x + mp[9].x + mp[13].x + mp[17].x) / 5.0f,
	    (mp[0].y + mp[5].y + mp[9].y + mp[13].y + mp[17].y) / 5.0f,
	    (mp[0].z + mp[5].z + mp[9].z + mp[13].z + mp[17].z) / 5.0f,
	};
	SET_JOINT(XRT_HAND_JOINT_PALM, palm_pos);
#undef SET_JOINT

	out->hand_pose.pose.position    = out->values.hand_joint_set_default[XRT_HAND_JOINT_WRIST].relation.pose.position;
	out->hand_pose.pose.orientation = (struct xrt_quat)XRT_QUAT_IDENTITY;
	out->hand_pose.relation_flags   = (enum xrt_space_relation_flags)pos_flags;
	out->is_active                  = true;
}

static inline void
u_mp_client_ensure_started()
{
    if (g_mp_client) return;

    U_LOG_I("Starting MediaPipe Webcam Server bypass...");
    int to_py[2], from_py[2];
    if (pipe(to_py) != 0 || pipe(from_py) != 0) return;

    pid_t pid = fork();
    if (pid < 0) return;

    if (pid == 0) {
        dup2(to_py[0], STDIN_FILENO);
        dup2(from_py[1], STDOUT_FILENO);
        close(to_py[1]); close(from_py[0]);
        const char *args[] = {"python3", MP_WEBCAM_SCRIPT_PATH, "--webcam-server", NULL};
        execvp("python3", (char *const *)args);
        _exit(127);
    }

    close(to_py[0]); close(from_py[1]);
    g_mp_client = U_TYPED_CALLOC(struct u_mp_client);
    g_mp_client->pid = pid;
    g_mp_client->to_py = fdopen(to_py[1], "w");
    g_mp_client->from_py = fdopen(from_py[0], "r");

    // Wait for python to create the shared memory by listening for READY
    char ready_buf[64];
    if (fgets(ready_buf, sizeof(ready_buf), g_mp_client->from_py)) {
        U_LOG_I("MediaPipe Webcam Server indicated READY");
    }

    g_mp_client->shm_fd = shm_open("monado_mp_data", O_RDONLY, 0666);
    if (g_mp_client->shm_fd < 0) {
        U_LOG_E("Failed to open shared memory 'monado_mp_data': %s", strerror(errno));
        return;
    }

    g_mp_client->shared_data = (struct MonadoHandData *)mmap(NULL, sizeof(struct MonadoHandData), PROT_READ, MAP_SHARED, g_mp_client->shm_fd, 0);
    if (g_mp_client->shared_data == MAP_FAILED) {
        U_LOG_E("Failed to mmap shared memory: %s", strerror(errno));
        g_mp_client->shared_data = NULL;
    }

    U_LOG_I("MediaPipe Shared Memory Mapped Successfully!");
}

static inline void
u_hand_joint_set_fill_hardcoded(struct xrt_hand_joint_set *set, bool is_right)
{
    u_mp_client_ensure_started();
    if (!g_mp_client || !g_mp_client->shared_data) return;

    struct MonadoHandData *data = g_mp_client->shared_data;
    
    // Process Left Hand
    if (data->left_active) {
        struct xrt_vec3 world[21];
        struct xrt_vec3 screen[21];
        for (int i=0; i<21; i++) {
            world[i].x = data->left_world[i][0];
            world[i].y = data->left_world[i][1];
            world[i].z = data->left_world[i][2];
            screen[i].x = data->left_screen[i][0];
            screen[i].y = data->left_screen[i][1];
            screen[i].z = data->left_screen[i][2];
        }
        u_mp21_to_xrt26(world, screen, &g_mp_client->left, false);
    } else {
        g_mp_client->left.is_active = false;
    }

    // Process Right Hand
    if (data->right_active) {
        struct xrt_vec3 world[21];
        struct xrt_vec3 screen[21];
        for (int i=0; i<21; i++) {
            world[i].x = data->right_world[i][0];
            world[i].y = data->right_world[i][1];
            world[i].z = data->right_world[i][2];
            screen[i].x = data->right_screen[i][0];
            screen[i].y = data->right_screen[i][1];
            screen[i].z = data->right_screen[i][2];
        }
        u_mp21_to_xrt26(world, screen, &g_mp_client->right, true);
    } else {
        g_mp_client->right.is_active = false;
    }



    if (is_right) *set = g_mp_client->right;
    else *set = g_mp_client->left;
}

#ifdef __cplusplus
}
#endif
