"""Tests for camera stream API — POST /api/camera_stream, GET /api/get_camera_info,
/api/set_quality, /api/set_camera_config, /api/get_detection_info.

Note: Camera API endpoints are on port 80 (not 8080).
Enabling the camera stream requires WiFi to be connected.
"""

import pytest


class TestCameraStreamToggle:
    """POST /api/camera_stream — enable/disable camera streaming."""

    def test_camera_stream_status_not_running(self, api):
        """Initial status shows cam_running as a bool."""
        status = api.status()
        assert "cam_running" in status, f"Missing cam_running: {status}"
        assert isinstance(status["cam_running"], bool)
        assert "cam_recording" in status, f"Missing cam_recording: {status}"

    def test_camera_stream_disable(self, api):
        """POST with enable:0 returns {ok: 1, enabled: false, running: false/true}."""
        r = api.camera_stream(enable=False)
        assert r.get("ok") in (1, True), f"Disable failed: {r}"
        assert "enabled" in r, f"Missing enabled: {r}"
        assert "running" in r, f"Missing running: {r}"
        assert "recording" in r, f"Missing recording: {r}"

    def test_camera_stream_enable_toggle(self, api):
        """POST with enable:1 returns confirmation (may fail if no WiFi)."""
        r = api.camera_stream(enable=True)
        if r.get("ok") in (1, True):
            assert "running" in r
        else:
            # If WiFi is not connected, we get {ok: 0, error: 'WiFi not connected'}
            assert "error" in r, f"Expected error message: {r}"


class TestCameraInfo:
    """GET /api/get_camera_info on port 80 — camera sensor info."""

    def test_get_camera_info(self, client, camera_base_url):
        """Returns camera sensor information (width, height, jpeg_quality, etc.)."""
        r = client.get(f"{camera_base_url}/api/get_camera_info", timeout=10)
        r.raise_for_status()
        info = r.json()
        assert "width" in info, f"Missing width: {info}"
        assert "height" in info, f"Missing height: {info}"
        assert "jpeg_quality" in info, f"Missing jpeg_quality: {info}"
        assert "frame_rate" in info, f"Missing frame_rate: {info}"
        assert "pixel_format" in info, f"Missing pixel_format: {info}"


class TestCameraQuality:
    """GET /api/set_quality on port 80 — adjust JPEG quality."""

    def test_set_quality(self, client, camera_base_url):
        """Set JPEG quality to a valid value and verify."""
        r = client.get(
            f"{camera_base_url}/api/set_quality",
            params={"value": 50},
            timeout=10,
        )
        r.raise_for_status()
        j = r.json()
        assert j.get("ok") in (1, True), f"Set quality failed: {j}"


class TestSetCameraConfig:
    """POST /api/set_camera_config on port 80 — Flutter app quality control."""

    def test_set_camera_config(self, client, camera_base_url):
        """POST with jpeg_quality config."""
        r = client.post(
            f"{camera_base_url}/api/set_camera_config",
            json={"jpeg_quality": 60},
            timeout=10,
        )
        r.raise_for_status()
        j = r.json()
        assert j.get("status") == "ok", f"Set config failed: {j}"


class TestDetectionInfo:
    """GET /api/get_detection_info on port 80 — object detection state."""

    def test_get_detection_info(self, client, camera_base_url):
        """Returns detection info."""
        r = client.get(f"{camera_base_url}/api/get_detection_info", timeout=10)
        r.raise_for_status()
        info = r.json()
        assert "detection_enabled" in info, f"Missing detection_enabled: {info}"


