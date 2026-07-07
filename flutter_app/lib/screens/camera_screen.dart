import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:path_provider/path_provider.dart';

import '../main.dart';
import '../providers/app_state.dart';
import '../widgets/image_viewer.dart';

/// Full-screen camera view with parameter config panel on the left.
class CameraScreen extends StatefulWidget {
  const CameraScreen({super.key});

  @override
  State<CameraScreen> createState() => _CameraScreenState();
}

class _CameraScreenState extends State<CameraScreen> {
  bool _logsVisible = true;
  final ScrollController _logScrollController = ScrollController();
  double _quality = 55;
  Timer? _qualityDebounce;
  late final AppState _state = AppStateScope.of(context);

  // Parameter config form state
  bool _wifiEnabled = false;
  final _ssidController = TextEditingController();
  final _passwordController = TextEditingController();
  double _volume = 60;
  bool _camStreamEnabled = false;

  @override
  void initState() {
    super.initState();
    _loadSettingsFromDevice();
  }

  Future<void> _loadSettingsFromDevice() async {
    try {
      await _state.fetchSettings();
      if (mounted) {
        setState(() {
          _wifiEnabled = _state.wifiEnabled;
          _ssidController.text = _state.ssid;
          _passwordController.text = _state.password;
          _volume = _state.volume.toDouble();
          _camStreamEnabled = _state.camStreamEnabled;
        });
      }
    } catch (_) {
      // Firmware may not support settings API — use defaults
    }
  }

  void _saveSettings() {
    _state.updateSettings(
      wifiEnabled: _wifiEnabled,
      ssid: _ssidController.text,
      password: _passwordController.text,
      volume: _volume.round(),
    );
  }

  void _confirmFactoryReset() {
    showDialog(
      context: context,
      builder: (ctx) => AlertDialog(
        backgroundColor: const Color(0xFF1a1a2e),
        title: const Text('恢复出厂设置',
            style: TextStyle(color: Colors.white)),
        content: const Text('确定要恢复出厂设置吗？设备将重启。',
            style: TextStyle(color: Colors.white70)),
        actions: [
          TextButton(
            onPressed: () => Navigator.pop(ctx),
            child: const Text('取消'),
          ),
          TextButton(
            onPressed: () {
              Navigator.pop(ctx);
              _state.factoryReset();
            },
            child: const Text('确定',
                style: TextStyle(color: Colors.redAccent)),
          ),
        ],
      ),
    );
  }

  Future<void> _saveImage() async {
    try {
      final data = await _state.saveImage();
      if (data != null) {
        final path = await _saveFile(data, 'capture', 'jpg');
        if (mounted) {
          ScaffoldMessenger.of(context)
              .showSnackBar(SnackBar(content: Text('Saved: $path')));
        }
      }
    } catch (e) {
      if (mounted) {
        ScaffoldMessenger.of(context)
            .showSnackBar(SnackBar(content: Text('Save failed: $e')));
      }
    }
  }

  Future<void> _saveRaw() async {
    try {
      final data = await _state.saveRaw();
      if (data != null) {
        final path = await _saveFile(data, 'raw', 'bin');
        if (mounted) {
          ScaffoldMessenger.of(context)
              .showSnackBar(SnackBar(content: Text('Saved: $path')));
        }
      }
    } catch (e) {
      if (mounted) {
        ScaffoldMessenger.of(context)
            .showSnackBar(SnackBar(content: Text('Save failed: $e')));
      }
    }
  }

  Future<String> _saveFile(Uint8List data, String prefix, String ext) async {
    final dir = await getApplicationDocumentsDirectory();
    final path =
        '${dir.path}/esp32_${prefix}_${DateTime.now().millisecondsSinceEpoch}.$ext';
    await File(path).writeAsBytes(data);
    return path;
  }

  Future<void> _setQuality(double val) async {
    setState(() => _quality = val);
    _qualityDebounce?.cancel();
    _qualityDebounce = Timer(const Duration(milliseconds: 300), () {
      _state.setQuality(val.round());
    });
  }

