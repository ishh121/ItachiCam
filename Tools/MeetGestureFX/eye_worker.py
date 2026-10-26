"""MediaPipe Tasks/OpenCV sidecar for the finger-on-lips eye effect."""

from __future__ import annotations

import argparse
import collections
import socket
import struct
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence

import cv2
import mediapipe as mp
import numpy as np
from mediapipe.tasks import python
from mediapipe.tasks.python import vision


HOST = "127.0.0.1"
PORT = 38741
READY_MARKER = Path(r"C:\ProgramData\MeetGestureFX\meet-gesture-fx-eye-worker.ready")
REQUEST_MAGIC = 0x3145474D  # MGE1
RESPONSE_MAGIC = 0x3245474D  # MGE2
REQUEST_HEADER = struct.Struct("<5I")
RESPONSE_HEADER = struct.Struct("<5I")
EYE_RECORD = struct.Struct("<25f")
MODEL_DIR = Path(__file__).resolve().parent / "models"
FACE_MODEL = MODEL_DIR / "face_landmarker.task"
HAND_MODEL = MODEL_DIR / "hand_landmarker.task"
FACE_MODEL_URL = "https://storage.googleapis.com/mediapipe-models/face_landmarker/face_landmarker/float16/1/face_landmarker.task"
HAND_MODEL_URL = "https://storage.googleapis.com/mediapipe-models/hand_landmarker/hand_landmarker/float16/1/hand_landmarker.task"

LEFT_EYE = (33, 160, 158, 133, 153, 144)
RIGHT_EYE = (362, 385, 387, 263, 373, 380)
LEFT_IRIS = tuple(range(468, 473))
RIGHT_IRIS = tuple(range(473, 478))


@dataclass
class EyeGeometry:
    center: tuple[float, float]
    radius: tuple[float, float]
    iris_center: tuple[float, float]
    iris_radius: tuple[float, float]
    contour: list[tuple[float, float]]
    confidence: float


def download_models() -> None:
    MODEL_DIR.mkdir(parents=True, exist_ok=True)
    for path, url in ((FACE_MODEL, FACE_MODEL_URL), (HAND_MODEL, HAND_MODEL_URL)):
        if path.exists() and path.stat().st_size > 100_000:
            continue
        print(f"downloading {path.name}...", flush=True)
        with urllib.request.urlopen(url, timeout=120) as response, path.open("wb") as output:
            while chunk := response.read(1024 * 1024):
                output.write(chunk)


def recv_exact(connection: socket.socket, size: int) -> bytes:
    chunks: list[bytes] = []
    while size:
        chunk = connection.recv(size)
        if not chunk:
            raise ConnectionError("native camera disconnected")
        chunks.append(chunk)
        size -= len(chunk)
    return b"".join(chunks)


def send_all(connection: socket.socket, data: bytes) -> None:
    view = memoryview(data)
    while view:
        sent = connection.send(view)
        if sent <= 0:
            raise ConnectionError("native camera send failed")
        view = view[sent:]


def point(landmarks: Sequence, index: int) -> tuple[float, float]:
    landmark = landmarks[index]
    return float(landmark.x), float(landmark.y)


def mean_point(points: Iterable[tuple[float, float]]) -> tuple[float, float]:
    values = list(points)
    return (
        sum(value[0] for value in values) / len(values),
        sum(value[1] for value in values) / len(values),
    ) if values else (0.5, 0.5)


def geometry_from_landmarks(landmarks: Sequence, contour_indices: Sequence[int], iris_indices: Sequence[int]) -> EyeGeometry:
    contour = [point(landmarks, index) for index in contour_indices]
    iris_points = [point(landmarks, index) for index in iris_indices]
    min_x = min(value[0] for value in contour)
    max_x = max(value[0] for value in contour)
    min_y = min(value[1] for value in contour)
    max_y = max(value[1] for value in contour)
    iris_center = mean_point(iris_points)
    iris_min_x = min(value[0] for value in iris_points)
    iris_max_x = max(value[0] for value in iris_points)
    iris_min_y = min(value[1] for value in iris_points)
    iris_max_y = max(value[1] for value in iris_points)
    return EyeGeometry(
        center=((min_x + max_x) * 0.5, (min_y + max_y) * 0.5),
        radius=(max(0.008, (max_x - min_x) * 0.55), max(0.006, (max_y - min_y) * 0.70)),
        iris_center=iris_center,
        # The four outer iris landmarks span the iris diameter. Keep the
        # radius close to the measured iris instead of expanding it over the
        # sclera; the old 1.8 multiplier made the native renderer look like a
        # flat red paint patch over the whole eye.
        iris_radius=(max(0.004, (iris_max_x - iris_min_x) * 0.62), max(0.003, (iris_max_y - iris_min_y) * 0.62)),
        contour=contour,
        confidence=1.0,
    )


def point_to_segment_distance(point_xy: tuple[float, float], start: tuple[float, float], end: tuple[float, float]) -> float:
    point_array = np.asarray(point_xy, dtype=np.float32)
    start_array = np.asarray(start, dtype=np.float32)
    segment = np.asarray(end, dtype=np.float32) - start_array
    denominator = float(np.dot(segment, segment))
    if denominator <= 1e-6:
        return float(np.linalg.norm(point_array - start_array))
    t = float(np.clip(np.dot(point_array - start_array, segment) / denominator, 0.0, 1.0))
    return float(np.linalg.norm(point_array - (start_array + t * segment)))


