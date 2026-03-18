// Copyright 2024, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0

#include "mp_interface.h"
#include "xrt/xrt_defines.h"
#include "xrt/xrt_frame.h"
#include "tracking/t_hand_tracking.h"
#include "util/u_misc.h"
#include "util/u_logging.h"
#include "math/m_mathinclude.h"
#include "os/os_time.h"
#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>

#define MP_DEFAULT_SCRIPT_PATH "/home/ankit/monado/src/xrt/tracking/mediapipe/mp.py"
#define MP_JOINT_RADIUS        0.01f

// Webcam FOV — measure yours or use 78° as a safe default for most USB webcams
#define MP_CAMERA_HFOV_RAD     (78.0f * (M_PI / 180.0f))

// Average wrist-to-middle-MCP distance in metres
#define MP_HAND_WRIST_TO_MCP_M  0.08f

// NO depth scale — let the geometry do the work
// Clamp to sane range only
#define MP_DEPTH_MIN_M          0.15f
#define MP_DEPTH_MAX_M          1.00f

#define MP_LOG_E(...) U_LOG_E(__VA_ARGS__)
#define MP_LOG_W(...) U_LOG_W(__VA_ARGS__)
#define MP_LOG_I(...) U_LOG_I(__VA_ARGS__)

struct mp_sync
{
    struct t_hand_tracking_sync base;
    pid_t python_pid;
    int   stdin_fd;
    int   stdout_fd;
    FILE *to_py;
    FILE *from_py;
};

static inline struct xrt_vec3
lerp3(struct xrt_vec3 a, struct xrt_vec3 b, float t)
{
    return (struct xrt_vec3){
        a.x + (b.x - a.x) * t,
        a.y + (b.y - a.y) * t,
        a.z + (b.z - a.z) * t,
    };
}

static inline float
vec3_dist(struct xrt_vec3 a, struct xrt_vec3 b)
{
    float dx = a.x - b.x;
    float dy = a.y - b.y;
    float dz = a.z - b.z;
    return sqrtf(dx*dx + dy*dy + dz*dz);
}

static float
estimate_depth(struct xrt_vec3 world_wrist,
               struct xrt_vec3 world_mcp9,
               float screen_wrist_x, float screen_wrist_y,
               float screen_mcp9_x,  float screen_mcp9_y,
               float frame_w,        float frame_h)
{
    // Real metric span from MediaPipe world landmarks
    float world_span = vec3_dist(world_wrist, world_mcp9);
    if (world_span < 0.001f)
        world_span = MP_HAND_WRIST_TO_MCP_M;

    // Screen span in pixels
    float dx = (screen_mcp9_x - screen_wrist_x) * frame_w;
    float dy = (screen_mcp9_y - screen_wrist_y) * frame_h;
    float screen_span_px = sqrtf(dx*dx + dy*dy);
    if (screen_span_px < 1.0f)
        return 0.4f;

    // Focal length in pixels
    float focal_px = (frame_w / 2.0f) / tanf(MP_CAMERA_HFOV_RAD / 2.0f);

    // Depth via similar triangles — no fudge factor
    float depth = focal_px * world_span / screen_span_px;

    // Log for tuning
    MP_LOG_I("depth=%.3f world_span=%.3f screen_span=%.1fpx focal=%.1fpx",
             depth, world_span, screen_span_px, focal_px);

    if (depth < MP_DEPTH_MIN_M) depth = MP_DEPTH_MIN_M;
    if (depth > MP_DEPTH_MAX_M) depth = MP_DEPTH_MAX_M;
    return depth;
}
// TEMPORARY: hardcode wrist at 0.3m in front of camera
// If this appears correctly, depth estimation is the problem
// If this also appears wrong, it's a coordinate space problem
struct xrt_vec3 wrist_cam = {0.0f, 0.0f, -0.3f};
static struct xrt_vec3
screen_to_camera(float norm_x, float norm_y, float depth_m,
                 float frame_w, float frame_h)
{
    float fov_x = MP_CAMERA_HFOV_RAD;
    float fov_y = fov_x * (frame_h / frame_w);

    return (struct xrt_vec3){
        .x =  (norm_x - 0.5f) * 2.0f * depth_m * tanf(fov_x / 2.0f),
        .y = -(norm_y - 0.5f) * 2.0f * depth_m * tanf(fov_y / 2.0f),
        .z = -depth_m,
    };
}

