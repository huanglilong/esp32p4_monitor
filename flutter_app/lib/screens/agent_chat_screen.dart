import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/material.dart';

import '../services/http_service.dart';
import '../providers/app_state.dart';
import '../main.dart' show AppStateScope;

/// AI Agent chat screen — send messages to the ESP32-P4 agent and display responses.
class AgentChatScreen extends StatefulWidget {
  const AgentChatScreen({super.key});
  @override
  State<AgentChatScreen> createState() => _AgentChatScreenState();
}

class _AgentChatScreenState extends State<AgentChatScreen> {
  final _inputController = TextEditingController();
  final _scrollController = ScrollController();
  final List<_ChatMessage> _messages = [];
  bool _sending = false;
  String? _error;

  // Agent message polling
  int _agentMsgIdx = 0;
  Timer? _pollTimer;

  // LLM config
  String _provider = 'deepseek';
  final _apiKeyController = TextEditingController();
  final _modelController = TextEditingController(text: 'deepseek-chat');
  final _baseUrlController = TextEditingController();
  bool _configLoaded = false;

  @override
  void initState() {
    super.initState();
    _loadLlmConfig();
  }

  @override
  void dispose() {
    _pollTimer?.cancel();
    _inputController.dispose();
    _scrollController.dispose();
    _apiKeyController.dispose();
    _modelController.dispose();
    _baseUrlController.dispose();
    super.dispose();
  }

  Future<void> _loadLlmConfig() async {
    final state = AppStateScope.of(context);
    final http = state.httpService;
    if (http == null) return;
    try {
      final resp = await http.getJson('/api/llm/config');
      if (resp != null) {
        if (resp['provider'] != null) {
          _provider = resp['provider'];
        }
        if (resp['has_api_key'] == true) {
          _apiKeyController.text = '(saved)';
        }
        if (resp['model'] != null) {
          _modelController.text = resp['model'];
        }
        if (resp['base_url'] != null && resp['base_url'].isNotEmpty) {
          _baseUrlController.text = resp['base_url'];
        }
      }
    } catch (_) {}
    if (mounted) setState(() => _configLoaded = true);
  }

