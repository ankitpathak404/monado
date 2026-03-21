import cv2
import sys
import os

def test_camera():
    print("Testing camera acquisition...")
    # Try libcamerasrc
    pipeline = "libcamerasrc ! video/x-raw, width=640, height=480, framerate=30/1 ! videoconvert ! appsink"
    cap = cv2.VideoCapture(pipeline, cv2.CAP_GSTREAMER)
    
    if not cap.isOpened():
        print("libcamerasrc failed, trying V4L2 index 0...")
        cap = cv2.VideoCapture(0)
        
    if not cap.isOpened():
        print("Error: Could not open any camera.")
        return

    print("Camera opened. Capturing 10 frames...")
    for i in range(10):
        ret, frame = cap.read()
        if ret:
            print(f"Frame {i} captured: {frame.shape}")
        else:
            print(f"Frame {i} failed to capture.")
            
    if ret:
        cv2.imwrite("/home/ankit/cam_test.jpg", frame)
        print("Saved /home/ankit/cam_test.jpg")
    
    cap.release()
    print("Test complete.")

if __name__ == "__main__":
    test_camera()
