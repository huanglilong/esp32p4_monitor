/// ULog Viewer — parse .ulg files, display extracted JPEG frames,
/// play as slideshow, and save individual/all frames.
///
/// Supports two entry points:
/// - Remote: download .ulg from ESP32 SD card via filesDownload API
/// - Local: parse a previously downloaded .ulg file on the device

import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:path_provider/path_provider.dart';

import '../main.dart';
import '../providers/app_state.dart';
import '../services/mjpeg_avi_writer.dart';
import '../services/ulog_parser.dart';

class UlogViewerScreen extends StatefulWidget {
  /// Remote path on ESP32 SD card (e.g. /ulog/2026-01-01.ulg).
  /// If provided, the file is downloaded first.
  final String? remotePath;

  /// Local path of a .ulg file already on device.
  /// If provided, parsing starts immediately without download.
  final String? localPath;

  const UlogViewerScreen({super.key, this.remotePath, this.localPath})
      : assert(remotePath != null || localPath != null,
            'Must provide remotePath or localPath');

  @override
  State<UlogViewerScreen> createState() => _UlogViewerScreenState();
}

class _UlogViewerScreenState extends State<UlogViewerScreen> {
  AppState? _state;
  bool _initialized = false;

  bool _downloading = false;
  bool _parsing = false;
  String? _error;
  UlogParseResult? _result;
  String? _localPath;

