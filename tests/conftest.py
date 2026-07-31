"""Shared fixtures and helpers for ESP32 web config integration tests."""

import os
import time
import pytest
import requests
from urllib.parse import urlparse, urlunparse


def pytest_addoption(parser):
    parser.addoption(
        "--base-url",
        default=None,
        help="ESP32 base URL (e.g., http://10.0.0.42:8080). "
             "Overrides ESP_BASE_URL env var and default.",
    )


@pytest.fixture(scope="session")
def base_url(request) -> str:
    """Return the ESP32 base URL.

    Precedence (highest first):
      1. --base-url CLI argument
      2. ESP_BASE_URL environment variable
      3. Built-in default
    """
    cli = request.config.getoption("--base-url")
    if cli:
        return cli
    env = os.environ.get("ESP_BASE_URL")
    if env:
        return env
    return "http://esp-web.local:8080"


@pytest.fixture(scope="session")
def client() -> requests.Session:
    """A requests.Session pre-configured with JSON headers."""
    s = requests.Session()
    s.headers.update({"Accept": "application/json"})
    return s


@pytest.fixture(scope="session")
def camera_base_url(base_url: str) -> str:
    """Camera stream API is on port 80, not 8080."""
    parsed = urlparse(base_url)
    # Replace port 8080 with 80, or add :80 if no port
    port = 80 if (parsed.port == 8080 or parsed.port is None) else parsed.port
    netloc = f"{parsed.hostname}:{port}"
    return urlunparse((parsed.scheme, netloc, parsed.path, parsed.params,
                       parsed.query, parsed.fragment))


@pytest.fixture(scope="session", autouse=True)
def device_info(client: requests.Session, base_url: str):
    """Fetch and print target device info at the start of the test run."""
    print(f"\n{'='*60}")
    print(f"Target device: {base_url}")
    try:
        r = client.get(f"{base_url}/api/status", timeout=10)
        r.raise_for_status()
        info = r.json()
        ssid = info.get("ssid", "(not set)")
        vol  = info.get("volume", "?")
        cam  = "✓" if info.get("cam_running") else "✗"
        rec  = "✓" if info.get("cam_recording") else "✗"
        print(f"  WiFi SSID  : {ssid}")
        print(f"  Volume     : {vol}")
        print(f"  Camera     : stream={cam}  recording={rec}")
        # Also try system_stats for more info
        try:
            sr = client.get(f"{base_url}/api/system_stats", timeout=5)
            sr.raise_for_status()
            sys_info = sr.json()
            mem = sys_info.get("memory", {})
            print(f"  Memory     : internal={mem.get('free_internal_kb', '?')}KB  "
                  f"psram={mem.get('free_psram_kb', '?')}KB")
            print(f"  CPU        : {sys_info.get('cpu_pct', '?'):.1f}%  "
                  f"tasks={sys_info.get('task_count', '?')}")
        except Exception:
            print("  System stats: unavailable")
    except requests.RequestException as e:
        print(f"  ⚠️  Failed to fetch device info: {e}")
    print(f"{'='*60}\n")
    yield


