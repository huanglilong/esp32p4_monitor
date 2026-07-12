"""Tests for /api/status — system status endpoint."""

import time
import pytest


class TestStatus:
    """Verify the system status endpoint returns expected fields."""

    def test_status_returns_200(self, api, client, base_url):
        """GET /api/status returns HTTP 200 with valid JSON body.
        Retries once if the device is temporarily unreachable."""
        for attempt in range(2):
            try:
                status = api.status()
                assert isinstance(status, dict)
                return
            except Exception as e:
                if attempt == 0:
                    time.sleep(5)
                else:
                    pytest.fail(f"Device unreachable after retry: {e}")

    def test_status_has_wifi_fields(self, api):
        """Response includes ssid (string) and has_pass (bool)."""
        status = api.status()
        assert "ssid" in status, "Missing ssid field"
        assert isinstance(status["ssid"], str)
        assert "has_pass" in status, "Missing has_pass field"
        assert isinstance(status["has_pass"], bool)

    def test_status_has_volume(self, api):
        """Response includes a numeric volume field."""
        status = api.status()
        assert "volume" in status, "Missing volume field"
        assert isinstance(status["volume"], (int, float))

    def test_status_has_camera_fields(self, api):
        """Response includes camera stream fields."""
        status = api.status()
        assert "cam_stream" in status, "Missing cam_stream field"
        assert "cam_running" in status, "Missing cam_running field"
        assert isinstance(status["cam_running"], bool)
        assert "cam_recording" in status, "Missing cam_recording field"
        assert isinstance(status["cam_recording"], bool)

    def test_status_volume_in_range(self, api):
        """Volume should be within expected range (0-100)."""
        status = api.status()
        vol = status.get("volume", -1)
        assert 0 <= vol <= 100, f"Volume {vol} out of range [0, 100]"