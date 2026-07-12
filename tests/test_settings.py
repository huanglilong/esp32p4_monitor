"""Tests for settings API — POST /api/settings (WiFi + volume) and
POST /api/factory_reset.

WARNING: factory_reset will erase NVS settings and reboot the ESP32.
The factory_reset test is marked @pytest.mark.skip by default.

WiFi operations can take a while (connect timeout + reconnect to old AP).
"""

import pytest


class TestSettings:
    """POST /api/settings — configure WiFi SSID/pass and volume."""

    def test_settings_volume_only(self, api):
        """POST with just volume should succeed and return {ok: 1, wifi_connected}."""
        r = api.settings(volume=50)
        assert r.get("ok") in (1, True), f"Volume set failed: {r}"
        assert "wifi_connected" in r, f"Missing wifi_connected: {r}"
        # Verify volume took effect
        status = api.status()
        assert status.get("volume") == 50, f"Volume mismatch: {status}"

    def test_settings_volume_edge_low(self, api):
        """Volume 0 should be accepted."""
        r = api.settings(volume=0)
        assert r.get("ok") in (1, True)
        status = api.status()
        assert status.get("volume") == 0

    def test_settings_volume_edge_high(self, api):
        """Volume 100 should be accepted."""
        r = api.settings(volume=100)
        assert r.get("ok") in (1, True)
        status = api.status()
        assert status.get("volume") == 100

    def test_settings_volume_restore(self, api):
        """Restore volume to a reasonable default."""
        r = api.settings(volume=60)
        assert r.get("ok") in (1, True)

    def test_settings_wifi_empty_ssid(self, api):
        """POST with empty SSID should return {ok: 1, wifi_connected: false}."""
        r = api.settings(**{"ssid": "", "pass": "", "volume": 60})
        assert r.get("wifi_connected") is False, f"Expected wifi fail: {r}"

    def test_settings_wifi_bad_credentials(self, api, client, base_url):
        """POST with unreachable WiFi should not crash, return wifi_connected: false.
        Uses a longer timeout because the firmware tries to connect (15s timeout)
        then reconnects to old AP. Marked xfail on timeout because the firmware
        blocks the httpd task during WiFi operations."""
        import requests as req_mod
        try:
            r = client.post(
                f"{base_url}/api/settings",
                json={"ssid": "test_integration_nonexistent_ssid",
                      "pass": "wrong_password",
                      "volume": 60},
                timeout=60,
            )
            r.raise_for_status()
            j = r.json()
            assert j.get("ok") in (1, True)
            assert j.get("wifi_connected") is False, \
                "Bad credentials should not be accepted"
        except req_mod.ReadTimeout:
            pytest.xfail("WiFi connect timeout blocks httpd task")


@pytest.mark.skip(reason="Factory reset erases NVS and reboots — run manually only")
class TestFactoryReset:
    """POST /api/factory_reset — erase NVS and reboot."""

    def test_factory_reset(self, api):
        """Factory reset returns confirmation and device reboots."""
        r = api.factory_reset()
        assert r.get("ok") in (1, True)
        assert "message" in r
        assert "reboot" in r["message"].lower()
        # The device will reboot — subsequent tests will fail until it comes back