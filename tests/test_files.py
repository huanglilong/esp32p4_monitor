"""Tests for file manager API endpoints — list, download, delete, delete_batch.

These tests require an SD card to be mounted on the ESP32.
"""

import pytest


class TestFilesList:
    """File listing via GET /api/files/list."""

    def test_files_list_root(self, api):
        """GET /api/files/list?dir=/ returns {ok: 1, files: [...], current, total_kb, free_kb}."""
        data = api.files_list("/")
        assert isinstance(data, dict)
        assert data.get("ok") == 1, f"Expected ok:1, got: {data}"
        assert "files" in data, f"Missing files: {data}"
        assert isinstance(data["files"], list)
        assert "current" in data, f"Missing current: {data}"
        assert "total_kb" in data, f"Missing total_kb: {data}"
        assert "free_kb" in data, f"Missing free_kb: {data}"

    def test_files_list_items_have_expected_keys(self, api):
        """Each file entry has name, is_dir, size, mtime."""
        data = api.files_list("/")
        for entry in data["files"]:
            assert "name" in entry, f"Missing name: {entry}"
            assert "is_dir" in entry, f"Missing is_dir: {entry}"
            assert isinstance(entry["is_dir"], bool)
            assert "size" in entry, f"Missing size: {entry}"
            assert "mtime" in entry, f"Missing mtime: {entry}"

    def test_files_list_invalid_path(self, api):
        """Querying a path outside /sdcard returns {ok: 0}."""
        data = api.files_list("/etc")
        assert data.get("ok") == 0, f"Expected ok:0, got: {data}"


class TestFilesDownload:
    """File download via GET /api/files/download."""

    def test_download_existing_file(self, api):
        """Download an existing .aac file and verify content."""
        listing = api.files_list("/")
        aac = [f for f in listing["files"] if f["name"].lower().endswith(".aac")
               and not f["is_dir"] and f["size"] > 0]
        if not aac:
            pytest.skip("No .aac files found on device to download")
        target = aac[0]
        filepath = f"/sdcard/{target['name']}"
        resp = api.files_download(filepath)
        assert resp.status_code == 200
        assert len(resp.content) > 0, "Downloaded file is empty"

    def test_download_nonexistent_file(self, api):
        """Downloading a non-existent file returns error text."""
        resp = api.files_download("/sdcard/nonexistent_file_xyz.aac")
        assert resp.status_code == 200
        text = resp.text
        assert "Cannot open file" in text or "not a file" in text.lower()


class TestFilesDelete:
    """File delete via POST /api/files/delete."""

    def test_delete_nonexistent_file(self, api):
        """Deleting a non-existent file returns {ok: 0, error: 'File not found'}."""
        r = api.files_delete("/sdcard/nonexistent_delete_test.aac")
        assert r.get("ok") == 0, f"Expected ok:0, got: {r}"
        assert "error" in r, f"Missing error: {r}"

    def test_delete_batch_nonexistent(self, api):
        """delete_batch with nonexistent paths returns {ok: 1, deleted: 0, failed: N}."""
        r = api.files_delete_batch(["/sdcard/nonexistent_a.aac",
                                    "/sdcard/nonexistent_b.aac"])
        assert r.get("ok") == 1, f"Expected ok:1, got: {r}"
        assert "deleted" in r, f"Missing deleted: {r}"
        assert "failed" in r, f"Missing failed: {r}"
        assert r["failed"] > 0, f"Expected failures: {r}"