class TestCameraCapture:
    """GET /api/camera/capture on port 8080 — take a picture to SD card."""

    def test_capture_requires_stream(self, client, base_url):
        """Capture without a running stream returns a JSON error (not a crash)."""
        r = client.get(f"{base_url}/api/camera/capture", timeout=10)
        r.raise_for_status()
        j = r.json()
        # Either the stream is running (ok=1) or we get a clean error
        if j.get("ok") not in (1, True):
            assert "error" in j, f"Expected error message: {j}"

    def test_take_picture(self, api, client, base_url):
        """Enable stream, capture a picture, verify file exists on SD card."""
        # Enable the camera stream (requires WiFi)
        r = api.camera_stream(enable=True)
        if r.get("ok") not in (1, True):
            pytest.skip(f"Camera stream could not start: {r}")

        # Wait for the capture task to produce at least one frame
        import time
        for _ in range(20):
            info = api.status()
            if info.get("cam_running"):
                break
            time.sleep(0.5)

        # Take a picture
        r = client.get(f"{base_url}/api/camera/capture", timeout=15)
        r.raise_for_status()
        j = r.json()
        assert j.get("ok") in (1, True), f"Capture failed: {j}"
        assert "file" in j, f"Missing file field: {j}"
        assert j["file"].endswith(".jpg"), f"File should be .jpg: {j['file']}"
        assert j.get("bytes", 0) > 0, f"Picture should have data: {j}"

        # Verify the file exists via the file manager API
        listing = api.files_list(dir="/")
        names = [f["name"] for f in listing.get("files", [])]
        assert j["file"] in names, f"Captured file {j['file']} not in SD root: {names[:10]}"

        # Cleanup: delete the captured picture
        d = api.files_delete(path="/" + j["file"])
        assert d.get("ok") in (1, True), f"Cleanup delete failed: {d}"


class TestCameraRotation:
    """GET /api/set_rotation on port 80 — camera image rotation."""

    def test_camera_info_has_rotation(self, client, camera_base_url):
        """Camera info includes rotation field."""
        r = client.get(f"{camera_base_url}/api/get_camera_info", timeout=10)
        r.raise_for_status()
        info = r.json()
        assert "rotation" in info, f"Missing rotation: {info}"
        assert info["rotation"] in (0, 90, 180, 270), f"Invalid rotation: {info['rotation']}"

    def test_set_rotation_90(self, client, camera_base_url):
        """Set rotation to 90 degrees."""
        r = client.get(
            f"{camera_base_url}/api/set_rotation",
            params={"value": 90},
            timeout=10,
        )
        r.raise_for_status()
        j = r.json()
        assert j.get("ok") in (1, True), f"Set rotation failed: {j}"
        assert j.get("rotation") == 90, f"Rotation not 90: {j}"

    def test_set_rotation_180(self, client, camera_base_url):
        """Set rotation to 180 degrees."""
        r = client.get(
            f"{camera_base_url}/api/set_rotation",
            params={"value": 180},
            timeout=10,
        )
        r.raise_for_status()
        j = r.json()
        assert j.get("ok") in (1, True), f"Set rotation failed: {j}"
        assert j.get("rotation") == 180, f"Rotation not 180: {j}"

    def test_set_rotation_270(self, client, camera_base_url):
        """Set rotation to 270 degrees."""
        r = client.get(
            f"{camera_base_url}/api/set_rotation",
            params={"value": 270},
            timeout=10,
        )
        r.raise_for_status()
        j = r.json()
        assert j.get("ok") in (1, True), f"Set rotation failed: {j}"
        assert j.get("rotation") == 270, f"Rotation not 270: {j}"

    def test_set_rotation_restore_0(self, client, camera_base_url):
        """Restore rotation to 0 degrees."""
        r = client.get(
            f"{camera_base_url}/api/set_rotation",
            params={"value": 0},
            timeout=10,
        )
        r.raise_for_status()
        j = r.json()
        assert j.get("ok") in (1, True), f"Set rotation failed: {j}"
        assert j.get("rotation") == 0, f"Rotation not 0: {j}"

    def test_rotation_in_status(self, api):
        """Rotation appears in /api/status on port 8080."""
        status = api.status()
        assert "cam_rotation" in status, f"Missing cam_rotation: {status}"
        assert status["cam_rotation"] in (0, 90, 180, 270), f"Invalid cam_rotation: {status}"