static void
init_hand_set(struct xrt_hand_joint_set *out)
{
    memset(out, 0, sizeof(*out));
    out->is_active = false;
    out->hand_pose.pose.orientation.w = 1.0f;
    out->hand_pose.relation_flags = 0;
    for (int i = 0; i < XRT_HAND_JOINT_COUNT; i++) {
        out->values.hand_joint_set_default[i].relation.pose.orientation.w = 1.0f;
        out->values.hand_joint_set_default[i].relation.relation_flags = 0;
    }
}

static void
mp21_to_xrt26(const struct xrt_vec3 mp[21], struct xrt_hand_joint_set *out)
{
    init_hand_set(out);

    const uint64_t full_flags =
        XRT_SPACE_RELATION_POSITION_VALID_BIT    |
        XRT_SPACE_RELATION_POSITION_TRACKED_BIT  |
        XRT_SPACE_RELATION_ORIENTATION_VALID_BIT |
        XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT;

#define SET_JOINT(xrt_idx, vec) \
    do { \
        out->values.hand_joint_set_default[xrt_idx].relation.pose.position = (vec); \
        out->values.hand_joint_set_default[xrt_idx].relation.pose.orientation = \
            (struct xrt_quat){0, 0, 0, 1}; \
        out->values.hand_joint_set_default[xrt_idx].relation.relation_flags = full_flags; \
        out->values.hand_joint_set_default[xrt_idx].radius = MP_JOINT_RADIUS; \
    } while (0)

    SET_JOINT(XRT_HAND_JOINT_WRIST, mp[0]);

    out->is_active = true;
    out->hand_pose.pose.position    = mp[0];
    out->hand_pose.pose.orientation = (struct xrt_quat){0, 0, 0, 1};
    out->hand_pose.relation_flags   = full_flags;

    SET_JOINT(XRT_HAND_JOINT_THUMB_METACARPAL, mp[1]);
    SET_JOINT(XRT_HAND_JOINT_THUMB_PROXIMAL,   mp[2]);
    SET_JOINT(XRT_HAND_JOINT_THUMB_DISTAL,     mp[3]);
    SET_JOINT(XRT_HAND_JOINT_THUMB_TIP,        mp[4]);

    SET_JOINT(XRT_HAND_JOINT_INDEX_METACARPAL,   lerp3(mp[0], mp[5], 0.5f));
    SET_JOINT(XRT_HAND_JOINT_INDEX_PROXIMAL,     mp[5]);
    SET_JOINT(XRT_HAND_JOINT_INDEX_INTERMEDIATE, mp[6]);
    SET_JOINT(XRT_HAND_JOINT_INDEX_DISTAL,       mp[7]);
    SET_JOINT(XRT_HAND_JOINT_INDEX_TIP,          mp[8]);

    SET_JOINT(XRT_HAND_JOINT_MIDDLE_METACARPAL,   lerp3(mp[0], mp[9],  0.5f));
    SET_JOINT(XRT_HAND_JOINT_MIDDLE_PROXIMAL,     mp[9]);
    SET_JOINT(XRT_HAND_JOINT_MIDDLE_INTERMEDIATE, mp[10]);
    SET_JOINT(XRT_HAND_JOINT_MIDDLE_DISTAL,       mp[11]);
    SET_JOINT(XRT_HAND_JOINT_MIDDLE_TIP,          mp[12]);

    SET_JOINT(XRT_HAND_JOINT_RING_METACARPAL,   lerp3(mp[0], mp[13], 0.5f));
    SET_JOINT(XRT_HAND_JOINT_RING_PROXIMAL,     mp[13]);
    SET_JOINT(XRT_HAND_JOINT_RING_INTERMEDIATE, mp[14]);
    SET_JOINT(XRT_HAND_JOINT_RING_DISTAL,       mp[15]);
    SET_JOINT(XRT_HAND_JOINT_RING_TIP,          mp[16]);

    SET_JOINT(XRT_HAND_JOINT_LITTLE_METACARPAL,   lerp3(mp[0], mp[17], 0.5f));
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
}