  Future<void> _saveLlmConfig() async {
    final state = AppStateScope.of(context);
    final http = state.httpService;
    if (http == null) return;
    try {
      final body = <String, dynamic>{
        'provider': _provider,
        'model': _modelController.text,
      };
      if (_apiKeyController.text.isNotEmpty && _apiKeyController.text != '(saved)') {
        body['api_key'] = _apiKeyController.text;
      }
      if (_baseUrlController.text.isNotEmpty) {
        body['base_url'] = _baseUrlController.text;
      }
      await http.postJson('/api/llm/config', body);
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          const SnackBar(content: Text('LLM config saved ✅'), duration: Duration(seconds: 2)),
        );
      }
    } catch (e) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text('Save failed: $e'), backgroundColor: Colors.red),
        );
      }
    }
  }

  Future<void> _sendMessage() async {
    final text = _inputController.text.trim();
    if (text.isEmpty || _sending) return;
    _inputController.clear();

    setState(() {
      _messages.add(_ChatMessage(role: 'user', text: text));
      _sending = true;
      _error = null;
    });
    _scrollToBottom();

    final state = AppStateScope.of(context);
    final http = state.httpService;
    if (http == null) {
      setState(() {
        _error = 'Not connected';
        _sending = false;
      });
      return;
    }

    try {
      final resp = await http.postJson('/api/agent/chat', {'message': text});
      if (resp != null && resp['ok'] == true) {
        // Start polling for agent response
        _startPolling();
      } else if (resp != null && resp['error'] != null) {
        setState(() => _error = resp['error']);
      }
    } catch (e) {
      setState(() => _error = 'Network error: $e');
    } finally {
      setState(() => _sending = false);
      _scrollToBottom();
    }
  }

  void _startPolling() {
    _pollTimer?.cancel();
    _pollTimer = Timer.periodic(const Duration(seconds: 2), (_) => _pollAgentMessages());
    // Also poll immediately
    _pollAgentMessages();
  }

  Future<void> _pollAgentMessages() async {
    final state = AppStateScope.of(context);
    final http = state.httpService;
    if (http == null) return;

    try {
      final resp = await http.getJson('/api/agent/messages?since=$_agentMsgIdx');
      if (resp == null) return;

      final messages = resp['messages'] as List?;
      if (messages != null && messages.isNotEmpty) {
        setState(() {
          for (final m in messages) {
            String text = m['text'] ?? '';
            final linkUrl = m['link_url'];
            if (linkUrl != null && linkUrl.isNotEmpty) {
              final label = m['link_label'] ?? linkUrl;
              text += '\n[$label]($linkUrl)';
            }
            _messages.add(_ChatMessage(role: 'agent', text: text));
          }
          _agentMsgIdx = (resp['next_index'] as num?)?.toInt() ?? _agentMsgIdx;
          _sending = false;
        });
        _scrollToBottom();
        // Stop polling after receiving a response (will restart on next send)
        _pollTimer?.cancel();
      }
    } catch (_) {
      // Silently ignore polling errors
    }
  }

  void _scrollToBottom() {
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (_scrollController.hasClients) {
        _scrollController.animateTo(
          _scrollController.position.maxScrollExtent,
          duration: const Duration(milliseconds: 200),
          curve: Curves.easeOut,
        );
      }
    });
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('AI Agent'),
        actions: [
          IconButton(
            icon: const Icon(Icons.settings),
            onPressed: _showConfigDialog,
          ),
        ],
      ),
      body: Column(
        children: [
          // Chat log
          Expanded(
            child: _messages.isEmpty
                ? Center(
                    child: Text(
                      'Send a message to the AI agent',
                      style: TextStyle(color: Colors.grey[500]),
                    ),
                  )
                : ListView.builder(
                    controller: _scrollController,
                    padding: const EdgeInsets.all(12),
                    itemCount: _messages.length,
                    itemBuilder: (context, index) {
                      final msg = _messages[index];
                      return _buildMessageBubble(msg);
                    },
                  ),
          ),
          // Error
          if (_error != null)
            Padding(
              padding: const EdgeInsets.symmetric(horizontal: 12),
              child: Text(_error!, style: const TextStyle(color: Colors.red, fontSize: 12)),
            ),
          if (_sending)
            const Padding(
              padding: EdgeInsets.all(8),
              child: Row(
                children: [
                  SizedBox(width: 24, height: 24, child: CircularProgressIndicator(strokeWidth: 2)),
                  SizedBox(width: 8),
                  Text('Agent is thinking...', style: TextStyle(color: Colors.grey)),
                ],
              ),
            ),
          // Input
          SafeArea(
            child: Padding(
              padding: const EdgeInsets.all(8),
              child: Row(
                children: [
                  Expanded(
                    child: TextField(
                      controller: _inputController,
                      decoration: InputDecoration(
                        hintText: 'Ask the AI agent...',
                        border: OutlineInputBorder(borderRadius: BorderRadius.circular(24)),
                        contentPadding: const EdgeInsets.symmetric(horizontal: 16, vertical: 10),
                      ),
                      onSubmitted: (_) => _sendMessage(),
                    ),
                  ),
                  const SizedBox(width: 8),
                  IconButton.filled(
                    onPressed: _sending ? null : _sendMessage,
                    icon: const Icon(Icons.send),
                  ),
                ],
              ),
            ),
          ),
        ],
      ),
    );
  }

  Widget _buildMessageBubble(_ChatMessage msg) {
    final isUser = msg.role == 'user';
    final isTool = msg.role == 'tool';
    final bgColor = isUser
        ? Colors.blue[700]
        : isTool
            ? Colors.orange[800]
            : Colors.green[700];
    final align = isUser ? Alignment.centerRight : Alignment.centerLeft;

    return Align(
      alignment: align,
      child: Container(
        margin: const EdgeInsets.only(bottom: 8),
        padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 10),
        constraints: BoxConstraints(maxWidth: MediaQuery.of(context).size.width * 0.75),
        decoration: BoxDecoration(
          color: bgColor,
          borderRadius: BorderRadius.circular(16),
        ),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text(
              isUser ? 'You' : isTool ? 'Tool' : 'Agent',
              style: const TextStyle(fontSize: 11, fontWeight: FontWeight.bold, color: Colors.white70),
            ),
            const SizedBox(height: 2),
            SelectableText(msg.text, style: const TextStyle(color: Colors.white, fontSize: 14)),
          ],
        ),
      ),
    );
  }

  void _showConfigDialog() {
    showDialog(
      context: context,
      builder: (ctx) => StatefulBuilder(
        builder: (ctx, setDialogState) => AlertDialog(
          title: const Text('LLM Configuration'),
          content: SingleChildScrollView(
            child: Column(
              mainAxisSize: MainAxisSize.min,
              children: [
                DropdownButtonFormField<String>(
                  value: _provider,
                  decoration: const InputDecoration(labelText: 'Provider'),
                  items: const [
                    DropdownMenuItem(value: 'deepseek', child: Text('DeepSeek')),
                    DropdownMenuItem(value: 'openai', child: Text('OpenAI')),
                    DropdownMenuItem(value: 'anthropic', child: Text('Anthropic')),
                    DropdownMenuItem(value: 'qwen', child: Text('Qwen')),
                    DropdownMenuItem(value: 'custom', child: Text('Custom')),
                  ],
                  onChanged: (v) => setDialogState(() => _provider = v ?? 'deepseek'),
                ),
                TextField(
                  controller: _apiKeyController,
                  decoration: const InputDecoration(labelText: 'API Key'),
                  obscureText: true,
                ),
                TextField(
                  controller: _modelController,
                  decoration: const InputDecoration(labelText: 'Model'),
                ),
                TextField(
                  controller: _baseUrlController,
                  decoration: const InputDecoration(labelText: 'Base URL (optional)'),
                ),
              ],
            ),
          ),
          actions: [
            TextButton(
              onPressed: () => Navigator.pop(ctx),
              child: const Text('Cancel'),
            ),
            FilledButton(
              onPressed: () {
                Navigator.pop(ctx);
                _saveLlmConfig();
              },
              child: const Text('Save'),
            ),
          ],
        ),
      ),
    );
  }
}

class _ChatMessage {
  final String role; // 'user', 'agent', 'tool'
  final String text;
  _ChatMessage({required this.role, required this.text});
}
