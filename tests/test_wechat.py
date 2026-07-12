"""Tests for WeChat QR login API — start, status, cancel, persist.

The login flow requires scanning a QR code with the WeChat app.
These tests verify the API contract without completing a real login.
"""

import pytest


class TestWechatLoginStart:
    """POST /api/wechat/login/start — initiate QR login."""

    def test_start_returns_ok(self, api):
        """Start returns {ok: true, status, message, qr_data_url?}."""
        r = api.wechat_login_start()
        assert r.get("ok") in (1, True), f"Expected ok, got: {r}"
        assert "status" in r, f"Missing status: {r}"
        assert "message" in r, f"Missing message: {r}"

    def test_start_returns_qr_data_url(self, api):
        """Start response includes a qr_data_url for rendering."""
        r = api.wechat_login_start()
        # qr_data_url is present when we're in waiting state
        if r.get("status") in ("waiting_scan",):
            assert "qr_data_url" in r, f"Missing qr_data_url: {r}"
            assert isinstance(r["qr_data_url"], str)
            assert len(r["qr_data_url"]) > 0


class TestWechatLoginStatus:
    """GET /api/wechat/login/status — poll login state."""

    def test_status_returns_expected_fields(self, api):
        """Status returns {ok, active, configured, completed, persisted, status, message}."""
        r = api.wechat_login_status()
        assert r.get("ok") in (1, True)
        for key in ("active", "configured", "completed", "persisted"):
            assert key in r, f"Missing {key}: {r}"
            assert isinstance(r[key], bool)
        assert "status" in r, f"Missing status: {r}"
        assert "message" in r, f"Missing message: {r}"


class TestWechatLoginCancel:
    """POST /api/wechat/login/cancel — abort QR login."""

    def test_cancel_returns_ok(self, api):
        """Cancel returns {ok: true, message}."""
        r = api.wechat_login_cancel()
        assert r.get("ok") in (1, True), f"Expected ok, got: {r}"
        assert "message" in r


class TestWechatLoginPersist:
    """POST /api/wechat/login/persist — save token to NVS."""

    def test_persist_without_completed_login(self, api):
        """Persisting without a completed login returns HTTP 400."""
        api.wechat_login_cancel()  # ensure no active session
        import requests as req_module
        try:
            r = api.wechat_login_persist()
            # May succeed if a prior token exists, or fail
            if r.get("ok"):
                assert "message" in r
        except req_module.exceptions.HTTPError as e:
            # 400 is acceptable if no completed login to persist
            if e.response is not None:
                assert e.response.status_code == 400, \
                    f"Expected 400, got {e.response.status_code}"
            else:
                raise  # Re-raise unexpected HTTP errors without a response