static bool
parse_json_result(const char *line,
                  struct xrt_hand_joint_set *out_left,
                  struct xrt_hand_joint_set *out_right,
                  bool *got_left,
                  bool *got_right)
{
    *got_left  = false;
    *got_right = false;

    cJSON *root = cJSON_Parse(line);
    if (!root) {
        MP_LOG_W("mp_sync: failed to parse JSON: %s", line);
        return false;
    }

    float frame_w = 640.0f, frame_h = 480.0f;
    cJSON *fw = cJSON_GetObjectItemCaseSensitive(root, "frame_width");
    cJSON *fh = cJSON_GetObjectItemCaseSensitive(root, "frame_height");
    if (cJSON_IsNumber(fw)) frame_w = (float)cJSON_GetNumberValue(fw);
    if (cJSON_IsNumber(fh)) frame_h = (float)cJSON_GetNumberValue(fh);

    cJSON *hands_arr = cJSON_GetObjectItemCaseSensitive(root, "hands");
    if (!cJSON_IsArray(hands_arr)) {
        cJSON_Delete(root);
        return true;
    }

    cJSON *hand_obj = NULL;
    cJSON_ArrayForEach(hand_obj, hands_arr)
    {
        cJSON *side_item = cJSON_GetObjectItemCaseSensitive(hand_obj, "side");
        cJSON *wlm_arr   = cJSON_GetObjectItemCaseSensitive(hand_obj, "world_landmarks");
        cJSON *slm_arr   = cJSON_GetObjectItemCaseSensitive(hand_obj, "screen_landmarks");

        if (!cJSON_IsString(side_item) ||
            !cJSON_IsArray(wlm_arr)    ||
            !cJSON_IsArray(slm_arr))
            continue;

        if (cJSON_GetArraySize(wlm_arr) != 21 ||
            cJSON_GetArraySize(slm_arr) != 21)
            continue;

        // Parse world landmarks — hand-relative, metres, flip Y and Z
        struct xrt_vec3 mp_world[21];
        {
            int idx = 0;
            cJSON *lm = NULL;
            cJSON_ArrayForEach(lm, wlm_arr) {
                cJSON *x = cJSON_GetObjectItemCaseSensitive(lm, "x");
                cJSON *y = cJSON_GetObjectItemCaseSensitive(lm, "y");
                cJSON *z = cJSON_GetObjectItemCaseSensitive(lm, "z");
                if (!cJSON_IsNumber(x) || !cJSON_IsNumber(y) || !cJSON_IsNumber(z))
                    break;
                mp_world[idx].x =  (float)cJSON_GetNumberValue(x);
                mp_world[idx].y = -(float)cJSON_GetNumberValue(y);
                mp_world[idx].z = -(float)cJSON_GetNumberValue(z);
                idx++;
            }
            if (idx != 21) continue;
        }

        // Parse screen landmarks — normalised 0..1
        float screen_x[21], screen_y[21];
        {
            int idx = 0;
            cJSON *lm = NULL;
            cJSON_ArrayForEach(lm, slm_arr) {
                cJSON *x = cJSON_GetObjectItemCaseSensitive(lm, "x");
                cJSON *y = cJSON_GetObjectItemCaseSensitive(lm, "y");
                if (!cJSON_IsNumber(x) || !cJSON_IsNumber(y)) break;
                screen_x[idx] = (float)cJSON_GetNumberValue(x);
                screen_y[idx] = (float)cJSON_GetNumberValue(y);
                idx++;
            }
            if (idx != 21) continue;
        }

        // Estimate depth from hand size ratio
        float depth = estimate_depth(
            mp_world[0], mp_world[9],
            screen_x[0], screen_y[0],
            screen_x[9], screen_y[9],
            frame_w, frame_h);

        // Real wrist position in camera/view space
        struct xrt_vec3 wrist_cam = screen_to_camera(
            screen_x[0], screen_y[0], depth, frame_w, frame_h);

        MP_LOG_I("wrist_cam: x=%.3f y=%.3f z=%.3f depth=%.3f",
                 wrist_cam.x, wrist_cam.y, wrist_cam.z, depth);

        // All joints: offset from hand-relative origin to real camera position
        struct xrt_vec3 mp[21];
        for (int i = 0; i < 21; i++) {
            mp[i].x = (mp_world[i].x - mp_world[0].x) + wrist_cam.x;
            mp[i].y = (mp_world[i].y - mp_world[0].y) + wrist_cam.y;
            mp[i].z = (mp_world[i].z - mp_world[0].z) + wrist_cam.z;
        }

        const char *side = cJSON_GetStringValue(side_item);
        if (strcmp(side, "Left") == 0) {
            mp21_to_xrt26(mp, out_left);
            *got_left = true;
        } else if (strcmp(side, "Right") == 0) {
            mp21_to_xrt26(mp, out_right);
            *got_right = true;
        }
    }

    cJSON_Delete(root);
    return true;
}

