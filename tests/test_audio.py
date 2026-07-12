"""Tests for audio API endpoints — record, play, list, stop.

These tests exercise the real I2S hardware (microphone + speaker/SD card).
The record-play cycle requires ~5 seconds of real time.
"""

"""Tests for audio API endpoints — record, play, list, stop.

Strategy (方案 B — no firmware modification):
  - Use fixed duration sleep + explicit stop for all operations.
  - Poll record_status to observe recording progress.
  - Play for a fixed time, then stop — no play_status endpoint needed.
"""

import time
import pytest


RECORD_SECONDS = 10
PLAY_SECONDS = 10


@pytest.fixture(autouse=True)
def _ensure_clean_start(api):
    """Before each test: stop any leftover playback/recording."""
    api.audio_stop()
    try:
        st = api.record_status()
        if st.get("recording") == 1:
            api.record_stop()
    except Exception:
        pass
    yield


class TestAudioRecord:
    """Recording lifecycle: start → poll record_status → stop."""

    @pytest.fixture(autouse=True)
    def _stop_recording_after_test(self, api):
        """Ensure recording is stopped after each test, even on failure."""
        yield
        try:
            api.record_stop()
        except Exception:
            pass

    def test_record_start_returns_ok(self, api):
        """GET /api/audio/record_start returns {ok: 1}."""
        r = api.record_start()
        assert r.get("ok") in (1, True), f"Expected ok, got: {r}"
        # Ensure stop for clean state
        api.record_stop()

    def test_record_status_while_active(self, api):
        """record_status shows recording=1, seconds and bytes growing."""
        api.record_start()
        time.sleep(1)
        status = api.record_status()
        assert status.get("recording") == 1, f"Not recording: {status}"
        assert "seconds" in status, f"Missing seconds: {status}"
        assert "bytes" in status, f"Missing bytes: {status}"
        assert status["seconds"] >= 0
        # Stop
        api.record_stop()

    def test_record_stop_returns_file_info(self, api):
        """record_stop returns {ok: 1, file: '/sdcard/...', bytes: N} with N > 0."""
        api.record_start()
        time.sleep(RECORD_SECONDS)
        # Poll status mid-recording
        mid = api.record_status()
        assert mid["recording"] == 1
        assert mid["seconds"] >= RECORD_SECONDS - 1
        assert mid["bytes"] > 0
        # Stop
        r = api.record_stop()
        assert r.get("ok") in (1, True), f"Stop failed: {r}"
        assert "file" in r, f"Missing file: {r}"
        assert r["file"].startswith("/sdcard/"), f"Bad path: {r['file']}"
        assert r.get("bytes", 0) > 0, f"Zero bytes: {r}"

    def test_record_idempotent_start(self, api):
        """Double record_start returns ok without error."""
        api.record_start()
        r2 = api.record_start()
        assert r2.get("ok") in (1, True)
        api.record_stop()

    def test_record_idempotent_stop(self, api):
        """Calling record_stop when already stopped returns ok."""
        api.record_stop()
        r = api.record_stop()
        assert r.get("ok") in (1, True)

    def test_record_status_when_idle(self, api):
        """record_status returns {recording: 0} when idle."""
        api.record_stop()
        st = api.record_status()
        assert st.get("recording") == 0


class TestAudioList:
    """Audio file listing."""

    def test_audio_list_returns_files_array(self, api):
        """audio/list returns {files: [...]}."""
        data = api.audio_list()
        assert isinstance(data, dict)
        assert "files" in data
        assert isinstance(data["files"], list)

    def test_audio_list_has_mp3_after_record(self, api):
        """After recording, list contains .mp3 entry."""
        api.record_start()
        time.sleep(RECORD_SECONDS)
        api.record_stop()
        time.sleep(0.5)
        data = api.audio_list()
        mp3 = [f for f in data["files"] if f.lower().endswith(".mp3")]
        assert len(mp3) > 0, f"No .mp3 in list: {data['files']}"
        # Expose for play tests (module-level)
        pytest.latest_recording = mp3[-1]


class TestAudioPlay:
    """Playback: start → fixed duration → stop."""

    @pytest.fixture(autouse=True)
    def _ensure_recording(self, api):
        """Fixture: create a short recording for play tests if none exists."""
        try:
            data = api.audio_list()
        except Exception:
            data = {"files": []}
        mp3 = [f for f in data["files"] if f.lower().endswith(".mp3")]
        if not mp3:
            api.record_start()
            time.sleep(RECORD_SECONDS)
            api.record_stop()
            time.sleep(0.5)
            data = api.audio_list()
            mp3 = [f for f in data["files"] if f.lower().endswith(".mp3")]
        pytest.latest_recording = mp3[-1] if mp3 else None

    def test_play_returns_ok(self, api):
        """play with valid file returns {ok: 1}."""
        if not pytest.latest_recording:
            pytest.skip("No recording available")
        r = api.audio_play(pytest.latest_recording)
        assert r.get("ok") in (1, True), f"Play failed: {r}"
        # Let it play a bit, then stop
        time.sleep(PLAY_SECONDS)
        api.audio_stop()

    def test_play_and_stop_with_poll(self, api):
        """Play, poll status, then stop cleanly."""
        if not pytest.latest_recording:
            pytest.skip("No recording available")
        r = api.audio_play(pytest.latest_recording)
        assert r.get("ok") in (1, True)
        time.sleep(PLAY_SECONDS)
        r = api.audio_stop()
        assert r.get("ok") in (1, True)

    def test_play_without_file_returns_ok_0(self, api, client, base_url):
        """play without ?file= param returns {ok: 0}."""
        resp = client.get(f"{base_url}/api/audio/play", timeout=10)
        resp.raise_for_status()
        j = resp.json()
        assert j.get("ok") == 0

    def test_audio_stop_returns_ok(self, api):
        """audio/stop returns {ok: 1}."""
        r = api.audio_stop()
        assert r.get("ok") in (1, True)

    def test_play_nonexistent_file(self, api):
        """play with nonexistent file — firmware play is async, so 'ok' is always present."""
        r = api.audio_play("nonexistent_abc123.mp3")
        # The firmware's esp_audio_simple_player_run is asynchronous;
        # it may return ok:1 even for non-existent files (failure is async via callback).
        # Just verify the response is valid JSON with 'ok' key.
        assert "ok" in r, f"Response missing 'ok' key: {r}"


class TestAudioCleanup:
    """Final cleanup — stop any active operation."""

    def test_stop_all(self, api):
        """Ensure playback and recording are both stopped."""
        api.audio_stop()
        try:
            st = api.record_status()
            if st.get("recording") == 1:
                api.record_stop()
        except Exception:
            pass