import 'dart:async';
import 'dart:io';
import 'package:flutter/foundation.dart';
import 'package:flutter/widgets.dart';
import 'dart:convert';

import '../models/esp32_device.dart';
import '../models/esp32_message.dart';
import '../services/connected_device_store.dart';
import '../services/device_discovery.dart';
import '../services/http_service.dart';

/// A single log entry for the on-screen log panel.
class LogEntry {
  final DateTime timestamp;
  final String message;
  LogEntry({required this.message, DateTime? timestamp})
    : timestamp = timestamp ?? DateTime.now();
}

/// Application state managing device discovery, connection, and data streaming.
class AppState extends ChangeNotifier {
  final DeviceDiscovery _discovery = DeviceDiscovery();
  final Esp32HttpService _httpService = Esp32HttpService();
  final ConnectedDeviceStore _deviceStore = ConnectedDeviceStore();

  // Discovered devices
  final List<Esp32Device> _devices = [];
  final Set<String> _savedConnectedDeviceIds = {};
  bool _isDiscovering = false;
  Future<void>? _loadSavedDevicesFuture;

  // Connection state
  bool _isConnecting = false;
  String? _connectionError;

  // Latest data
  Uint8List? _latestFrame;
  String _latestText = '';
  String _deviceStatus = '';

  // Detection info (polled every 3s)
  int _personCount = 0;
  double _maxConfidence = 0.0;
  bool _detectionEnabled = false;

  // Client-side FPS tracking (frame arrival timestamps)
  double _fps = 0.0;
  final List<int> _fpsTimestamps = [];
  static const int _fpsWindowMs = 2000;

  // Device settings (from GET /api/status)
  bool _wifiEnabled = false;
  String _ssid = '';
  String _password = '';
  int _volume = 60;
  bool _camStreamEnabled = false;
  bool _settingsLoading = false;

  Timer? _detectionPollTimer;

  // Frame throttling — ESP32 streams at 50fps
  DateTime _lastFrameTime = DateTime(2000);
  static const Duration _frameInterval = Duration(milliseconds: 33); // ~30fps

  // On-screen log
  final List<LogEntry> _logs = [];
  static const int _maxLogs = 100;
  bool _disposed = false;

  // Getters
  List<Esp32Device> get devices => List.unmodifiable(_devices);
  bool get isDiscovering => _isDiscovering;
  bool get isConnecting => _isConnecting;
  bool get isConnected => _httpService.isConnected;
  Esp32HttpService get httpService => _httpService;  // Expose for audio API access
  String? get connectionError => _connectionError;
  Uint8List? get latestFrame => _latestFrame;
  String get latestText => _latestText;
  String get deviceStatus => _deviceStatus;
  List<LogEntry> get logs => _logs;
  Esp32Device? get connectedDevice => _httpService.connectedDevice;
  int get personCount => _personCount;
  double get maxConfidence => _maxConfidence;
  bool get detectionEnabled => _detectionEnabled;
  double get fps => _fps;
  bool get wifiEnabled => _wifiEnabled;
  String get ssid => _ssid;
  String get password => _password;
  int get volume => _volume;
  bool get camStreamEnabled => _camStreamEnabled;
  bool get settingsLoading => _settingsLoading;
  Set<String> get savedConnectedDeviceIds => _savedConnectedDeviceIds;

  AppState() {
    _httpService.messageStream.listen(_onMessage);
    _httpService.connectionStream.listen((device) {
      _isConnecting = false;
      _connectionError = null;
      notifyListeners();
    });
    _loadSavedConnectedDevices();
  }

  // ── Device Discovery ──

  Future<void> startDiscovery() async {
    if (_isDiscovering) return;
    await _loadSavedConnectedDevices();
    _isDiscovering = true;
    // Mark all existing devices as not from current scan
    for (int i = 0; i < _devices.length; i++) {
      _devices[i] = _devices[i].copyWith(isFromScan: false);
    }
    notifyListeners();

    _discovery
        .startDiscovery()
        .listen(
          (device) {
            // Newly discovered devices are from scan
            final scanDevice = device.copyWith(isFromScan: true);
            if (_upsertDevice(scanDevice)) {
              notifyListeners();
            }
            // Check reachability in background
            _checkReachability(scanDevice);
          },
          onDone: () {
            _isDiscovering = false;
            // Check reachability for all historical devices too
            for (final d in _devices.where((d) => !d.isFromScan)) {
              _checkReachability(d);
            }
            notifyListeners();
          },
        );
  }

