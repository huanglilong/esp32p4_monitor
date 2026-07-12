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