@pytest.fixture
def api(client: requests.Session, base_url: str):
    """Namespace-like fixture with helper methods for common API calls.
    
    Usage:
        def test_something(api):
            resp = api.status()
            assert resp["ssid"]
    """
    class _API:
        @staticmethod
        def status():
            r = client.get(f"{base_url}/api/status", timeout=10)
            r.raise_for_status()
            return r.json()

        @staticmethod
        def settings(**kw):
            r = client.post(f"{base_url}/api/settings", json=kw, timeout=15)
            r.raise_for_status()
            return r.json()

        @staticmethod
        def camera_stream(enable: bool):
            r = client.post(
                f"{base_url}/api/camera_stream",
                json={"enable": 1 if enable else 0},
                timeout=10,
            )
            r.raise_for_status()
            return r.json()

        @staticmethod
        def factory_reset():
            r = client.post(f"{base_url}/api/factory_reset", timeout=5)
            r.raise_for_status()
            return r.json()

        @staticmethod
        def record_start(timeout=30):
            """Start recording, retrying if previous recording cleanup is in progress."""
            deadline = time.time() + timeout
            while True:
                r = client.get(f"{base_url}/api/audio/record_start", timeout=10)
                r.raise_for_status()
                j = r.json()
                if j.get("ok") in (1, True):
                    return j
                retry = j.get("retry_after", 0)
                if retry > 0 and time.time() + retry < deadline:
                    time.sleep(min(retry, 2))
                    continue
                return j  # return error response

        @staticmethod
        def record_stop():
            r = client.get(f"{base_url}/api/audio/record_stop", timeout=10)
            r.raise_for_status()
            return r.json()

        @staticmethod
        def record_status():
            r = client.get(f"{base_url}/api/audio/record_status", timeout=5)
            r.raise_for_status()
            return r.json()

        @staticmethod
        def audio_list():
            r = client.get(f"{base_url}/api/audio/list", timeout=10)
            r.raise_for_status()
            return r.json()

        @staticmethod
        def audio_play(file: str):
            r = client.get(
                f"{base_url}/api/audio/play",
                params={"file": file},
                timeout=30,
            )
            r.raise_for_status()
            return r.json()

        @staticmethod
        def audio_stop():
            r = client.get(f"{base_url}/api/audio/stop", timeout=5)
            r.raise_for_status()
            return r.json()

        @staticmethod
        def files_list(dir: str = "/"):
            r = client.get(
                f"{base_url}/api/files/list",
                params={"dir": dir},
                timeout=10,
            )
            r.raise_for_status()
            return r.json()

        @staticmethod
        def files_download(path: str):
            r = client.get(
                f"{base_url}/api/files/download",
                params={"path": path},
                timeout=30,
            )
            return r  # return raw response (may be binary)

        @staticmethod
        def files_delete(path: str):
            r = client.post(
                f"{base_url}/api/files/delete",
                json={"path": path},
                timeout=10,
            )
            r.raise_for_status()
            return r.json()

        @staticmethod
        def files_delete_batch(paths: list):
            r = client.post(
                f"{base_url}/api/files/delete_batch",
                json={"paths": paths},
                timeout=10,
            )
            r.raise_for_status()
            return r.json()

        @staticmethod
        def ulog_status():
            r = client.get(f"{base_url}/api/ulog/status", timeout=5)
            r.raise_for_status()
            return r.json()

        @staticmethod
        def ulog_start():
            r = client.post(f"{base_url}/api/ulog/start", timeout=10)
            r.raise_for_status()
            return r.json()

        @staticmethod
        def ulog_stop():
            r = client.post(f"{base_url}/api/ulog/stop", timeout=5)
            r.raise_for_status()
            return r.json()

        @staticmethod
        def system_stats():
            r = client.get(f"{base_url}/api/system_stats", timeout=5)
            r.raise_for_status()
            return r.json()

        @staticmethod
        def system_alerts():
            r = client.get(f"{base_url}/api/system_alerts", timeout=5)
            r.raise_for_status()
            return r.json()

        @staticmethod
        def llm_config_get():
            r = client.get(f"{base_url}/api/llm/config", timeout=5)
            r.raise_for_status()
            return r.json()

        @staticmethod
        def llm_config_set(**kw):
            r = client.post(
                f"{base_url}/api/llm/config",
                json=kw,
                timeout=10,
            )
            r.raise_for_status()
            return r.json()

        @staticmethod
        def wechat_login_start():
            r = client.post(f"{base_url}/api/wechat/login/start", timeout=10)
            r.raise_for_status()
            return r.json()

        @staticmethod
        def wechat_login_status():
            r = client.get(f"{base_url}/api/wechat/login/status", timeout=5)
            r.raise_for_status()
            return r.json()

        @staticmethod
        def wechat_login_cancel():
            r = client.post(f"{base_url}/api/wechat/login/cancel", timeout=5)
            r.raise_for_status()
            return r.json()

        @staticmethod
        def wechat_login_persist():
            r = client.post(f"{base_url}/api/wechat/login/persist", timeout=5)
            r.raise_for_status()
            return r.json()

        @staticmethod
        def agent_chat(message: str):
            r = client.post(
                f"{base_url}/api/agent/chat",
                json={"message": message},
                timeout=60,
            )
            r.raise_for_status()
            return r.json()

        @staticmethod
        def agent_messages(since: int = 0):
            r = client.get(
                f"{base_url}/api/agent/messages",
                params={"since": since},
                timeout=10,
            )
            r.raise_for_status()
            return r.json()

        @staticmethod
        def feishu_config(**kw):
            r = client.post(
                f"{base_url}/api/feishu/config",
                json=kw,
                timeout=10,
            )
            r.raise_for_status()
            return r.json()

        @staticmethod
        def qq_config(**kw):
            r = client.post(
                f"{base_url}/api/qq/config",
                json=kw,
                timeout=10,
            )
            r.raise_for_status()
            return r.json()

        @staticmethod
        def tg_config(**kw):
            r = client.post(
                f"{base_url}/api/tg/config",
                json=kw,
                timeout=10,
            )
            r.raise_for_status()
            return r.json()

        @staticmethod
        def get_camera_info():
            r = client.get(f"{base_url}/api/get_camera_info", timeout=5)
            r.raise_for_status()
            return r.json()

        @staticmethod
        def set_quality(value: int):
            r = client.get(
                f"{base_url}/api/set_quality",
                params={"value": value},
                timeout=5,
            )
            r.raise_for_status()
            return r.json()

        @staticmethod
        def set_camera_config(**kw):
            r = client.post(
                f"{base_url}/api/set_camera_config",
                json=kw,
                timeout=10,
            )
            r.raise_for_status()
            return r.json()

        @staticmethod
        def get_detection_info():
            r = client.get(f"{base_url}/api/get_detection_info", timeout=5)
            r.raise_for_status()
            return r.json()

    return _API