  void addManualDevice(String host, int port, {String name = 'ESP32-P4'}) {
    final device = Esp32Device(
      name: name,
      host: host,
      port: port,
      id: '$host:$port',
      isFromScan: true,
    );
    if (_upsertDevice(device)) {
      notifyListeners();
    }
    _checkReachability(device);
  }

  Future<void> _loadSavedConnectedDevices() {
    return _loadSavedDevicesFuture ??= () async {
      try {
        final devices = await _deviceStore.loadDevices();
        var changed = false;
        for (final device in devices) {
          _savedConnectedDeviceIds.add(device.id);
          // Saved devices are historical (not from current scan)
          final histDevice = device.copyWith(isFromScan: false);
          changed = _upsertDevice(histDevice) || changed;
        }
        if (changed && !_disposed) notifyListeners();
      } catch (e) {
        debugPrint('[AppState] Load connected devices failed: $e');
      }
    }();
  }

  /// Check if a device is reachable by attempting a TCP connection.
  /// Tries port 80 first (camera), then 8080 (web config).
  Future<void> _checkReachability(Esp32Device device) async {
    bool reachable = false;
    try {
      final socket = await Socket.connect(
        device.host,
        80,
        timeout: const Duration(seconds: 2),
      );
      socket.destroy();
      reachable = true;
    } catch (_) {
      // Port 80 failed, try 8080
      try {
        final socket = await Socket.connect(
          device.host,
          8080,
          timeout: const Duration(seconds: 2),
        );
        socket.destroy();
        reachable = true;
      } catch (_) {
        reachable = false;
      }
    }

    final index = _devices.indexWhere((d) => d.id == device.id);
    if (index >= 0 && _devices[index].isReachable != reachable) {
      _devices[index] = _devices[index].copyWith(isReachable: reachable);
      if (!_disposed) notifyListeners();
    }
  }

  bool _upsertDevice(Esp32Device device) {
    final index = _devices.indexWhere((d) => d.id == device.id);
    if (index < 0) {
      _devices.add(device);
      _sortDevices();
      return true;
    }

    // Merge: preserve isFromScan=true if either existing or new is from scan
    final existing = _devices[index];
    final merged = existing.copyWith(
      isFromScan: existing.isFromScan || device.isFromScan,
      isReachable: device.isReachable,
    );
    _devices[index] = merged;
    _sortDevices();
    return true;
  }

  /// Sort devices: newly scanned first, then historical (saved) devices.
  /// Within each group, maintain insertion order.
  void _sortDevices() {
    _devices.sort((a, b) {
      // Newly scanned devices come first
      if (a.isFromScan != b.isFromScan) {
        return a.isFromScan ? -1 : 1;
      }
      return 0;
    });
  }

  // ── Logging ──

