"""
MediaPipe Hand Tracker for Monado.

Two modes:
  - Interactive (default):  python3 mp.py
      Opens the webcam, renders landmarks in a window.
  - IPC server:             python3 mp.py --server
      Reads raw RGB frames from stdin, writes JSON joint data to stdout.
      This mode is used when invoked as a subprocess by Monado's mp_sync.c.
  - Webcam server:          python3 mp.py --webcam-server
      Opens the webcam internally and runs MediaPipe in a loop.
      Writes the latest detection to stdout whenever it receives a newline on stdin.

IPC Protocol (--server mode):
  C → Python (stdin):
    Each frame is prefixed by a text header line:
        "<width> <height> <timestamp_ms>\n"
    followed immediately by exactly  width * height * 3  bytes of raw RGB data.
    To signal exit the C side closes stdin (EOF) or sends "EXIT\n".

  Python → C (stdout):
    One JSON line per frame:
        {"hands": [
            {"side": "Left",  "world_landmarks": [{x,y,z}, ...]},
            {"side": "Right", "world_landmarks": [{x,y,z}, ...]}
          ],
          "timestamp_ms": <int>}
    A hand entry is only present if that hand was detected.
    On error / no detection the JSON is {"hands": [], "timestamp_ms": <int>}.
"""

import cv2
import mediapipe as mp
from mediapipe.tasks import python
from mediapipe.tasks.python import vision
import time
import os
import sys
import json
import numpy as np
import struct
import threading
from multiprocessing import shared_memory

# ──────────────────────────────────────────────────────────────────────────────
# Shared tracker class
# ──────────────────────────────────────────────────────────────────────────────

class MediaPipeHandTracker:
    def __init__(self, model_path=None, max_hands=2):
        if model_path is None:
            model_path = os.path.join(
                os.path.dirname(os.path.abspath(__file__)),
                'hand_landmarker.task')

        base_options = python.BaseOptions(model_asset_path=model_path)
        options = vision.HandLandmarkerOptions(
            base_options=base_options,
            running_mode=vision.RunningMode.VIDEO,
            num_hands=max_hands,
            min_hand_detection_confidence=0.5,
            min_hand_presence_confidence=0.5,
            min_tracking_confidence=0.5
        )
        self.detector = vision.HandLandmarker.create_from_options(options)

    def process_frame(self, bgr_frame, timestamp_ms):
        """
        Process a single BGR frame.

        Returns (hand_data, detection_result) where hand_data is a list of dicts:
            {
                'side':             'Left' | 'Right',
                'world_landmarks':  list of 21 {x, y, z} dicts  (metres, hand-centre-relative),
                'screen_landmarks': list of 21 {x, y, z} dicts  (normalised image coords),
            }
        """
        rgb_frame = cv2.cvtColor(bgr_frame, cv2.COLOR_BGR2RGB)
        mp_image  = mp.Image(image_format=mp.ImageFormat.SRGB, data=rgb_frame)

        detection_result = self.detector.detect_for_video(mp_image, timestamp_ms)

        hand_data = []

        if detection_result.hand_landmarks:
            for hand_lm, hand_wlm, handedness in zip(
                    detection_result.hand_landmarks,
                    detection_result.hand_world_landmarks,
                    detection_result.handedness):

                # handedness is a list of Category; pick the top one
                side = handedness[0].category_name  # 'Left' or 'Right'

                world_coords = [{'x': lm.x, 'y': lm.y, 'z': lm.z}
                                for lm in hand_wlm]
                screen_coords = [{'x': lm.x, 'y': lm.y, 'z': lm.z}
                                 for lm in hand_lm]

                hand_data.append({
                    'side':             side,
                    'world_landmarks':  world_coords,
                    'screen_landmarks': screen_coords,
                    'raw_landmarks':    hand_lm,   # kept for interactive drawing
                })

        return hand_data, detection_result


# ──────────────────────────────────────────────────────────────────────────────
# IPC server mode  (invoked by Monado's mp_sync.c)
# ──────────────────────────────────────────────────────────────────────────────

def server_mode():
    tracker = MediaPipeHandTracker()

    # Signal readiness to the C parent process
    sys.stdout.write("READY\n")
    sys.stdout.flush()

    stdin_bin = sys.stdin.buffer   # raw bytes
    stdout    = sys.stdout

    while True:
        # ── read header ──────────────────────────────────────────────────────
        header_line = sys.stdin.buffer.readline().decode('utf-8')
        if not header_line:
            break  # EOF – parent closed the pipe
        header_line = header_line.strip()
        if header_line == "EXIT":
            break

        try:
            parts        = header_line.split()
            width        = int(parts[0])
            height       = int(parts[1])
            timestamp_ms = int(parts[2])
        except (ValueError, IndexError):
            stdout.write(json.dumps({"hands": [], "timestamp_ms": 0}) + "\n")
            stdout.flush()
            continue

        # ── read raw RGB bytes ────────────────────────────────────────────────
        n_bytes = width * height * 3
        raw     = stdin_bin.read(n_bytes)
        if len(raw) != n_bytes:
            break  # connection broken

        frame = np.frombuffer(raw, dtype=np.uint8).reshape((height, width, 3))
        # frame is RGB; process_frame expects BGR → convert back
        bgr_frame = cv2.cvtColor(frame, cv2.RGB2BGR)

        # ── run MediaPipe ──────────────────────────────────────────────────────
        try:
            hand_data, _ = tracker.process_frame(bgr_frame, timestamp_ms)
        except Exception as e:
            sys.stderr.write(f"[mp.py] error: {e}\n")
            hand_data = []

        # ── serialise result ──────────────────────────────────────────────────
        output = {
            "timestamp_ms": timestamp_ms,
            "hands": [
                {
                    "side":            h['side'],
                    "world_landmarks": h['world_landmarks'],
                }
                for h in hand_data
            ]
        }
        stdout.write(json.dumps(output) + "\n")
        stdout.flush()


