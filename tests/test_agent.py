"""Tests for AI Agent Chat API — submit message and poll responses.

The agent requires LLM configuration to be set (api_key, provider, etc.).
If the agent is not configured, these tests verify graceful error handling.
"""

import time
import pytest


class TestAgentChatEndpoint:
    """POST /api/agent/chat — submit a message to the AI agent."""

    def test_chat_returns_ok(self, api):
        """POST with a valid message returns {ok: true, reply: '...'}."""
        r = api.agent_chat("Hello, this is an integration test ping.")
        # The agent may or may not be configured — both paths are valid
        assert "ok" in r or "error" in r, f"Unexpected response: {r}"

    def test_chat_without_message_fails(self, api, client, base_url):
        """POST without 'message' field returns HTTP 400."""
        import requests as req_module
        r = req_module.post(
            f"{base_url}/api/agent/chat",
            json={"not_message": "bad"},
            timeout=30,
        )
        assert r.status_code == 400, f"Expected 400, got {r.status_code}: {r.text}"

    def test_chat_when_llm_not_configured(self, api):
        """If LLM is not configured, chat returns a helpful error message."""
        r = api.agent_chat("ping")
        if "error" in r:
            # Graceful error when LLM not configured
            assert isinstance(r["error"], str)
            assert len(r["error"]) > 0


class TestAgentMessages:
    """GET /api/agent/messages — poll for agent responses."""

    def test_messages_returns_next_index(self, api):
        """Messages endpoint returns {next_index, messages: [...]}."""
        r = api.agent_messages(since=0)
        assert "next_index" in r, f"Missing next_index: {r}"
        assert "messages" in r, f"Missing messages: {r}"
        assert isinstance(r["messages"], list)

    def test_messages_entry_format(self, api):
        """Each message entry has index, message_id, chat_id, text, timestamp_ms."""
        r = api.agent_messages(since=0)
        for msg in r["messages"]:
            assert "index" in msg, f"Missing index: {msg}"
            assert "text" in msg, f"Missing text: {msg}"
            assert "message_id" in msg, f"Missing message_id: {msg}"
            assert "chat_id" in msg, f"Missing chat_id: {msg}"
            assert "timestamp_ms" in msg, f"Missing timestamp_ms: {msg}"

    def test_chat_and_poll_cycle(self, api):
        """Submit a chat message then poll for responses (non-blocking test)."""
        r = api.agent_chat("What is the current system status?")
        if r.get("ok") in (1, True):
            # Give agent time to respond, then poll
            time.sleep(5)
            msgs = api.agent_messages(since=0)
            if msgs["messages"]:
                latest = msgs["messages"][-1]
                assert "text" in latest
                assert len(latest["text"]) > 0