  void _addLog(String message) {
    _logs.add(LogEntry(message: message));
    if (_logs.length > _maxLogs) {
      _logs.removeAt(0);
    }
    // Defer notifyListeners to avoid crash when widget tree is locked (e.g., during dispose)
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!_disposed) notifyListeners();
    });
  }

  // ── Connection ──

  Future<void> connectToDevice(Esp32Device device) async {
    if (_isConnecting) return;
    _isConnecting = true;
    _connectionError = null;
    _latestFrame = null;
    _latestText = '';
    _deviceStatus = '';
    _logs.clear();
    notifyListeners();

    _addLog('Connecting to ${device.host}:81...');
    try {
      await _httpService.connect(device);
      await _deviceStore.saveConnectedDevice(device);
      _savedConnectedDeviceIds.add(device.id);
      _upsertDevice(device);
      _addLog('Connected to ${device.name}');
      // Fetch camera info on connect (may fail on headless boards — non-fatal)
      try {
        final info = await _httpService.fetchCameraInfo();
        _deviceStatus = 'Camera connected';
        _addLog('Camera info received');
        if (info['cameras'] is List && (info['cameras'] as List).isNotEmpty) {
          final cam = (info['cameras'] as List).first as Map<String, dynamic>;
          final res = cam['currentResolution'] as Map<String, dynamic>?;
          if (res != null) {
            final resStr =
                '${res['width']}x${res['height']} @ ${cam['currentFrameRate']}fps';
            _latestText = resStr;
            _addLog('Resolution: $resStr');
          }
          final fmt = cam['pixelFormat'] ?? 'unknown';
          _addLog('Pixel format: $fmt');
        }
      } catch (_) {
        _addLog('Failed to fetch camera info');
      }
      fetchSettings();
      _startDetectionPolling();
      _isConnecting = false;
      notifyListeners();
    } catch (e) {
      _connectionError = 'Connection failed: $e';
      _isConnecting = false;
      _addLog('Connection failed: $e');
      notifyListeners();
    }
  }

  void disconnect() {
    _stopDetectionPolling();
    _httpService.disconnect();
    _isConnecting = false;  // Reset in case of stale state
    _latestFrame = null;
    _latestText = '';
    _deviceStatus = '';
    _personCount = 0;
    _maxConfidence = 0.0;
    _detectionEnabled = false;
    _fps = 0.0;
    _fpsTimestamps.clear();
    _wifiEnabled = false;
    _ssid = '';
    _password = '';
    _volume = 60;
    _camStreamEnabled = false;
    _settingsLoading = false;
    _addLog('Disconnected');
    _notifySafe();
  }

  /// Clear all saved connected devices and remove them from the UI.
  Future<void> clearConnectedDevices() async {
    await _deviceStore.clear();
    _savedConnectedDeviceIds.clear();
    _devices.clear();
    _addLog('Cleared connection history');
    notifyListeners();
  }

  /// Web-only connect (port 8080) for settings + audio without camera stream.
  Future<void> connectToDeviceWeb(Esp32Device device) async {
    if (_isConnecting) return;
    _isConnecting = true;
    _connectionError = null;
    notifyListeners();

    _addLog('Web connect to ${device.host}:8080...');
    try {
      await _httpService.connectWeb(device);
      await _deviceStore.saveConnectedDevice(device);
      _savedConnectedDeviceIds.add(device.id);
      _upsertDevice(device);
      _addLog('Web connected to ${device.name}');
    } catch (e) {
      _connectionError = e.toString();
      _addLog('Web connect failed: $e');
      _isConnecting = false;
      notifyListeners();
      return;
    }
    _isConnecting = false;
    notifyListeners();
  }

  /// Schedule a safe deferred notifyListeners.
  void _notifySafe() {
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!_disposed) notifyListeners();
    });
  }

  // ── Detection Info Polling ──

  void _startDetectionPolling() {
    _detectionPollTimer?.cancel();
    _pollDetectionInfo();
    _detectionPollTimer = Timer.periodic(
      const Duration(seconds: 3),
      (_) => _pollDetectionInfo(),
    );
  }

  void _stopDetectionPolling() {
    _detectionPollTimer?.cancel();
    _detectionPollTimer = null;
  }

  Future<void> _pollDetectionInfo() async {
    try {
      final info = await _httpService.getDetectionInfo();
      _personCount = (info['person_count'] as num?)?.toInt() ?? 0;
      _maxConfidence = (info['max_confidence'] as num?)?.toDouble() ?? 0.0;
      _detectionEnabled = (info['detection_enabled'] as bool?) ?? false;
    } catch (_) {
      // Silently ignore poll errors
    }
    _notifySafe();
  }

  // ── Actions: Save & Settings ──

  /// Capture and return a single JPEG image frame.
  Future<Uint8List?> saveImage() async {
    try {
      return await _httpService.captureImage();
    } catch (e) {
      debugPrint('[AppState] Save image failed: $e');
      return null;
    }
  }

  /// Capture and return raw binary image data.
  Future<Uint8List?> saveRaw() async {
    try {
      return await _httpService.captureRaw();
    } catch (e) {
      debugPrint('[AppState] Save raw failed: $e');
      return null;
    }
  }

  /// Set JPEG compression quality (1-100).
  Future<void> setQuality(int quality) async {
    _addLog('Setting JPEG quality to $quality');
    try {
      await _httpService.setQuality(quality);
      _addLog('JPEG quality set to $quality');
    } catch (e) {
      _addLog('Set quality failed: $e');
      debugPrint('[AppState] Set quality failed: $e');
    }
  }

  // ── Settings (from /api/status) ──

  /// Fetch device settings (WiFi, volume, etc.).
  Future<void> fetchSettings() async {
    _settingsLoading = true;
    notifyListeners();
    try {
      final s = await _httpService.fetchSettings();
      _wifiEnabled = (s['wifi_en'] as num?)?.toInt() == 1;
      _ssid = (s['ssid'] as String?) ?? '';
      _password = (s['pass'] as String?) ?? '';
      _volume = (s['volume'] as num?)?.toInt() ?? 60;
      _camStreamEnabled = (s['cam_stream'] as num?)?.toInt() == 1;
      _addLog('Settings loaded');
    } catch (e) {
      _addLog('Failed to load settings: $e');
    }
    _settingsLoading = false;
    notifyListeners();
  }

  /// Save settings (WiFi, volume) to device.
  Future<void> updateSettings({
    bool? wifiEnabled,
    String? ssid,
    String? password,
    int? volume,
  }) async {
    _addLog('Saving settings...');
    try {
      await _httpService.updateSettings(
        wifiEnabled: wifiEnabled,
        ssid: ssid,
        password: password,
        volume: volume,
      );
      _addLog('Settings saved successfully');
    } catch (e) {
      _addLog('Save settings failed: $e');
    }
  }

  /// Factory reset device.
  Future<void> factoryReset() async {
    _addLog('Factory reset...');
    try {
      await _httpService.factoryReset();
      _addLog('Factory reset OK, device rebooting');
    } catch (e) {
      _addLog('Factory reset failed: $e');
    }
  }

  /// Toggle camera stream on the device.
  Future<void> toggleCameraStream(bool enable) async {
    _addLog('${enable ? "Starting" : "Stopping"} camera stream...');
    try {
      await _httpService.toggleCameraStream(enable);
      _camStreamEnabled = enable;
      _addLog('Camera stream ${enable ? "started" : "stopped"}');
    } catch (e) {
      _addLog('Toggle camera stream failed: $e');
    }
  }

  // ── Message Handling ──

  void _onMessage(Esp32Message message) {
    switch (message.type) {
      case MessageType.frame:
        if (message.data is Uint8List) {
          final data = message.data as Uint8List;
          final now = DateTime.now();
          if (now.difference(_lastFrameTime) >= _frameInterval) {
            _lastFrameTime = now;
            _latestFrame = data;
            // Client-side FPS: track frame timestamps
            final nowMs = now.millisecondsSinceEpoch;
            _fpsTimestamps.add(nowMs);
            _fpsTimestamps.removeWhere((t) => nowMs - t > _fpsWindowMs);
            _fps = _fpsTimestamps.length / (_fpsWindowMs / 1000.0);
            notifyListeners();
          }
        } else if (message.data is String) {
          try {
            _latestFrame = _base64Decode(message.data as String);
            notifyListeners();
          } catch (_) {}
        }
        break;
      case MessageType.text:
        _latestText = message.data.toString();
        _addLog('Info: ${message.data}');
        notifyListeners();
        break;
      case MessageType.status:
        _deviceStatus = message.data.toString();
        notifyListeners();
        break;
    }
  }

  Uint8List _base64Decode(String data) {
    final cleaned = data.contains(',')
        ? data.split(',').last.trim()
        : data.trim();
    return base64Decode(cleaned);
  }

  @override
  void dispose() {
    _disposed = true;
    _discovery.dispose();
    _httpService.dispose();
    super.dispose();
  }
}
