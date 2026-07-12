"""Tests for LLM configuration API — GET and POST /api/llm/config.

Note: These tests write to NVS (non-volatile storage). The test values are
unique and harmless, but they will persist on the device after the test run.
"""


class TestLlmConfigGet:
    """GET /api/llm/config — read current LLM configuration."""

    def test_get_returns_json(self, api):
        """Returns a JSON object."""
        config = api.llm_config_get()
        assert isinstance(config, dict)

    def test_get_optional_fields(self, api):
        """Response may contain provider, has_api_key, model, base_url."""
        config = api.llm_config_get()
        for key in ("provider", "has_api_key", "model", "base_url"):
            if key in config:
                if key == "has_api_key":
                    assert isinstance(config[key], bool)
                else:
                    assert isinstance(config[key], (str, type(None)))


class TestLlmConfigSet:
    """POST /api/llm/config — save/validate LLM configuration."""

    TEST_PROVIDER = "test_integration_openai"
    TEST_MODEL = "test_integration_gpt4"
    TEST_KEY = "sk-test-integration-key"
    TEST_URL = "https://test-integration.example.com"

    def test_set_valid_config(self, api):
        """POST with all fields → {ok: true}. Read-back verification."""
        r = api.llm_config_set(
            provider=self.TEST_PROVIDER,
            api_key=self.TEST_KEY,
            model=self.TEST_MODEL,
            base_url=self.TEST_URL,
        )
        assert r.get("ok") in (1, True), f"Save failed: {r}"
        # Read back
        config = api.llm_config_get()
        assert config.get("provider") == self.TEST_PROVIDER
        assert config.get("model") == self.TEST_MODEL
        assert config.get("base_url") == self.TEST_URL
        # API key must NOT be exposed in plaintext
        assert config.get("has_api_key") is True, "has_api_key should be True"
        assert "api_key" not in config, "API key MUST NOT be exposed in GET"

    def test_set_empty_api_key_fails(self, api):
        """POST with empty api_key → {ok: false, error: '...'}."""
        r = api.llm_config_set(
            provider=self.TEST_PROVIDER,
            api_key="",
            model=self.TEST_MODEL,
            base_url=self.TEST_URL,
        )
        assert r.get("ok") in (False, 0), f"Expected false, got: {r}"
        assert "error" in r, f"Missing error: {r}"

    def test_set_partial_config_fails(self, api):
        """POST with only provider → {ok: false}."""
        r = api.llm_config_set(provider=self.TEST_PROVIDER)
        assert r.get("ok") in (False, 0), f"Expected false, got: {r}"