def detect_finger_on_lips(face_landmarks: Sequence | None, hands: Sequence[Sequence] | None) -> bool:
    if not face_landmarks or not hands:
        return False
    mouth = mean_point((point(face_landmarks, 13), point(face_landmarks, 14)))
    face_left = point(face_landmarks, 234)
    face_right = point(face_landmarks, 454)
    face_top = point(face_landmarks, 10)
    face_bottom = point(face_landmarks, 152)
    face_width = max(0.08, abs(face_right[0] - face_left[0]))
    face_height = max(0.12, abs(face_bottom[1] - face_top[1]))

    for hand in hands:
        tip = point(hand, 8)
        pip = point(hand, 6)
        mcp = point(hand, 5)
        finger_vector = np.asarray(mcp, dtype=np.float32) - np.asarray(tip, dtype=np.float32)
        finger_length = float(np.linalg.norm(finger_vector))
        vertical = finger_vector[1] > face_height * 0.08 and finger_vector[1] > abs(float(finger_vector[0])) * 0.70
        near_mouth = float(np.linalg.norm(np.asarray(tip) - np.asarray(mouth))) < face_height * 0.34
        aligned = abs(tip[0] - mouth[0]) < face_width * 0.38
        touching_line = point_to_segment_distance(mouth, tip, mcp) < face_height * 0.18
        index_extended = finger_length > face_height * 0.10 and tip[1] <= mouth[1] + face_height * 0.10
        if vertical and near_mouth and index_extended and (aligned or touching_line):
            return True
    return False


def pack_eye(eye: EyeGeometry) -> bytes:
    contour = eye.contour[:8] + [(0.0, 0.0)] * max(0, 8 - len(eye.contour))
    values = [
        eye.center[0], eye.center[1], eye.radius[0], eye.radius[1],
        eye.iris_center[0], eye.iris_center[1], eye.iris_radius[0], eye.iris_radius[1],
    ]
    values.extend(component for xy in contour for component in xy)
    values.append(eye.confidence)
    return EYE_RECORD.pack(*values)


class LandmarkRunner:
    def __init__(self) -> None:
        if not FACE_MODEL.exists() or not HAND_MODEL.exists():
            raise FileNotFoundError(
                f"Missing models. Run this worker with --download-models first: {FACE_MODEL} and {HAND_MODEL}"
            )
        base = python.BaseOptions
        running_mode = vision.RunningMode.VIDEO
        self.face = vision.FaceLandmarker.create_from_options(vision.FaceLandmarkerOptions(
            base_options=base(model_asset_path=str(FACE_MODEL)),
            running_mode=running_mode,
            num_faces=1,
            min_face_detection_confidence=0.55,
            min_face_presence_confidence=0.55,
            min_tracking_confidence=0.55,
        ))
        self.hand = vision.HandLandmarker.create_from_options(vision.HandLandmarkerOptions(
            base_options=base(model_asset_path=str(HAND_MODEL)),
            running_mode=running_mode,
            num_hands=2,
            min_hand_detection_confidence=0.55,
            min_hand_presence_confidence=0.55,
            min_tracking_confidence=0.55,
        ))

    def close(self) -> None:
        self.face.close()
        self.hand.close()

    def process(self, frame: np.ndarray, timestamp_ms: int) -> tuple[bool, list[EyeGeometry]]:
        rgb = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
        image = mp.Image(image_format=mp.ImageFormat.SRGB, data=rgb)
        face_result = self.face.detect_for_video(image, timestamp_ms)
        hand_result = self.hand.detect_for_video(image, timestamp_ms)
        face_landmarks = face_result.face_landmarks[0] if face_result.face_landmarks else None
        hands = hand_result.hand_landmarks if hand_result.hand_landmarks else []
        shush = detect_finger_on_lips(face_landmarks, hands)
        eyes = []
        if face_landmarks:
            eyes = [
                geometry_from_landmarks(face_landmarks, LEFT_EYE, LEFT_IRIS),
                geometry_from_landmarks(face_landmarks, RIGHT_EYE, RIGHT_IRIS),
            ]
        return shush, eyes


def serve() -> None:
    runner = LandmarkRunner()
    history: collections.deque[bool] = collections.deque(maxlen=5)
    active = False
    frame_counter = 0
    READY_MARKER.parent.mkdir(parents=True, exist_ok=True)
    READY_MARKER.touch()
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
            server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            server.bind((HOST, PORT))
            server.listen(1)
            print(f"Meet Gesture FX eye worker listening on {HOST}:{PORT}", flush=True)
            while True:
                connection, address = server.accept()
                print(f"native camera connected from {address[0]}:{address[1]}", flush=True)
                with connection:
                    try:
                        while True:
                            magic, frame_id, width, height, channels = REQUEST_HEADER.unpack(
                                recv_exact(connection, REQUEST_HEADER.size)
                            )
                            if magic != REQUEST_MAGIC or channels != 3 or width <= 0 or height <= 0:
                                raise ValueError("invalid eye-worker request")
                            payload = recv_exact(connection, width * height * channels)
                            frame = np.frombuffer(payload, dtype=np.uint8).reshape((height, width, channels))
                            frame_counter += 1
                            raw_shush, eyes = runner.process(frame, frame_counter * 33)
                            history.append(raw_shush)
                            positives = sum(history)
                            active = positives >= (2 if active else 3)
                            response_payload = b"".join(pack_eye(eye) for eye in eyes[:2])
                            response = RESPONSE_HEADER.pack(
                                RESPONSE_MAGIC,
                                frame_id,
                                1 if active else 0,
                                min(2, len(eyes)),
                                len(response_payload),
                            )
                            send_all(connection, response + response_payload)
                    except (ConnectionError, OSError, ValueError) as error:
                        print(f"native camera disconnected: {error}", flush=True)
    finally:
        READY_MARKER.unlink(missing_ok=True)
        runner.close()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--download-models", action="store_true")
    args = parser.parse_args()
    if args.download_models:
        download_models()
        return
    serve()


if __name__ == "__main__":
    main()
