import 'dart:async';

import 'package:flutter/material.dart';

import '../main.dart';
import '../services/http_service.dart';

/// Audio screen: MP3 recording + playback for ESP32-P4 WIFI6 / LCD-4B boards.
class AudioScreen extends StatefulWidget {
  const AudioScreen({super.key});

  @override
  State<AudioScreen> createState() => _AudioScreenState();
}

class _AudioScreenState extends State<AudioScreen> {
  Esp32HttpService get _http => AppStateScope.of(context).httpService;

  bool _isRecording = false;
  int _recordingSeconds = 0;
  int _recordingBytes = 0;
  Timer? _statusTimer;
  Timer? _timerCounter;

  List<String> _files = [];
  bool _loadingFiles = true;
  String? _playingFile;
  String? _errorMessage;

  @override
  void initState() {
    super.initState();
    _loadFiles();
  }

  @override
  void dispose() {
    _statusTimer?.cancel();
    _timerCounter?.cancel();
    super.dispose();
  }

  Future<void> _loadFiles() async {
    setState(() {
      _loadingFiles = true;
      _errorMessage = null;
    });
    try {
      final resp = await _http.audioList();
      if (resp['ok'] == 1 && resp['files'] != null) {
        setState(() {
          _files = List<String>.from(resp['files'] as List);
          _loadingFiles = false;
        });
      } else {
        setState(() {
          _files = [];
          _loadingFiles = false;
          _errorMessage = resp['error'] as String? ?? 'Failed to load files';
        });
      }
    } catch (e) {
      setState(() {
        _loadingFiles = false;
        _errorMessage = 'Connection error: $e';
      });
    }
  }

  Future<void> _startRecording() async {
    try {
      final resp = await _http.audioRecordStart();
      if (resp['ok'] == 1) {
        setState(() {
          _isRecording = true;
          _recordingSeconds = 0;
          _recordingBytes = 0;
          _errorMessage = null;
        });

        _timerCounter = Timer.periodic(const Duration(seconds: 1), (_) {
          setState(() => _recordingSeconds++);
        });

        _statusTimer = Timer.periodic(const Duration(seconds: 2), (_) async {
          try {
            final s = await _http.audioRecordStatus();
            if (mounted && s['ok'] == 1) {
              setState(() {
                _recordingSeconds = s['seconds'] as int? ?? _recordingSeconds;
                _recordingBytes = s['bytes'] as int? ?? _recordingBytes;
              });
            }
          } catch (_) {}
        });
      } else {
        setState(() => _errorMessage = resp['error'] as String? ?? 'Failed to start');
      }
    } catch (e) {
      setState(() => _errorMessage = 'Error: $e');
    }
  }

  Future<void> _stopRecording() async {
    _timerCounter?.cancel();
    _statusTimer?.cancel();
    try {
      final resp = await _http.audioRecordStop();
      setState(() => _isRecording = false);
      if (resp['ok'] == 1) {
        _loadFiles();
      } else {
        setState(() => _errorMessage = resp['error'] as String? ?? 'Failed to stop');
      }
    } catch (e) {
      setState(() {
        _isRecording = false;
        _errorMessage = 'Error: $e';
      });
    }
  }

  Future<void> _playFile(String filename) async {
    try {
      final resp = await _http.audioPlay(filename);
      if (resp['ok'] == 1) {
        setState(() {
          _playingFile = filename;
          _errorMessage = null;
        });
      } else {
        setState(() => _errorMessage = resp['error'] as String? ?? 'Play failed');
      }
    } catch (e) {
      setState(() => _errorMessage = 'Error: $e');
    }
  }

  Future<void> _stopPlayback() async {
    try {
      await _http.audioStop();
      setState(() => _playingFile = null);
    } catch (_) {
      setState(() => _playingFile = null);
    }
  }

  String _formatDuration(int seconds) {
    final m = seconds ~/ 60;
    final s = seconds % 60;
    return '${m.toString().padLeft(2, '0')}:${s.toString().padLeft(2, '0')}';
  }