  Widget _buildConfigPanel(AppState state) {
    return Container(
      width: 280,
      color: const Color(0xFF1a1a2e),
      child: SingleChildScrollView(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            const Text(
              '参数配置',
              style: TextStyle(
                color: Colors.white,
                fontSize: 18,
                fontWeight: FontWeight.bold,
              ),
            ),
            const SizedBox(height: 8),
            const Divider(color: Colors.white24),
            const SizedBox(height: 16),
            if (_state.settingsLoading)
              const Center(
                child: Padding(
                  padding: EdgeInsets.all(32),
                  child: CircularProgressIndicator(color: Colors.white38),
                ),
              )
            else ...[
              _sectionTitle(Icons.tune, '图像质量'),
              const SizedBox(height: 4),
              Row(
                children: [
                  const Text('JPEG Quality',
                      style: TextStyle(color: Colors.white70, fontSize: 13)),
                  const Spacer(),
                  Text('${_quality.round()}',
                      style: const TextStyle(
                          color: Colors.white,
                          fontWeight: FontWeight.bold)),
                ],
              ),
              Slider(
                value: _quality,
                min: 1,
                max: 100,
                divisions: 99,
                activeColor: Colors.indigo,
                inactiveColor: Colors.white24,
                onChanged: _setQuality,
              ),
              const SizedBox(height: 20),
              _sectionTitle(Icons.wifi, 'WiFi'),
              const SizedBox(height: 8),
              Row(
                children: [
                  const Text('启用 WiFi',
                      style: TextStyle(color: Colors.white70, fontSize: 13)),
                  const Spacer(),
                  SizedBox(
                    height: 24,
                    child: Switch.adaptive(
                      value: _wifiEnabled,
                      activeTrackColor: Colors.cyan,
                      onChanged: (v) => setState(() => _wifiEnabled = v),
                    ),
                  ),
                ],
              ),
              const SizedBox(height: 8),
              TextField(
                controller: _ssidController,
                enabled: _wifiEnabled,
                style: const TextStyle(color: Colors.white, fontSize: 14),
                decoration: _inputDecoration('SSID'),
              ),
              const SizedBox(height: 8),
              TextField(
                controller: _passwordController,
                enabled: _wifiEnabled,
                obscureText: true,
                style: const TextStyle(color: Colors.white, fontSize: 14),
                decoration: _inputDecoration('Password'),
              ),
              const SizedBox(height: 20),
              _sectionTitle(Icons.volume_up, '音量'),
              const SizedBox(height: 4),
              Row(
                children: [
                  const Text('0',
                      style: TextStyle(color: Colors.white38, fontSize: 11)),
                  Expanded(
                    child: Slider(
                      value: _volume,
                      min: 0,
                      max: 100,
                      divisions: 100,
                      activeColor: Colors.cyan,
                      inactiveColor: Colors.white24,
                      onChanged: (v) => setState(() => _volume = v),
                    ),
                  ),
                  const Text('100',
                      style: TextStyle(color: Colors.white38, fontSize: 11)),
                  const SizedBox(width: 8),
                  Text('${_volume.round()}',
                      style: const TextStyle(
                          color: Colors.white,
                          fontWeight: FontWeight.bold)),
                ],
              ),
              const SizedBox(height: 20),
              _sectionTitle(Icons.videocam, '摄像头'),
              const SizedBox(height: 8),
              Row(
                children: [
                  const Text('Camera Stream',
                      style: TextStyle(color: Colors.white70, fontSize: 13)),
                  const Spacer(),
                  SizedBox(
                    height: 24,
                    child: Switch.adaptive(
                      value: _camStreamEnabled,
                      activeTrackColor: Colors.cyan,
                      onChanged: (v) {
                        setState(() => _camStreamEnabled = v);
                        _state.toggleCameraStream(v);
                      },
                    ),
                  ),
                ],
              ),
              const SizedBox(height: 24),
              SizedBox(
                width: double.infinity,
                child: ElevatedButton.icon(
                  icon: const Icon(Icons.save, size: 18),
                  label: const Text('保存设置'),
                  style: ElevatedButton.styleFrom(
                    backgroundColor: Colors.cyan,
                    foregroundColor: Colors.white,
                    padding: const EdgeInsets.symmetric(vertical: 12),
                  ),
                  onPressed: _saveSettings,
                ),
              ),
              const SizedBox(height: 12),
              SizedBox(
                width: double.infinity,
                child: OutlinedButton.icon(
                  icon: const Icon(Icons.restart_alt, size: 18),
                  label: const Text('恢复出厂设置'),
                  style: OutlinedButton.styleFrom(
                    foregroundColor: Colors.redAccent,
                    side: const BorderSide(color: Colors.redAccent),
                    padding: const EdgeInsets.symmetric(vertical: 12),
                  ),
                  onPressed: _confirmFactoryReset,
                ),
              ),
            ],
          ],
        ),
      ),
    );
  }

  InputDecoration _inputDecoration(String label) {
    return InputDecoration(
      labelText: label,
      labelStyle: const TextStyle(color: Colors.white38, fontSize: 13),
      enabledBorder: const OutlineInputBorder(
        borderSide: BorderSide(color: Colors.white24),
      ),
      focusedBorder: const OutlineInputBorder(
        borderSide: BorderSide(color: Colors.cyan),
      ),
      border: const OutlineInputBorder(),
      contentPadding:
          const EdgeInsets.symmetric(horizontal: 12, vertical: 10),
      isDense: true,
    );
  }

  Widget _sectionTitle(IconData icon, String title) {
    return Row(
      children: [
        Icon(icon, color: Colors.cyan, size: 18),
        const SizedBox(width: 8),
        Text(
          title,
          style: const TextStyle(
            color: Colors.white,
            fontSize: 15,
            fontWeight: FontWeight.w600,
          ),
        ),
      ],
    );
  }

  Widget _buildLogPanel(AppState state) {
    return Container(
      height: 160,
      color: Colors.black87,
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Container(
            padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 6),
            color: Colors.white10,
            child: Row(
              children: [
                const Icon(Icons.list_alt, color: Colors.white70, size: 16),
                const SizedBox(width: 8),
                const Text(
                  'Event Log',
                  style: TextStyle(color: Colors.white70, fontSize: 12),
                ),
                const Spacer(),
                Text(
                  '${state.logs.length} entries',
                  style: const TextStyle(color: Colors.white38, fontSize: 11),
                ),
                const SizedBox(width: 8),
                InkWell(
                  onTap: () {
                    if (_logScrollController.hasClients) {
                      _logScrollController.animateTo(
                        _logScrollController.position.maxScrollExtent,
                        duration: const Duration(milliseconds: 200),
                        curve: Curves.easeOut,
                      );
                    }
                  },
                  child: const Icon(
                    Icons.arrow_downward,
                    color: Colors.white54,
                    size: 16,
                  ),
                ),
              ],
            ),
          ),
          Expanded(
            child: state.logs.isEmpty
                ? const Center(
                    child: Text(
                      'No logs yet',
                      style: TextStyle(color: Colors.white24, fontSize: 12),
                    ),
                  )
                : ListView.builder(
                      controller: _logScrollController,
                      padding: const EdgeInsets.symmetric(
                        horizontal: 12,
                        vertical: 4,
                      ),
                      itemCount: state.logs.length,
                      itemBuilder: (context, index) {
                        final entry = state.logs[index];
                        final time =
                            '${entry.timestamp.hour.toString().padLeft(2, '0')}:'
                            '${entry.timestamp.minute.toString().padLeft(2, '0')}:'
                            '${entry.timestamp.second.toString().padLeft(2, '0')}';
                        return Padding(
                          padding: const EdgeInsets.symmetric(vertical: 1),
                          child: Row(
                            crossAxisAlignment: CrossAxisAlignment.start,
                            children: [
                              Text(
                                '[$time]',
                                style: const TextStyle(
                                  color: Colors.white38,
                                  fontSize: 11,
                                  fontFamily: 'monospace',
                                ),
                              ),
                              const SizedBox(width: 6),
                              Expanded(
                                child: Text(
                                  entry.message,
                                  style: const TextStyle(
                                    color: Colors.white70,
                                    fontSize: 12,
                                    fontFamily: 'monospace',
                                  ),
                                ),
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

  @override
  void dispose() {
    _qualityDebounce?.cancel();
    _logScrollController.dispose();
    _ssidController.dispose();
    _passwordController.dispose();
    _state.disconnect();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final state = _state;

    return PopScope(
      canPop: false,
      onPopInvokedWithResult: (didPop, _) {
        if (!didPop) {
          state.disconnect();
          Navigator.of(context).pop();
        }
      },
      child: SelectionArea(
        child: Scaffold(
          appBar: AppBar(
            title: Text(state.connectedDevice?.name ?? 'Camera'),
            backgroundColor: Colors.black,
            foregroundColor: Colors.white,
          actions: [
            IconButton(
              icon: const Icon(Icons.save_alt),
              tooltip: 'Save JPEG',
              onPressed: _saveImage,
            ),
            IconButton(
              icon: const Icon(Icons.save),
              tooltip: 'Save Raw',
              onPressed: _saveRaw,
            ),
            IconButton(
              icon: Icon(
                _logsVisible ? Icons.terminal : Icons.terminal_outlined,
              ),
              tooltip: 'Toggle log panel',
              onPressed: () => setState(() => _logsVisible = !_logsVisible),
            ),
            IconButton(
              icon: const Icon(Icons.close),
              tooltip: 'Disconnect & Close',
              onPressed: () {
                state.disconnect();
                Navigator.pop(context);
              },
            ),
          ],
        ),
        backgroundColor: Colors.black,
        body: ListenableBuilder(
          listenable: state,
          builder: (context, _) {
            return Column(
              children: [
                Expanded(
                  child: Stack(
                    fit: StackFit.expand,
                    children: [
                      if (state.latestFrame != null)
                        ImageViewer(imageBytes: state.latestFrame!)
                      else
                        const Center(
                          child: Column(
                            mainAxisSize: MainAxisSize.min,
                            children: [
                              Icon(Icons.camera, size: 64, color: Colors.white24),
                              SizedBox(height: 16),
                              Text('Waiting for camera feed...',
                                style: TextStyle(color: Colors.white54, fontSize: 16)),
                              SizedBox(height: 16),
                              CircularProgressIndicator(color: Colors.white38),
                            ],
                          ),
                        ),
                      if (state.latestText.isNotEmpty || state.detectionEnabled)
                        Positioned(
                          left: 0, right: 0, bottom: 0,
                          child: Container(
                            padding: const EdgeInsets.all(12),
                            decoration: const BoxDecoration(
                              gradient: LinearGradient(
                                begin: Alignment.topCenter, end: Alignment.bottomCenter,
                                colors: [Colors.transparent, Colors.black54],
                              ),
                            ),
                            child: Row(children: [
                              const Icon(Icons.info_outline, color: Colors.white70, size: 18),
                              const SizedBox(width: 8),
                              Expanded(
                                child: Column(crossAxisAlignment: CrossAxisAlignment.start, mainAxisSize: MainAxisSize.min, children: [
                                  if (state.latestText.isNotEmpty)
                                    Text(state.latestText, style: const TextStyle(color: Colors.white, fontSize: 14)),
                                  Text('FPS: ${state.fps.toStringAsFixed(1)}', style: const TextStyle(color: Colors.greenAccent, fontSize: 12)),
                                  if (state.detectionEnabled)
                                    Text('People: ${state.personCount}  Conf: ${state.maxConfidence.toStringAsFixed(3)}',
                                      style: TextStyle(
                                        color: state.personCount > 0 ? Colors.greenAccent : Colors.white70, fontSize: 13,
                                        fontWeight: state.personCount > 0 ? FontWeight.bold : FontWeight.normal,
                                      ),
                                    ),
                                ]),
                              ),
                            ]),
                          ),
                        ),
                      if (state.deviceStatus.isNotEmpty)
                        Positioned(
                          top: 8, right: 8,
                          child: Container(
                            padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
                            decoration: BoxDecoration(color: Colors.black54, borderRadius: BorderRadius.circular(8)),
                            child: Text(state.deviceStatus, style: const TextStyle(color: Colors.white, fontSize: 12)),
                          ),
                        ),
                    ],
                  ),
                ),
                if (_logsVisible) _buildLogPanel(state),
              ],
            );
          },
        ),
      ),
    ),
    );
  }
}