# ──────────────────────────────────────────────────────────────────────────────
# Webcam Server mode (Standalone Capture)
# ──────────────────────────────────────────────────────────────────────────────

# We define a strict struct format for zero-latency IPC:
# struct MonadoHandData {
#     uint8_t left_active;
#     uint8_t right_active;
#     float left_world[21][3];
#     float right_world[21][3];
#     float left_screen[21][3];
#     float right_screen[21][3];
#     uint32_t width;
#     uint32_t height;
# };
# Format: 2 bytes (B), 252 floats (f), 2 ints (I) = 1018 bytes.
# We MUST use '<' to enforce standard little-endian packing with NO padding bytes.
SHM_FMT = '<BB' + ('f'*252) + 'II'
SHM_DATA_SIZE = struct.calcsize(SHM_FMT)

def apply_hand_to_arrays(hand, world_arr, screen_arr):
    for i, lm in enumerate(hand['world_landmarks']):
        world_arr[i*3 + 0] = lm['x']
        world_arr[i*3 + 1] = lm['y']
        world_arr[i*3 + 2] = lm['z']
    for i, lm in enumerate(hand['screen_landmarks']):
        screen_arr[i*3 + 0] = lm['x']
        screen_arr[i*3 + 1] = lm['y']
        screen_arr[i*3 + 2] = lm['z']

# ──────────────────────────────────────────────────────────────────────────────
# Webcam Server mode (Standalone Capture)
# ──────────────────────────────────────────────────────────────────────────────