  int _selectedIndex = -1;
  bool _isPlaying = false;
  double _framerate = 5.0;
  Timer? _playTimer;
  final Map<int, ImageProvider> _thumbCache = {};
  final ScrollController _thumbScrollController = ScrollController();

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    _state = AppStateScope.of(context);
    if (!_initialized) {
      _initialized = true;
      _startProcessing();
    }
  }

  @override
  void dispose() {
    _playTimer?.cancel();
    _thumbScrollController.dispose();
    _cleanupTempFile();
    super.dispose();
  }

  // ── Processing ──

  Future<void> _startProcessing() async {
    if (widget.localPath != null) {
      setState(() {
        _parsing = true;
        _localPath = widget.localPath;
      });
      final result = UlogParser.parseFile(widget.localPath!);
      if (!mounted) return;
      setState(() {
        _parsing = false;
        _result = result;
        if (result.isSuccess && result.frames.isNotEmpty) {
          _selectedIndex = 0;
        }
      });
      return;
    }

    // Download from remote device
    setState(() {
      _downloading = true;
      _error = null;
    });

    try {
      final http = _state?.httpService;
      if (http == null) throw Exception('Not connected');

      final data = await http.filesDownload(widget.remotePath!);
      debugPrint('[UlogViewer] filesDownload returned ${data.length} bytes');
      if (data.isEmpty) throw Exception('Download returned empty data');

      // Debug: log first 16 bytes to diagnose format issues
      debugPrint('[UlogViewer] Downloaded ${data.length} bytes, '
          'first 16: ${data.take(16).map((b) => b.toRadixString(16).padLeft(2, '0')).join(' ')}');

      if (!mounted) return;
      setState(() {
        _downloading = false;
        _parsing = true;
      });

      // Parse directly from memory — avoids file I/O issues on sandboxed macOS
      final result = UlogParser.parseBytes(data);

      // Also save to temp for potential later use
      final tempDir = await getTemporaryDirectory();
      final filename = widget.remotePath!.split('/').last;
      final localPath = '${tempDir.path}/$filename';
      try {
        await File(localPath).writeAsBytes(data);
        _localPath = localPath;
      } catch (_) {
        // Non-fatal: file save is just a convenience
      }

      if (!mounted) return;
      setState(() {
        _parsing = false;
        _result = result;
        if (result.isSuccess && result.frames.isNotEmpty) {
          _selectedIndex = 0;
        }
      });
    } catch (e) {
      if (!mounted) return;
      setState(() {
        _downloading = false;
        _parsing = false;
        _error = 'Failed: $e';
      });
    }
  }

  void _cleanupTempFile() {
    if (_localPath != null && widget.localPath == null) {
      try {
        File(_localPath!).deleteSync();
      } catch (_) {}
    }
  }

  // ── Playback ──

  void _togglePlayback() {
    if (_result == null || _result!.frames.isEmpty) return;

    if (_isPlaying) {
      _playTimer?.cancel();
      setState(() => _isPlaying = false);
    } else {
      _startPlayTimer();
      setState(() => _isPlaying = true);
    }
  }

  void _startPlayTimer() {
    _playTimer?.cancel();
    final intervalMs = (1000 / _framerate).round();
    _playTimer = Timer.periodic(Duration(milliseconds: intervalMs), (_) {
      if (!mounted) return;
      final next = (_selectedIndex + 1) % _result!.frames.length;
      setState(() => _selectedIndex = next);
      _scrollToThumb(next);
    });
  }

  /// Auto-scroll thumbnail strip to keep selected item visible.
  void _scrollToThumb(int index) {
    if (!_thumbScrollController.hasClients) return;
    // Each thumbnail is 72px wide + 4px margin = 76px per item
    const itemWidth = 76.0;
    final offset = index * itemWidth - _thumbScrollController.position.viewportDimension / 2 + itemWidth / 2;
    _thumbScrollController.animateTo(
      offset.clamp(0.0, _thumbScrollController.position.maxScrollExtent),
      duration: const Duration(milliseconds: 200),
      curve: Curves.easeOut,
    );
  }

  // ── Save ──

  Future<void> _saveCurrentFrame() async {
    if (_result == null || _selectedIndex < 0) return;
    final frame = _result!.frames[_selectedIndex];

    try {
      final dir = await getApplicationDocumentsDirectory();
      final name =
          'frame_${frame.frameIndex}_${DateTime.now().millisecondsSinceEpoch}.jpg';
      await File('${dir.path}/$name').writeAsBytes(frame.jpegData);
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text('Saved: $name')),
      );
    } catch (e) {
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text('Save failed: $e'), backgroundColor: Colors.red),
      );
    }
  }

  bool _savingVideo = false;

  Future<void> _saveAllFrames() async {
    if (_result == null || _result!.frames.isEmpty) return;

    setState(() => _savingVideo = true);

    try {
      final dir = await getApplicationDocumentsDirectory();
      final ulgName =
          (widget.remotePath ?? widget.localPath ?? 'ulog').split('/').last.replaceAll('.ulg', '');

      // Generate MJPEG AVI video (pure Dart, no temp files needed)
      final videoPath = '${dir.path}/${ulgName}.avi';
      final success = await _generateVideo(videoPath);

      if (!mounted) return;
      setState(() => _savingVideo = false);

      if (success) {
        final size = File(videoPath).lengthSync();
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(
            content: Text('Video saved: ${ulgName}.avi (${_fmtBytes(size)})'),
            duration: const Duration(seconds: 4),
          ),
        );
      } else {
        ScaffoldMessenger.of(context).showSnackBar(
          const SnackBar(
            content: Text('Video generation failed'),
            backgroundColor: Colors.red,
          ),
        );
      }
    } catch (e) {
      if (!mounted) return;
      setState(() => _savingVideo = false);
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text('Save failed: $e'), backgroundColor: Colors.red),
      );
    }
  }

  /// Generate MJPEG AVI video from JPEG frames (pure Dart, no native dependencies).
  /// Returns true on success.
  Future<bool> _generateVideo(String outputPath) async {
    try {
      final writer = MjpegAviWriter(
        width: _result!.frames.first.width,
        height: _result!.frames.first.height,
        fps: _framerate.round(),
      );
      final jpegFrames = _result!.frames.map((f) => f.jpegData).toList();
      final size = await writer.write(outputPath, jpegFrames);
      return size > 0;
    } catch (e) {
      debugPrint('[UlogViewer] AVI generation error: $e');
      return false;
    }
  }

  // ── Helpers ──

  ImageProvider _getThumbnail(int index) {
    return _thumbCache.putIfAbsent(
      index,
      () => MemoryImage(_result!.frames[index].jpegData),
    );
  }

  String _fmtBytes(int b) {
    if (b < 1024) return '$b B';
    if (b < 1048576) return '${(b / 1024).toStringAsFixed(1)} KB';
    return '${(b / 1048576).toStringAsFixed(2)} MB';
  }

  String get _title =>
      (widget.remotePath ?? widget.localPath ?? 'ULog').split('/').last;

  // ── Build ──

  @override
  Widget build(BuildContext context) {
    final cs = Theme.of(context).colorScheme;
    final tt = Theme.of(context).textTheme;

    return SelectionArea(
      child: Scaffold(
        appBar: AppBar(
          title: Text(_title, overflow: TextOverflow.ellipsis),
          backgroundColor: cs.primaryContainer,
          actions: [
            if (_result != null && _result!.isSuccess) ...[
              IconButton(
                icon: Icon(_isPlaying ? Icons.pause : Icons.play_arrow),
                tooltip: _isPlaying ? 'Pause' : 'Play slideshow',
                onPressed: _togglePlayback,
              ),
              IconButton(
                icon: const Icon(Icons.save),
                tooltip: 'Save current frame',
                onPressed: _saveCurrentFrame,
              ),
              IconButton(
                icon: _savingVideo
                    ? const SizedBox(
                        width: 20, height: 20,
                        child: CircularProgressIndicator(strokeWidth: 2),
                      )
                    : const Icon(Icons.videocam),
                tooltip: 'Save as video',
                onPressed: _savingVideo ? null : _saveAllFrames,
              ),
            ],
          ],
        ),
        body: _buildBody(tt, cs),
      ),
    );
  }

  Widget _buildBody(TextTheme tt, ColorScheme cs) {
    // Loading
    if (_downloading) {
      return Center(
        child: Column(mainAxisSize: MainAxisSize.min, children: [
          const CircularProgressIndicator(),
          const SizedBox(height: 16),
          Text('Downloading $_title...'),
        ]),
      );
    }
    if (_parsing) {
      return const Center(
        child: Column(mainAxisSize: MainAxisSize.min, children: [
          CircularProgressIndicator(),
          SizedBox(height: 16),
          Text('Parsing ULog frames...'),
        ]),
      );
    }

    // Error
    if (_error != null) {
      return Center(
        child: Column(mainAxisSize: MainAxisSize.min, children: [
          Icon(Icons.error_outline, size: 64, color: Colors.red[300]),
          const SizedBox(height: 16),
          Text(_error!, style: tt.bodyLarge, textAlign: TextAlign.center),
          const SizedBox(height: 24),
          FilledButton.icon(
            onPressed: _startProcessing,
            icon: const Icon(Icons.refresh),
            label: const Text('Retry'),
          ),
        ]),
      );
    }

    // No frames
    if (_result == null || !_result!.isSuccess || _result!.frames.isEmpty) {
      return Center(
        child: Column(mainAxisSize: MainAxisSize.min, children: [
          Icon(Icons.videocam_off, size: 64, color: Colors.grey[400]),
          const SizedBox(height: 16),
          Text(_result?.error ?? 'No camera frames found'),
        ]),
      );
    }

    final frames = _result!.frames;

    // Main content
    return Column(children: [
      // Info bar
      Container(
        padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 6),
        color: cs.surfaceContainerHighest,
        child: Row(children: [
          Icon(Icons.videocam, size: 16, color: cs.primary),
          const SizedBox(width: 6),
          Text(
            '${frames.length} frames · ${frames.first.width}×${frames.first.height} · ${_fmtBytes(_result!.totalBytes)}',
            style: tt.bodySmall,
          ),
          const Spacer(),
          Text('${_framerate.toStringAsFixed(1)} fps', style: tt.bodySmall),
          SizedBox(
            width: 100,
            child: Slider(
              value: _framerate,
              min: 0.5,
              max: 30,
              divisions: 59,
              label: '${_framerate.toStringAsFixed(1)} fps',
              onChanged: (v) {
                setState(() => _framerate = v);
                if (_isPlaying) _startPlayTimer();
              },
            ),
          ),
        ]),
      ),

      // Full-size frame viewer
      Expanded(
        flex: 3,
        child: _selectedIndex >= 0 && _selectedIndex < frames.length
            ? Center(
                child: InteractiveViewer(
                  minScale: 0.5,
                  maxScale: 4.0,
                  child: Image.memory(
                    frames[_selectedIndex].jpegData,
                    fit: BoxFit.contain,
                    gaplessPlayback: true,
                  ),
                ),
              )
            : const Center(child: Text('Select a frame')),
      ),

      // Frame info
      if (_selectedIndex >= 0 && _selectedIndex < frames.length)
        Container(
          padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 4),
          color: cs.surfaceContainerHighest,
          child: Row(children: [
            Text(
              'Frame ${frames[_selectedIndex].frameIndex} · '
              '${frames[_selectedIndex].width}×${frames[_selectedIndex].height} · '
              '${_fmtBytes(frames[_selectedIndex].jpegSize)}',
              style: tt.bodySmall,
            ),
            const Spacer(),
            Text(
              '${_selectedIndex + 1} / ${frames.length}',
              style: tt.bodySmall?.copyWith(color: cs.primary),
            ),
          ]),
        ),

      // Thumbnail strip
      SizedBox(
        height: 80,
        child: ListView.builder(
          controller: _thumbScrollController,
          scrollDirection: Axis.horizontal,
          itemCount: frames.length,
          padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
          itemBuilder: (context, index) {
            final selected = index == _selectedIndex;
            return GestureDetector(
              onTap: () {
                setState(() {
                  _selectedIndex = index;
                  if (_isPlaying) {
                    _playTimer?.cancel();
                    _isPlaying = false;
                  }
                });
                _scrollToThumb(index);
              },
              child: Container(
                width: 72,
                margin: const EdgeInsets.symmetric(horizontal: 2),
                decoration: BoxDecoration(
                  border: Border.all(
                    color: selected ? Colors.blue : Colors.transparent,
                    width: 2,
                  ),
                  borderRadius: BorderRadius.circular(4),
                ),
                child: ClipRRect(
                  borderRadius: BorderRadius.circular(2),
                  child: Image(
                    image: _getThumbnail(index),
                    fit: BoxFit.cover,
                    gaplessPlayback: true,
                  ),
                ),
              ),
            );
          },
        ),
      ),
    ]);
  }
}