static void
mp_sync_process(struct t_hand_tracking_sync *sync,
                struct xrt_frame *left_frame,
                struct xrt_frame *right_frame,
                struct xrt_hand_joint_set *out_left,
                struct xrt_hand_joint_set *out_right,
                int64_t *out_timestamp_ns)
{
    struct mp_sync *mps = (struct mp_sync *)sync;

    init_hand_set(out_left);
    init_hand_set(out_right);

    int64_t ts = left_frame   ? left_frame->timestamp
               : right_frame  ? right_frame->timestamp
               : os_monotonic_get_ns();
    *out_timestamp_ns = ts;

    fprintf(mps->to_py, "POLL\n");
    fflush(mps->to_py);

    char line_buf[65536];
    if (!fgets(line_buf, sizeof(line_buf), mps->from_py)) {
        MP_LOG_E("mp_sync: read from python failed (EOF?)");
        return;
    }

    bool got_left = false, got_right = false;
    parse_json_result(line_buf, out_left, out_right, &got_left, &got_right);

    *out_timestamp_ns = os_monotonic_get_ns();
}

static void
mp_sync_destroy(struct t_hand_tracking_sync *sync)
{
    struct mp_sync *mps = (struct mp_sync *)sync;

    if (mps->to_py) {
        fprintf(mps->to_py, "EXIT\n");
        fflush(mps->to_py);
        fclose(mps->to_py);
        mps->to_py = NULL;
    }
    if (mps->from_py) {
        fclose(mps->from_py);
        mps->from_py = NULL;
    }
    if (mps->python_pid > 0) {
        int status;
        waitpid(mps->python_pid, &status, 0);
        mps->python_pid = 0;
    }
    free(mps);
}

struct t_hand_tracking_sync *
t_hand_tracking_sync_mediapipe_create(const char *script_path)
{
    if (!script_path || script_path[0] == '\0')
        script_path = getenv("XRT_MP_SCRIPT_PATH");
    if (!script_path || script_path[0] == '\0')
        script_path = MP_DEFAULT_SCRIPT_PATH;

    MP_LOG_I("mp_sync: launching mediapipe server: %s", script_path);

    int to_python[2], from_python[2];
    if (pipe(to_python) != 0 || pipe(from_python) != 0) {
        MP_LOG_E("mp_sync: pipe() failed: %s", strerror(errno));
        return NULL;
    }

    pid_t pid = fork();
    if (pid < 0) {
        MP_LOG_E("mp_sync: fork() failed: %s", strerror(errno));
        close(to_python[0]);   close(to_python[1]);
        close(from_python[0]); close(from_python[1]);
        return NULL;
    }

    if (pid == 0) {
        dup2(to_python[0],   STDIN_FILENO);
        dup2(from_python[1], STDOUT_FILENO);
        close(to_python[1]);
        close(from_python[0]);
        const char *args[] = {"python3", (char *)script_path, "--webcam-server", NULL};
        execvp("python3", (char *const *)args);
        _exit(127);
    }

    close(to_python[0]);
    close(from_python[1]);

    FILE *to_py   = fdopen(to_python[1],  "w");
    FILE *from_py = fdopen(from_python[0], "r");

    if (!to_py || !from_py) {
        MP_LOG_E("mp_sync: fdopen() failed");
        kill(pid, SIGTERM);
        return NULL;
    }

    char ready_buf[64];
    if (!fgets(ready_buf, sizeof(ready_buf), from_py) ||
        strncmp(ready_buf, "READY", 5) != 0) {
        MP_LOG_E("mp_sync: Python did not send READY signal");
        fclose(to_py);
        fclose(from_py);
        kill(pid, SIGTERM);
        return NULL;
    }

    MP_LOG_I("mp_sync: Python mediapipe subprocess ready (pid %d)", (int)pid);

    struct mp_sync *mps = U_TYPED_CALLOC(struct mp_sync);
    mps->base.process  = mp_sync_process;
    mps->base.destroy  = mp_sync_destroy;
    mps->python_pid    = pid;
    mps->stdin_fd      = to_python[1];
    mps->stdout_fd     = from_python[0];
    mps->to_py         = to_py;
    mps->from_py       = from_py;

    return &mps->base;
}