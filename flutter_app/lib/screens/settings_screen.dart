import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:path_provider/path_provider.dart';

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
  String? _playingFile;

  // File Manager state (replaces old audio-only _files / _loadingFiles)
  String _fmDir = '/';
  List<Map<String, dynamic>> _fmEntries = [];
  bool _fmLoading = false;
  String _fmError = '';
  String _fmCapText = '';

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
        if (mounted) { _loadSettings(); _fmLoad(); }
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
    if (mounted) { setState(() => _isRecording = false); _fmLoad(); }
  }

  Future<void> _playFile(String f) async {
    try { final r = await _http!.audioPlay(f); if (mounted) setState(() => _playingFile = r['ok'] == 1 ? f : null); } catch (_) {}
  }

  Future<void> _stopPlayback() async {
    try { await _http!.audioStop(); } catch (_) {}
    if (mounted) setState(() => _playingFile = null);
  }

  // === File Manager methods ===
  Future<void> _fmLoad() async {
    setState(() { _fmLoading = true; _fmError = ''; });
    try {
      final r = await _http!.filesList(_fmDir);
      if (!mounted) return;
      final entries = <Map<String, dynamic>>[];
      if (r['files'] != null) {
        for (final f in (r['files'] as List)) {
          entries.add(Map<String, dynamic>.from(f as Map));
        }
      }
      entries.sort((a, b) {
        final aDir = a['is_dir'] == true, bDir = b['is_dir'] == true;
        if (aDir != bDir) return aDir ? -1 : 1;
        return (a['name'] as String).compareTo(b['name'] as String);
      });
      String cap = '';
      final tk = r['total_kb'] as num?;
      final fk = r['free_kb'] as num?;
      if (tk != null && tk > 0) {
        final free = fk?.toDouble() ?? 0, total = tk.toDouble(), used = total - free;
        String fmt(double v) => v > 1048576 ? '${(v / 1048576).toStringAsFixed(1)}GB' : v > 1024 ? '${(v / 1024).toStringAsFixed(1)}MB' : '${v.toInt()}KB';
        cap = '${fmt(used)} used / ${fmt(total)}';
      }
      final cur = r['current'] as String? ?? _fmDir;
      setState(() {
        _fmEntries = entries;
        _fmDir = cur;
        _fmCapText = cap;
        _fmLoading = false;
      });
    } catch (e) {
      if (mounted) setState(() { _fmError = 'Load failed: $e'; _fmLoading = false; });
    }
  }

  void _fmNavigate(String name) {
    final base = _fmDir.endsWith('/') ? _fmDir : '$_fmDir/';
    _fmDir = '$base$name';
    _fmLoad();
  }

  void _fmNavigateUp() {
    if (_fmDir == '/' || _fmDir.isEmpty) return;
    var p = _fmDir.endsWith('/') ? _fmDir.substring(0, _fmDir.length - 1) : _fmDir;
    final i = p.lastIndexOf('/');
    _fmDir = i <= 0 ? '/' : p.substring(0, i);
    _fmLoad();
  }

  Future<void> _fmDownload(String name) async {
    final base = _fmDir.endsWith('/') ? _fmDir : '$_fmDir/';
    final path = '$base$name';
    try {
      final data = await _http!.filesDownload(path);
      if (!mounted || data.isEmpty) return;
      // Save to app documents directory
      final dir = await getApplicationDocumentsDirectory();
      final localPath = '${dir.path}/$name';
      await File(localPath).writeAsBytes(data);
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text('Saved: $localPath')),
      );
    } catch (e) {
      if (mounted) ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text('Download failed: $e'), backgroundColor: Colors.red),
      );
    }
  }

  Future<void> _fmDelete(String name) async {
    final base = _fmDir.endsWith('/') ? _fmDir : '$_fmDir/';
    final path = '$base$name';
    final confirm = await showDialog<bool>(context: context, builder: (ctx) => AlertDialog(
      title: const Text('Delete file?'),
      content: Text(name),
      actions: [
        TextButton(onPressed: () => Navigator.pop(ctx, false), child: const Text('Cancel')),
        TextButton(onPressed: () => Navigator.pop(ctx, true), child: const Text('Delete', style: TextStyle(color: Colors.redAccent))),
      ],
    ));
    if (confirm != true || !mounted) return;
    try {
      final r = await _http!.filesDelete(path);
      if (!mounted) return;
      if (r['ok'] == 1) {
        _fmLoad();
        ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text('Deleted: $name')));
      } else {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text(r['error'] ?? 'Delete failed'), backgroundColor: Colors.red),
        );
      }
    } catch (e) {
      if (mounted) ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text('Error: $e'), backgroundColor: Colors.red),
      );
    }
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
        Card(child: Padding(padding: const EdgeInsets.all(12), child: Row(children: [
          Icon(_isRecording ? Icons.fiber_manual_record : Icons.mic, color: _isRecording ? Colors.red : null), const SizedBox(width: 8),
          Expanded(child: Text(_isRecording ? 'Recording ${_t(_recordingSeconds)}  ${_b(_recordingBytes)}' : 'Audio Recorder', style: tt.titleSmall)),
          const SizedBox(width: 8),
          _isRecording
              ? FilledButton.icon(onPressed: _stopRecording, icon: const Icon(Icons.stop, size: 18), label: const Text('Stop'))
              : FilledButton.icon(onPressed: _startRecording, icon: const Icon(Icons.fiber_manual_record, size: 18), label: const Text('Record'),
                  style: FilledButton.styleFrom(backgroundColor: Colors.red, foregroundColor: Colors.white)),
        ]))),
        const SizedBox(height: 8),
        // === SD Card Files (directory browser + play/download/delete) ===
        Text('SD Card', style: tt.titleSmall),
        if (_fmCapText.isNotEmpty)
          Text(_fmCapText, style: TextStyle(color: Colors.grey[600], fontSize: 12)),
        const SizedBox(height: 4),
        Row(children: [
          Expanded(child: Text(_fmDir, style: TextStyle(color: Colors.blue[300], fontSize: 13), overflow: TextOverflow.ellipsis)),
          if (_fmDir != '/') TextButton(onPressed: _fmNavigateUp, child: const Text('..')),
        ]),
        if (_fmLoading) const Center(child: Padding(padding: EdgeInsets.all(32), child: CircularProgressIndicator()))
        else if (_fmError.isNotEmpty) Center(child: Text(_fmError, style: const TextStyle(color: Colors.red)))
        else if (_fmEntries.isEmpty) const Center(child: Padding(padding: EdgeInsets.all(32), child: Text('Empty', style: TextStyle(color: Colors.grey))))
        else SizedBox(
          height: 320,
          child: ListView(children: _fmEntries.map((f) {
            final name = f['name'] as String;
            final isDir = f['is_dir'] == true;
            final isMp3 = !isDir && name.toLowerCase().endsWith('.mp3');
            final isPlaying = _playingFile == name;
            return ListTile(
              visualDensity: VisualDensity.compact,
              dense: true,
              leading: Icon(
                isDir ? Icons.folder : isMp3 ? (isPlaying ? Icons.volume_up : Icons.music_note) : Icons.insert_drive_file,
                color: isDir ? Colors.amber : isPlaying ? Colors.green : null,
              ),
              title: Text(name, overflow: TextOverflow.ellipsis),
              subtitle: isDir ? null : Text(_b((f['size'] as num?)?.toInt() ?? 0), style: const TextStyle(fontSize: 11)),
              onTap: isDir ? () => _fmNavigate(name) : null,
              trailing: Row(mainAxisSize: MainAxisSize.min, children: [
                if (isMp3)
                  isPlaying
                      ? IconButton(icon: const Icon(Icons.stop_circle, color: Colors.red, size: 22), onPressed: _stopPlayback)
                      : IconButton(icon: const Icon(Icons.play_circle, color: Colors.green, size: 22), onPressed: () => _playFile(name)),
                if (!isDir) ...[
                  IconButton(icon: const Icon(Icons.download, size: 22), onPressed: () => _fmDownload(name)),
                  const SizedBox(width: 4),
                ],
                IconButton(icon: const Icon(Icons.delete, size: 22, color: Colors.redAccent), onPressed: () => _fmDelete(name)),
              ]),
            );
          }).toList()),
        ),
      ]))),
    );
  }
}