def webcam_server_mode():
    """
    Opens the webcam and runs MediaPipe in a continuous loop.
    Writes zero-latency struct data into POSIX Shared Memory so C/C# can read instantly.
    """
    tracker = MediaPipeHandTracker()
    
    cap = None
    for i in range(5):
        cap = cv2.VideoCapture(i)
        if cap.isOpened():
            # ── Set 1080p resolution and MJPG format ──────────────────────
            cap.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter_fourcc('M', 'J', 'P', 'G'))
            cap.set(cv2.CAP_PROP_FRAME_WIDTH, 1920)
            cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 1080)
            cap.set(cv2.CAP_PROP_FPS, 30)
            cap.set(cv2.CAP_PROP_BUFFERSIZE, 1) # VITAL FOR LOW LATENCY
            sys.stderr.write(f"[mp.py] Webcam server opened on index {i}\n")
            break
            
    if not cap or not cap.isOpened():
        sys.stderr.write("[mp.py] Error: Could not open webcam for server.\n")
        sys.exit(1)

    # Prepare shared memory blocks
    frame_w = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    frame_h = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    frame_size = frame_w * frame_h * 4  # 4 bytes per pixel (RGBA)

    # Ensure previous orphaned chunks are cleaned up
    try:
        shm_data_old = shared_memory.SharedMemory(name="monado_mp_data")
        shm_data_old.unlink()
    except FileNotFoundError: pass

    try:
        shm_frame_old = shared_memory.SharedMemory(name="monado_mp_frame")
        shm_frame_old.unlink()
    except FileNotFoundError: pass

    shm_data = shared_memory.SharedMemory(create=True, size=SHM_DATA_SIZE, name="monado_mp_data")
    shm_frame = shared_memory.SharedMemory(create=True, size=frame_size, name="monado_mp_frame")

    # Map a numpy view directly over the shared memory buffer. 
    # This completely eliminates 240MB/s of Python memory allocations.
    shm_frame_ndarray = np.ndarray((frame_h, frame_w, 4), dtype=np.uint8, buffer=shm_frame.buf)

    # Signal readiness
    sys.stdout.write("READY\n")
    sys.stdout.flush()

    global_raw_frame = None
    global_raw_ret = False
    global_frame = None
    global_ret = False

    def raw_grab_loop():
        nonlocal global_raw_frame, global_raw_ret
        while cap.isOpened():
            ret, frame = cap.read()
            if ret:
                global_raw_frame = frame
                global_raw_ret = True

    raw_thread = threading.Thread(target=raw_grab_loop, daemon=True)
    raw_thread.start()

    def passthrough_loop():
        nonlocal global_frame, global_ret
        last_converted = None
        frames_grabbed = 0
        t0 = time.time()
        while cap.isOpened():
            if not global_raw_ret or global_raw_frame is last_converted:
                time.sleep(0.001)
                continue
                
            frame_to_convert = global_raw_frame
            last_converted = frame_to_convert
            
            # Zero-copy, zero-allocation RGBA conversion directly into C# StereoKit's memory!
            cv2.cvtColor(frame_to_convert, cv2.COLOR_BGR2RGBA, dst=shm_frame_ndarray)
            
            global_frame = frame_to_convert
            global_ret = True
            
            frames_grabbed += 1
            if frames_grabbed % 30 == 0:
                t1 = time.time()
                fps = 30.0 / (t1 - t0)
                sys.stderr.write(f"[mp.py] Webcam Grabber: {fps:.1f} FPS\n")
                t0 = t1

    pt_thread = threading.Thread(target=passthrough_loop, daemon=True)
    pt_thread.start()

    def capture_loop():
        last_processed = None
        frames_processed = 0
        t0 = time.time()
        while cap.isOpened():
            if not global_ret or global_frame is last_processed:
                time.sleep(0.002)
                continue

            frame_to_process = global_frame
            last_processed = frame_to_process
            
            ts_ms = int(time.time() * 1000)
            try:
                hand_data, _ = tracker.process_frame(frame_to_process, ts_ms)
                
                frames_processed += 1
                if frames_processed % 30 == 0:
                    t1 = time.time()
                    fps = 30.0 / (t1 - t0)
                    sys.stderr.write(f"[mp.py] MediaPipe Inference: {fps:.1f} FPS\n")
                    t0 = t1

                left_active = 0
                right_active = 0
                left_w = [0.0] * 63
                right_w = [0.0] * 63
                left_s = [0.0] * 63
                right_s = [0.0] * 63

                for h in hand_data:
                    if h['side'] == 'Left':
                        left_active = 1
                        apply_hand_to_arrays(h, left_w, left_s)
                    else:
                        right_active = 1
                        apply_hand_to_arrays(h, right_w, right_s)

                packed_data = struct.pack(SHM_FMT,
                    left_active, right_active,
                    *left_w, *right_w, *left_s, *right_s,
                    frame_w, frame_h
                )
                
                shm_data.buf[:SHM_DATA_SIZE] = packed_data

            except Exception as e:
                sys.stderr.write(f"[mp.py] capture error: {e}\n")

    thread = threading.Thread(target=capture_loop, daemon=True)
    thread.start()

    # Wait for completion or exit signal on stdin
    while True:
        line = sys.stdin.readline()
        if not line or line.strip() == "EXIT":
            break

    sys.stderr.write("[mp.py] Shutting down shared memory...\n")
    cap.release()
    shm_data.close()
    shm_data.unlink()
    shm_frame.close()
    shm_frame.unlink()


# ──────────────────────────────────────────────────────────────────────────────
# Interactive mode  (standalone usage / testing)
# ──────────────────────────────────────────────────────────────────────────────

def draw_landmarks(frame, hand_data):
    h, w, _ = frame.shape
    for hand in hand_data:
        for lm in hand['screen_landmarks']:
            cx, cy = int(lm['x'] * w), int(lm['y'] * h)
            cv2.circle(frame, (cx, cy), 4, (0, 255, 0), -1)


def interactive_mode():
    tracker = MediaPipeHandTracker()

    cap = None
    for i in range(5):
        cap = cv2.VideoCapture(i)
        if cap.isOpened():
            # ── Set 1080p resolution ──────────────────────────────
            cap.set(cv2.CAP_PROP_FRAME_WIDTH, 1920)
            cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 1080)
            cap.set(cv2.CAP_PROP_FPS, 30)
            print(f"[mp.py] Webcam opened on index {i} at 1080p", flush=True)
            break

    if not cap or not cap.isOpened():
        print("[mp.py] Error: Could not open webcam (tried indices 0-4).", flush=True)
        return

    print("[mp.py] MediaPipe Hand Tracking. Press 'q' to quit.", flush=True)

    while cap.isOpened():
        success, image = cap.read()
        if not success:
            break

        timestamp = int(time.time() * 1000)
        hand_data, _ = tracker.process_frame(image, timestamp)

        draw_landmarks(image, hand_data)

        for i, hand in enumerate(hand_data):
            wrist = hand['world_landmarks'][0]
            side  = hand['side']
            print(f"Hand {i} ({side}) Wrist (m): "
                  f"x={wrist['x']:.4f}, y={wrist['y']:.4f}, z={wrist['z']:.4f}",
                  flush=True)

        cv2.imshow('MediaPipe Hands - World Coordinates', image)
        if cv2.waitKey(5) & 0xFF == ord('q'):
            break

    cap.release()
    cv2.destroyAllWindows()


if __name__ == "__main__":
    if len(sys.argv) > 1:
        if sys.argv[1] == "--server":
            server_mode()
        elif sys.argv[1] == "--webcam-server":
            webcam_server_mode()
        else:
            interactive_mode()
    else:
        interactive_mode()