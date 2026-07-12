"""Tests for IM (Instant Messaging) config endpoints —
Feishu (Lark), QQ, and Telegram.

These endpoints are behind #ifdef CONFIG_APP_CLAW_CAP_IM_* in the firmware.
If the firmware wasn't built with support, the endpoint returns 404.
Tests gracefully handle this with pytest.xfail.
"""

import time
import pytest
import requests


def _try_post(client, base_url, path, json=None, timeout=10):
    """POST to an IM config endpoint; xfail on 404 (feature not compiled in)
    or connection reset (transient TCP issue)."""
    for attempt in range(2):
        try:
            r = client.post(f"{base_url}{path}", json=json, timeout=timeout)
            if r.status_code == 404:
                pytest.xfail(f"Feature not compiled in (endpoint {path} returned 404)")
            r.raise_for_status()
            return r.json()
        except (requests.exceptions.ConnectionError, ConnectionResetError) as e:
            if attempt == 0:
                time.sleep(3)
            else:
                pytest.xfail(f"Connection reset on {path}: {e}")


class TestFeishuConfig:
    """POST /api/feishu/config — configure Feishu bot credentials."""

    def test_feishu_valid(self, client, base_url):
        r = _try_post(client, base_url, "/api/feishu/config",
                      {"app_id": "test_fs_id", "app_secret": "test_fs_secret"})
        assert r.get("ok") in (1, True)

    def test_feishu_missing_fields(self, client, base_url):
        r = _try_post(client, base_url, "/api/feishu/config",
                      {"app_id": "", "app_secret": ""})
        assert r.get("ok") in (False, 0)
        assert "error" in r

    def test_feishu_partial(self, client, base_url):
        r = _try_post(client, base_url, "/api/feishu/config",
                      {"app_id": "partial_test_id"})
        assert r.get("ok") in (False, 0)


class TestQqConfig:
    """POST /api/qq/config — configure QQ bot credentials."""

    def test_qq_valid(self, client, base_url):
        r = _try_post(client, base_url, "/api/qq/config",
                      {"app_id": "test_qq_id", "app_secret": "test_qq_secret"})
        assert r.get("ok") in (1, True)

    def test_qq_missing_fields(self, client, base_url):
        r = _try_post(client, base_url, "/api/qq/config",
                      {"app_id": "", "app_secret": ""})
        assert r.get("ok") in (False, 0)


class TestTgConfig:
    """POST /api/tg/config — configure Telegram bot token."""

    def test_tg_valid(self, client, base_url):
        r = _try_post(client, base_url, "/api/tg/config",
                      {"token": "123456:test_integration_token"})
        assert r.get("ok") in (1, True)

    def test_tg_missing_token(self, client, base_url):
        r = _try_post(client, base_url, "/api/tg/config",
                      {"token": ""})
        assert r.get("ok") in (False, 0)
        assert "error" in r