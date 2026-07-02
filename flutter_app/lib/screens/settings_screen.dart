import 'dart:async';

import 'package:flutter/material.dart';

import '../main.dart';
import '../providers/app_state.dart';
import '../services/http_service.dart';

/// Settings + Audio: device config (WiFi/volume/camera stream) + audio recording/playback.
class SettingsScreen extends StatefulWidget {
  const SettingsScreen({super.key});
  @override
  State<SettingsScreen> createState() => _SettingsScreenState();
}

class _SettingsScreenState extends State<SettingsScreen> {
  AppState? _state;
  Esp32HttpService? _http;
  bool _initialized = false;

  bool _wifiEnabled = false;
  final _ssidController = TextEditingController();
  final _passwordController = TextEditingController();
  double _volume = 60;
  bool _camStreamEnabled = false;
  bool _settingsLoading = true;

  bool _isRecording = false;
  int _recordingSeconds = 0;
  int _recordingBytes = 0;
  Timer? _statusTimer;
  Timer? _timerCounter;
  List<String> _files = [];
  bool _loadingFiles = true;
  String? _playingFile;

  @override
  void initState() => super.initState();

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    _state = AppStateScope.of(context);
    _http = _state!.httpService;
    if (!_initialized) {
      _initialized = true;
      WidgetsBinding.instance.addPostFrameCallback((_) {
        if (mounted) { _loadSettings(); _loadFiles(); }
      });
    }
  }

  @override
  void dispose() {
    _statusTimer?.cancel(); _timerCounter?.cancel();
    _ssidController.dispose(); _passwordController.dispose();
    super.dispose();
  }

  Future<void> _loadSettings() async {
    try {
      await _state!.fetchSettings();
      if (mounted) setState(() {
        _wifiEnabled = _state!.wifiEnabled;
        _ssidController.text = _state!.ssid;
        _passwordController.text = _state!.password;
        _volume = _state!.volume.toDouble();
        _camStreamEnabled = _state!.camStreamEnabled;
        _settingsLoading = false;
      });
    } catch (_) { if (mounted) setState(() => _settingsLoading = false); }
  }

  void _saveSettings() => _state!.updateSettings(
    wifiEnabled: _wifiEnabled, ssid: _ssidController.text,
    password: _passwordController.text, volume: _volume.round(),
  );

  void _confirmFactoryReset() {
    showDialog(context: context, builder: (ctx) => AlertDialog(
      title: const Text('Factory Reset'),
      content: const Text('Erase all settings and reboot?'),
      actions: [
        TextButton(onPressed: () => Navigator.pop(ctx), child: const Text('Cancel')),
        TextButton(onPressed: () { Navigator.pop(ctx); _state!.factoryReset(); },
          child: const Text('Reset', style: TextStyle(color: Colors.redAccent))),
      ],
    ));
  }

  Future<void> _loadFiles() async {
    setState(() => _loadingFiles = true);
    try {
      final r = await _http!.audioList();
      if (mounted) setState(() {
        _files = r['files'] != null ? List<String>.from(r['files']) : [];
        _loadingFiles = false;
      });
    } catch (_) { if (mounted) setState(() { _files = []; _loadingFiles = false; }); }
  }

  Future<void> _startRecording() async {
    try {
      final r = await _http!.audioRecordStart();
      if (!mounted) return;
      if (r['ok'] == 1) {
        setState(() { _isRecording = true; _recordingSeconds = 0; _recordingBytes = 0; });
        _timerCounter = Timer.periodic(const Duration(seconds: 1), (_) { if (mounted) setState(() => _recordingSeconds++); });
        _statusTimer = Timer.periodic(const Duration(seconds: 2), (_) async {
          try {
            final s = await _http!.audioRecordStatus();
            if (mounted && s['ok'] == 1) setState(() { _recordingSeconds = s['seconds'] ?? _recordingSeconds; _recordingBytes = s['bytes'] ?? _recordingBytes; });
          } catch (_) {}
        });
      }
    } catch (_) {}
  }

  Future<void> _stopRecording() async {
    _timerCounter?.cancel(); _statusTimer?.cancel();
    try { await _http!.audioRecordStop(); } catch (_) {}
    if (mounted) { setState(() => _isRecording = false); _loadFiles(); }
  }

  Future<void> _playFile(String f) async {
    try { final r = await _http!.audioPlay(f); if (mounted) setState(() => _playingFile = r['ok'] == 1 ? f : null); } catch (_) {}
  }

  Future<void> _stopPlayback() async {
    try { await _http!.audioStop(); } catch (_) {}
    if (mounted) setState(() => _playingFile = null);
  }

  String _t(int s) => '${(s ~/ 60).toString().padLeft(2, '0')}:${(s % 60).toString().padLeft(2, '0')}';
  String _b(int b) => b < 1024 ? '$b B' : b < 1048576 ? '${(b / 1024).toStringAsFixed(1)} KB' : '${(b / 1048576).toStringAsFixed(1)} MB';

  @override
  Widget build(BuildContext context) {
    final tt = Theme.of(context).textTheme;
    return Scaffold(
      appBar: AppBar(title: const Text('Settings'), backgroundColor: Theme.of(context).colorScheme.primaryContainer),
      body: Center(
        child: ConstrainedBox(
          constraints: const BoxConstraints(maxWidth: 420),
          child: ListView(padding: const EdgeInsets.all(12), children: [
        if (_settingsLoading) const Center(child: CircularProgressIndicator()) else ...[
          Text('WiFi', style: tt.titleSmall),
          Row(children: [const Text('Enable'), const Spacer(), Switch.adaptive(value: _wifiEnabled, onChanged: (v) => setState(() => _wifiEnabled = v))]),
          TextField(controller: _ssidController, enabled: _wifiEnabled, decoration: const InputDecoration(labelText: 'SSID', border: OutlineInputBorder(), isDense: true)),
          const SizedBox(height: 8),
          TextField(controller: _passwordController, enabled: _wifiEnabled, obscureText: true, decoration: const InputDecoration(labelText: 'Password', border: OutlineInputBorder(), isDense: true)),
          const SizedBox(height: 12),
          Text('Volume: ${_volume.round()}', style: tt.titleSmall),
          Slider(value: _volume, min: 0, max: 100, divisions: 100, onChanged: (v) => setState(() => _volume = v)),
          const SizedBox(height: 8),
          Row(children: [const Text('Camera Stream'), const Spacer(),
            Switch.adaptive(value: _camStreamEnabled, onChanged: (v) { setState(() => _camStreamEnabled = v); _state!.toggleCameraStream(v); })]),
          const SizedBox(height: 12),
          Row(children: [
            Expanded(child: FilledButton.icon(onPressed: _saveSettings, icon: const Icon(Icons.save, size: 18), label: const Text('Save'))),
            const SizedBox(width: 8),
            OutlinedButton.icon(onPressed: _confirmFactoryReset, icon: const Icon(Icons.restart_alt, size: 18), label: const Text('FactoryReset'), style: OutlinedButton.styleFrom(foregroundColor: Colors.redAccent)),
          ]),
        ],
        const Divider(height: 32),
        Card(child: Padding(padding: const EdgeInsets.all(12), child: Column(children: [
          Row(children: [Icon(_isRecording ? Icons.fiber_manual_record : Icons.mic, color: _isRecording ? Colors.red : null), const SizedBox(width: 8),
            Text(_isRecording ? 'Recording ${_t(_recordingSeconds)}' : 'Audio Recorder', style: tt.titleSmall)]),
          if (_isRecording) Text(_b(_recordingBytes), style: TextStyle(color: Colors.grey[600], fontSize: 12)),
          const SizedBox(height: 8),
          _isRecording
              ? FilledButton.icon(onPressed: _stopRecording, icon: const Icon(Icons.stop), label: const Text('Stop'))
              : FilledButton.icon(onPressed: _startRecording, icon: const Icon(Icons.fiber_manual_record), label: const Text('Record'),
                  style: FilledButton.styleFrom(backgroundColor: Colors.red, foregroundColor: Colors.white)),
        ]))),
        const SizedBox(height: 8),
        Text('Recordings (${_files.length})', style: tt.titleSmall),
        if (_loadingFiles) const Center(child: Padding(padding: EdgeInsets.all(32), child: CircularProgressIndicator()))
        else if (_files.isEmpty) const Center(child: Padding(padding: EdgeInsets.all(32), child: Text('No recordings', style: TextStyle(color: Colors.grey))))
        else SizedBox(
          height: 280,
          child: ListView(children: _files.map((f) => ListTile(
            visualDensity: VisualDensity.compact,
            dense: true,
            leading: Icon(_playingFile == f ? Icons.volume_up : Icons.music_note, color: _playingFile == f ? Colors.green : null),
            title: Text(f, overflow: TextOverflow.ellipsis),
            trailing: _playingFile == f
                ? IconButton(icon: const Icon(Icons.stop_circle, color: Colors.red), onPressed: _stopPlayback)
                : IconButton(icon: const Icon(Icons.play_circle, color: Colors.green), onPressed: () => _playFile(f)),
          )).toList()),
        ),
      ]))),
    );
  }
}
