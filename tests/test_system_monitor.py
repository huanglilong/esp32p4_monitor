"""Tests for system monitoring endpoints — /api/system_stats and /api/system_alerts."""


class TestSystemStats:
    """GET /api/system_stats — system performance snapshot."""

    def test_stats_returns_json(self, api):
        """Returns a JSON object with memory and cpu fields."""
        stats = api.system_stats()
        assert isinstance(stats, dict)
        assert "memory" in stats, f"Missing memory object: {stats}"

    def test_memory_fields(self, api):
        """Memory sub-object has all expected fields."""
        stats = api.system_stats()
        mem = stats["memory"]
        assert "free_internal_kb" in mem, f"Missing free_internal_kb: {mem}"
        assert "free_psram_kb" in mem, f"Missing free_psram_kb: {mem}"
        assert "min_free_internal_kb" in mem, f"Missing min_free_internal_kb: {mem}"
        assert "min_free_psram_kb" in mem, f"Missing min_free_psram_kb: {mem}"
        # Values should be non-negative
        for key in ("free_internal_kb", "free_psram_kb",
                     "min_free_internal_kb", "min_free_psram_kb"):
            assert mem[key] >= 0, f"{key} is negative: {mem[key]}"

    def test_cpu_fields(self, api):
        """CPU fields are present and within reasonable range."""
        stats = api.system_stats()
        assert "task_count" in stats, f"Missing task_count: {stats}"
        assert stats["task_count"] > 0, f"task_count should be > 0: {stats}"
        for key in ("cpu_pct", "core0_cpu_pct", "core1_cpu_pct"):
            assert key in stats, f"Missing {key}: {stats}"
            assert 0 <= stats[key] <= 100, \
                f"{key} out of range [0, 100]: {stats[key]}"


class TestSystemAlerts:
    """GET /api/system_alerts — current alert state."""

    def test_alerts_returns_json(self, api):
        """Returns a JSON object with cpu, mem_internal, mem_psram, thresholds."""
        alerts = api.system_alerts()
        assert isinstance(alerts, dict)
        for key in ("cpu", "mem_internal", "mem_psram"):
            assert key in alerts, f"Missing {key}: {alerts}"
            # Each alert object has at least an 'active' field
            assert "active" in alerts[key], \
                f"Missing active in {key}: {alerts[key]}"

    def test_alert_active_type(self, api):
        """Active alerts have additional fields (type, severity)."""
        alerts = api.system_alerts()
        for key in ("cpu", "mem_internal", "mem_psram"):
            entry = alerts[key]
            if entry.get("active"):
                assert "type" in entry, f"Active alert {key} missing type"
                assert "severity" in entry, f"Active alert {key} missing severity"
                assert entry["severity"] in ("warning", "critical", "info")

    def test_thresholds_present(self, api):
        """Thresholds sub-object has cpu_pct, mem_pct, cooldown_s."""
        alerts = api.system_alerts()
        assert "thresholds" in alerts, f"Missing thresholds: {alerts}"
        th = alerts["thresholds"]
        for key in ("cpu_pct", "mem_pct", "cooldown_s"):
            assert key in th, f"Missing threshold {key}: {th}"