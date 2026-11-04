"""
Squat counter + form check demo using MediaPipe Pose and your webcam.

Setup (once):
    pip install mediapipe==0.10.14 opencv-python

Run:
    python squat_counter.py
Press Q to quit.

Stand side-on to the camera so your hip, knee and ankle are visible.
"""

import json
import math
import time

import cv2
import mediapipe as mp

PARTICIPANT_ID = "03"
EXERCISE = "SQUATS"

DOWN_ANGLE = 100    # knee angle below this = bottom of squat
UP_ANGLE = 160      # knee angle above this = standing
GOOD_DEPTH = 90     # must reach at least this angle for "good" form

mp_pose = mp.solutions.pose
mp_draw = mp.solutions.drawing_utils


def angle(a, b, c):
    """Angle at point b (degrees) formed by points a-b-c."""
    ang = math.degrees(
        math.atan2(c.y - b.y, c.x - b.x) - math.atan2(a.y - b.y, a.x - b.x)
    )
    ang = abs(ang)
    return 360 - ang if ang > 180 else ang


def main():
    cap = cv2.VideoCapture(0)
    reps, flags = 0, 0
    stage = "up"
    lowest = 180
    last_msg = ""

    with mp_pose.Pose(min_detection_confidence=0.5,
                      min_tracking_confidence=0.5) as pose:
        while cap.isOpened():
            ok, frame = cap.read()
            if not ok:
                break

            frame = cv2.flip(frame, 1)
            results = pose.process(cv2.cvtColor(frame, cv2.COLOR_BGR2RGB))

            if results.pose_landmarks:
                lm = results.pose_landmarks.landmark
                hip = lm[mp_pose.PoseLandmark.LEFT_HIP]
                knee = lm[mp_pose.PoseLandmark.LEFT_KNEE]
                ankle = lm[mp_pose.PoseLandmark.LEFT_ANKLE]
                knee_angle = angle(hip, knee, ankle)

                if stage == "down":
                    lowest = min(lowest, knee_angle)

                if knee_angle < DOWN_ANGLE and stage == "up":
                    stage = "down"
                    lowest = knee_angle
                elif knee_angle > UP_ANGLE and stage == "down":
                    stage = "up"
                    reps += 1
                    if lowest > GOOD_DEPTH:
                        flags += 1
                        last_msg = "! Go deeper"
                    else:
                        last_msg = "Good rep"
                    # This is what would be sent to the web app (no video)
                    print(json.dumps({
                        "id": PARTICIPANT_ID,
                        "exercise": EXERCISE,
                        "reps": reps,
                        "flags": flags,
                        "last_rep": last_msg,
                        "time": time.strftime("%H:%M:%S"),
                    }))

                mp_draw.draw_landmarks(frame, results.pose_landmarks,
                                       mp_pose.POSE_CONNECTIONS)
                cv2.putText(frame, f"Knee: {int(knee_angle)}",
                            (20, 130), cv2.FONT_HERSHEY_SIMPLEX, 0.8,
                            (255, 255, 255), 2)

            cv2.rectangle(frame, (0, 0), (330, 100), (40, 40, 40), -1)
            cv2.putText(frame, f"ID {PARTICIPANT_ID}  {EXERCISE}", (10, 30),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.8, (255, 255, 255), 2)
            cv2.putText(frame, f"Reps: {reps}   Flags: {flags}", (10, 60),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.8, (255, 255, 255), 2)
            color = (0, 0, 255) if last_msg.startswith("!") else (0, 200, 0)
            cv2.putText(frame, last_msg, (10, 90),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.8, color, 2)

            cv2.imshow("Squat Form Check", frame)
            if cv2.waitKey(1) & 0xFF == ord("q"):
                break

    cap.release()
    cv2.destroyAllWindows()


if __name__ == "__main__":
    main()