  String _formatBytes(int bytes) {
    if (bytes < 1024) return '$bytes B';
    if (bytes < 1024 * 1024) return '${(bytes / 1024).toStringAsFixed(1)} KB';
    return '${(bytes / (1024 * 1024)).toStringAsFixed(1)} MB';
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('Audio Recorder'),
        backgroundColor: Theme.of(context).colorScheme.primaryContainer,
        actions: [
          IconButton(
            icon: const Icon(Icons.refresh),
            tooltip: 'Refresh file list',
            onPressed: _loadFiles,
          ),
        ],
      ),
      body: Column(
        children: [
          // ===== Recording Controls =====
          Card(
            margin: const EdgeInsets.all(12),
            child: Padding(
              padding: const EdgeInsets.all(16),
              child: Column(
                children: [
                  Row(
                    mainAxisAlignment: MainAxisAlignment.center,
                    children: [
                      Icon(
                        _isRecording ? Icons.fiber_manual_record : Icons.mic,
                        color: _isRecording ? Colors.red : Colors.grey,
                        size: 32,
                      ),
                      const SizedBox(width: 12),
                      Text(
                        _isRecording
                            ? 'Recording ${_formatDuration(_recordingSeconds)}'
                            : 'Audio Recorder',
                        style: Theme.of(context).textTheme.titleMedium,
                      ),
                    ],
                  ),
                  if (_isRecording) ...[
                    const SizedBox(height: 8),
                    Text(
                      _formatBytes(_recordingBytes),
                      style: Theme.of(context).textTheme.bodySmall?.copyWith(
                            color: Colors.grey[600],
                          ),
                    ),
                  ],
                  const SizedBox(height: 16),
                  Row(
                    mainAxisAlignment: MainAxisAlignment.center,
                    children: [
                      if (!_isRecording)
                        FilledButton.icon(
                          onPressed: _startRecording,
                          icon: const Icon(Icons.fiber_manual_record),
                          label: const Text('Record'),
                          style: FilledButton.styleFrom(
                            backgroundColor: Colors.red,
                            foregroundColor: Colors.white,
                          ),
                        ),
                      if (_isRecording)
                        FilledButton.icon(
                          onPressed: _stopRecording,
                          icon: const Icon(Icons.stop),
                          label: const Text('Stop'),
                        ),
                    ],
                  ),
                ],
              ),
            ),
          ),

          // ===== Error Message =====
          if (_errorMessage != null)
            Padding(
              padding: const EdgeInsets.symmetric(horizontal: 12),
              child: Card(
                color: Colors.orange.shade50,
                child: Padding(
                  padding: const EdgeInsets.all(12),
                  child: Row(
                    children: [
                      const Icon(Icons.warning_amber, color: Colors.orange),
                      const SizedBox(width: 8),
                      Expanded(
                        child: Text(
                          _errorMessage!,
                          style: const TextStyle(color: Colors.orange),
                        ),
                      ),
                      IconButton(
                        icon: const Icon(Icons.close, size: 18),
                        onPressed: () => setState(() => _errorMessage = null),
                      ),
                    ],
                  ),
                ),
              ),
            ),

          // ===== File List =====
          Padding(
            padding: const EdgeInsets.symmetric(horizontal: 12),
            child: Row(
              children: [
                const Icon(Icons.library_music, size: 20),
                const SizedBox(width: 8),
                Text(
                  'Recordings (${_files.length})',
                  style: Theme.of(context).textTheme.titleSmall,
                ),
              ],
            ),
          ),
          Expanded(
            child:
                _loadingFiles
                    ? const Center(child: CircularProgressIndicator())
                    : _files.isEmpty
                    ? Center(
                      child: Text(
                        'No recordings yet',
                        style: Theme.of(
                          context,
                        ).textTheme.bodyLarge?.copyWith(color: Colors.grey),
                      ),
                    )
                    : ListView.builder(
                      padding: const EdgeInsets.symmetric(horizontal: 8),
                      itemCount: _files.length,
                      itemBuilder: (context, i) {
                        final filename = _files[i];
                        final isPlaying = _playingFile == filename;
                        return ListTile(
                          leading: Icon(
                            isPlaying ? Icons.volume_up : Icons.music_note,
                            color: isPlaying ? Colors.green : null,
                          ),
                          title: Text(
                            filename,
                            overflow: TextOverflow.ellipsis,
                          ),
                          trailing: Row(
                            mainAxisSize: MainAxisSize.min,
                            children: [
                              if (isPlaying)
                                IconButton(
                                  icon: const Icon(Icons.stop_circle),
                                  color: Colors.red,
                                  onPressed: _stopPlayback,
                                )
                              else
                                IconButton(
                                  icon: const Icon(Icons.play_circle),
                                  color: Colors.green,
                                  onPressed: () => _playFile(filename),
                                ),
                            ],
                          ),
                        );
                      },
                    ),
          ),
        ],
      ),
    );
  }
}
