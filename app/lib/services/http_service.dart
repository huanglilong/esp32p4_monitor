import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import '../models/esp32_device.dart';
import '../models/esp32_message.dart';

/// HTTP-based service for ESP32-P4 communication.
/// Uses Dart HttpClient which handles chunked transfer encoding automatically.
class Esp32HttpService {
  Esp32Device? _connectedDevice;

  HttpClient? _client;
  StreamSubscription? _responseSub;
  Timer? _reconnectTimer;
  int _reconnectAttempts = 0;
  static const int _maxReconnectAttempts = 5;
  static const Duration _reconnectDelay = Duration(seconds: 3);
  static const String TAG = '[Esp32HttpService]';

  final StreamController<Esp32Message> _messageController =
      StreamController<Esp32Message>.broadcast();
  final StreamController<Esp32Device> _connectionController =
      StreamController<Esp32Device>.broadcast();

  // MJPEG parser state
  Uint8List _mjpegBuf = Uint8List(0);
  int _frameCount = 0;

  Stream<Esp32Message> get messageStream => _messageController.stream;
  Stream<Esp32Device> get connectionStream => _connectionController.stream;
  Esp32Device? get connectedDevice => _connectedDevice;
  bool get isConnected => _client != null;

  Future<void> connect(Esp32Device device) async {
    print('$TAG 🔌 Connecting to ${device.host}:81/stream...');
    disconnect();

    _connectedDevice = device;
    _reconnectAttempts = 0;

    try {
      // Verify device
      await _fetchCameraInfo();

      // HttpClient handles HTTP/1.1 + chunked encoding automatically
      _client = HttpClient()
        ..connectionTimeout = const Duration(seconds: 5)
        ..findProxy = (url) => 'DIRECT';
      final req = await _client!.getUrl(
        Uri.parse('http://${device.host}:81/stream'),
      );
      final resp = await req.close();

      _mjpegBuf = Uint8List(0);
      _frameCount = 0;

      // resp is already dechunked by HttpClient — clean MJPEG data
      _responseSub = resp.listen(
        _onData,
        onError: (e) {
          print('$TAG ⚠️ Stream error: $e');
          _reconnect();
        },
        onDone: () {
          print('$TAG ⚠️ Stream ended');
          _reconnect();
        },
      );
    } catch (e) {
      print('$TAG ❌ Failed: $e');
      _connectedDevice = null;
      _client?.close();
      _client = null;
      _scheduleReconnect(device);
      rethrow;
    }

    _connectionController.add(device);
    print('$TAG ✅ Stream connected');
  }

  /// Parse MJPEG from clean (already dechunked by HttpClient) data.
  void _onData(List<int> chunk) {
    _mjpegBuf = Uint8List.fromList([..._mjpegBuf, ...chunk]);

    while (true) {
      // Find Content-Type: image/jpeg
      final ct = _find(_mjpegBuf, 'Content-Type: image/jpeg'.codeUnits, 0);
      if (ct < 0) break;

      // Find end of headers \r\n\r\n
      final he = _find(_mjpegBuf, '\r\n\r\n'.codeUnits, ct);
      if (he < 0) break;

      // Parse Content-Length
      final hdr = String.fromCharCodes(_mjpegBuf.sublist(ct, he));
      final li = hdr.indexOf('Content-Length: ');
      if (li < 0) break;
      final le = hdr.indexOf('\r\n', li);
      final len = int.tryParse(
        hdr.substring(li + 'Content-Length: '.length, le),
      );
      if (len == null || len <= 0) break;

      final imgStart = he + 4;
      if (imgStart + len > _mjpegBuf.length) break;

      final jpeg = _mjpegBuf.sublist(imgStart, imgStart + len);

      // Validate JPEG header: FF D8 FF
      if (jpeg.length >= 3 &&
          jpeg[0] == 0xFF &&
          jpeg[1] == 0xD8 &&
          jpeg[2] == 0xFF) {
        _frameCount++;
        _messageController.add(
          Esp32Message(type: MessageType.frame, data: jpeg),
        );
      } else {
        print(
          '$TAG ⚠️ Bad JPEG frame #$_frameCount: '
          'first bytes=${jpeg.take(8).map((b) => b.toRadixString(16)).join(" ")}',
        );
      }

      // Advance past this frame
      _mjpegBuf = _mjpegBuf.sublist(imgStart + len);
    }
  }

  int _find(Uint8List buf, List<int> pat, int start) {
    if (start + pat.length > buf.length) return -1;
    for (int i = start; i <= buf.length - pat.length; i++) {
      bool ok = true;
      for (int j = 0; j < pat.length; j++) {
        if (buf[i + j] != pat[j]) {
          ok = false;
          break;
        }
      }
      if (ok) return i;
    }
    return -1;
  }

  void _reconnect() {
    _responseSub?.cancel();
    _responseSub = null;
    _client?.close(force: true);
    _client = null;
    if (_connectedDevice != null) {
      print('$TAG 🔄 Reconnecting...');
      Future.delayed(
        const Duration(seconds: 1),
        () => connect(_connectedDevice!).catchError((_) {}),
      );
    }
  }

  void disconnect() {
    _responseSub?.cancel();
    _responseSub = null;
    _client?.close(force: true);
    _client = null;
    _reconnectTimer?.cancel();
    _reconnectTimer = null;
    _connectedDevice = null;
    _mjpegBuf = Uint8List(0);
  }

  Future<Map<String, dynamic>> fetchCameraInfo() => _fetchCameraInfo();

  Future<Map<String, dynamic>> _fetchCameraInfo() async {
    final c = HttpClient()
      ..connectionTimeout = const Duration(seconds: 3)
      ..findProxy = (url) => 'DIRECT';
    try {
      final r = await c.getUrl(
        Uri.parse('http://${_connectedDevice!.host}:80/api/get_camera_info'),
      );
      final resp = await r.close();
      return jsonDecode(await resp.transform(utf8.decoder).join())
          as Map<String, dynamic>;
    } finally {
      c.close();
    }
  }

  /// Fetch detection info (person_count, max_confidence, detection_enabled).
  Future<Map<String, dynamic>> getDetectionInfo() async {
    final c = HttpClient()
      ..connectionTimeout = const Duration(seconds: 3)
      ..findProxy = (url) => 'DIRECT';
    try {
      final r = await c.getUrl(
        Uri.parse(
          'http://${_connectedDevice!.host}:80/api/get_detection_info',
        ),
      );
      final resp = await r.close();
      return jsonDecode(await resp.transform(utf8.decoder).join())
          as Map<String, dynamic>;
    } finally {
      c.close();
    }
  }

  /// Capture a single JPEG image (download, not from stream).
  Future<Uint8List> captureImage() async {
    return _capture('/api/capture_image?source=0');
  }

  /// Capture raw binary image data.
  Future<Uint8List> captureRaw() async {
    return _capture('/api/capture_binary?source=0');
  }

  Future<Uint8List> _capture(String path) async {
    final c = HttpClient()
      ..connectionTimeout = const Duration(seconds: 10)
      ..findProxy = (url) => 'DIRECT';
    try {
      final r = await c.getUrl(
        Uri.parse('http://${_connectedDevice!.host}:80$path'),
      );
      final resp = await r.close();
      return await resp.fold<Uint8List>(
        Uint8List(0),
        (p, chunk) => Uint8List.fromList([...p, ...chunk]),
      );
    } finally {
      c.close();
    }
  }

  /// Set camera JPEG quality (1-100) via raw socket.
  Future<void> setQuality(int quality) async {
    final q = quality.clamp(1, 100);
    final body = jsonEncode({'index': 0, 'image_format': 0, 'jpeg_quality': q});
    print('$TAG ⚙️ POST /api/set_camera_config $body');

    try {
      final socket = await Socket.connect(
        _connectedDevice!.host,
        80,
        timeout: const Duration(seconds: 5),
      );
      socket.write(
        'POST /api/set_camera_config HTTP/1.0\r\n'
        'Host: ${_connectedDevice!.host}\r\n'
        'Content-Type: application/json\r\n'
        'Content-Length: ${body.length}\r\n'
        '\r\n'
        '$body',
      );
      await socket.flush();
      final resp = await socket.cast<List<int>>().transform(utf8.decoder).first;
      socket.close();
      if (resp.contains('200 OK')) {
        print('$TAG ✅ Quality set to $q');
      } else {
        print('$TAG ⚠️ Response:\n$resp');
      }
    } catch (e) {
      print('$TAG ⚠️ Failed: $e');
    }
  }

  Esp32Device get _connectedOrThrow {
    final d = _connectedDevice;
    if (d == null) throw StateError('Not connected to any device');
    return d;
  }

  /// Fetch device settings (WiFi, volume, etc.) from /api/status.
  Future<Map<String, dynamic>> fetchSettings() async {
    final device = _connectedOrThrow;
    final c = HttpClient()
      ..connectionTimeout = const Duration(seconds: 5)
      ..findProxy = (url) => 'DIRECT';
    try {
      final r = await c.getUrl(
        Uri.parse('http://${device.host}:8080/api/status'),
      );
      final resp = await r.close();
      return jsonDecode(await resp.transform(utf8.decoder).join())
          as Map<String, dynamic>;
    } finally {
      c.close();
    }
  }

  /// Update device settings via POST /api/settings.
  Future<void> updateSettings({
    bool? wifiEnabled,
    String? ssid,
    String? password,
    int? volume,
  }) async {
    final body = <String, dynamic>{};
    if (wifiEnabled != null) body['wifi_en'] = wifiEnabled ? 1 : 0;
    if (ssid != null) body['ssid'] = ssid;
    if (password != null) body['pass'] = password;
    if (volume != null) body['volume'] = volume;

    final device = _connectedOrThrow;
    final json = jsonEncode(body);
    print('$TAG ⚙️ POST /api/settings $json');

    try {
      final socket = await Socket.connect(
        device.host,
        8080,
        timeout: const Duration(seconds: 5),
      );
      socket.write(
        'POST /api/settings HTTP/1.0\r\n'
        'Host: ${device.host}:8080\r\n'
        'Content-Type: application/json\r\n'
        'Content-Length: ${json.length}\r\n'
        '\r\n'
        '$json',
      );
      await socket.flush();
      final resp = await socket.cast<List<int>>().transform(utf8.decoder).first;
      socket.close();
      if (resp.contains('200 OK')) {
        print('$TAG ✅ Settings saved');
      } else {
        print('$TAG ⚠️ Response:\n$resp');
      }
    } catch (e) {
      print('$TAG ⚠️ Failed: $e');
      rethrow;
    }
  }

  /// Factory reset via POST /api/factory_reset.
  Future<void> factoryReset() async {
    final device = _connectedOrThrow;
    print('$TAG ⚙️ POST /api/factory_reset');
    try {
      final socket = await Socket.connect(
        device.host,
        8080,
        timeout: const Duration(seconds: 5),
      );
      socket.write(
        'POST /api/factory_reset HTTP/1.0\r\n'
        'Host: ${device.host}:8080\r\n'
        '\r\n',
      );
      await socket.flush();
      final resp = await socket.cast<List<int>>().transform(utf8.decoder).first;
      socket.close();
      if (resp.contains('200 OK')) {
        print('$TAG ✅ Factory reset executed');
      } else {
        print('$TAG ⚠️ Response:\n$resp');
      }
    } catch (e) {
      print('$TAG ⚠️ Failed: $e');
      rethrow;
    }
  }

  /// Toggle camera stream via POST /api/camera_stream.
  Future<void> toggleCameraStream(bool enable) async {
    final device = _connectedOrThrow;
    final body = jsonEncode({'enable': enable ? 1 : 0});
    print('$TAG ⚙️ POST /api/camera_stream $body');
    try {
      final socket = await Socket.connect(
        device.host,
        8080,
        timeout: const Duration(seconds: 5),
      );
      socket.write(
        'POST /api/camera_stream HTTP/1.0\r\n'
        'Host: ${device.host}:8080\r\n'
        'Content-Type: application/json\r\n'
        'Content-Length: ${body.length}\r\n'
        '\r\n'
        '$body',
      );
      await socket.flush();
      final resp = await socket.cast<List<int>>().transform(utf8.decoder).first;
      socket.close();
      if (resp.contains('200 OK')) {
        print('$TAG ✅ Camera stream ${enable ? "started" : "stopped"}');
      } else {
        print('$TAG ⚠️ Response:\n$resp');
      }
    } catch (e) {
      print('$TAG ⚠️ Failed: $e');
      rethrow;
    }
  }

  // ==================== Audio API (port 8080) ====================

  Future<Map<String, dynamic>> _audioGet(String path) async {
    final c = HttpClient()
      ..connectionTimeout = const Duration(seconds: 5)
      ..findProxy = (url) => 'DIRECT';
    try {
      final r = await c.getUrl(
        Uri.parse('http://${_connectedDevice!.host}:8080$path'),
      );
      final resp = await r.close();
      return jsonDecode(await resp.transform(utf8.decoder).join())
          as Map<String, dynamic>;
    } finally {
      c.close();
    }
  }

  /// GET /api/audio/record_start — Start audio recording to SD card.
  Future<Map<String, dynamic>> audioRecordStart() => _audioGet('/api/audio/record_start');

  /// GET /api/audio/record_stop — Stop recording, returns filename and size.
  Future<Map<String, dynamic>> audioRecordStop() => _audioGet('/api/audio/record_stop');

  /// GET /api/audio/record_status — Get recording status (seconds, bytes).
  Future<Map<String, dynamic>> audioRecordStatus() => _audioGet('/api/audio/record_status');

  /// GET /api/audio/list — List MP3 files on SD card.
  Future<Map<String, dynamic>> audioList() => _audioGet('/api/audio/list');

  /// GET /api/audio/play?file=xxx.mp3 — Play a recording.
  Future<Map<String, dynamic>> audioPlay(String filename) =>
      _audioGet('/api/audio/play?file=${Uri.encodeComponent(filename)}');

  /// GET /api/audio/stop — Stop playback.
  Future<Map<String, dynamic>> audioStop() => _audioGet('/api/audio/stop');

  void _scheduleReconnect(Esp32Device device) {
    if (_reconnectAttempts >= _maxReconnectAttempts) {
      print('$TAG ❌ Max reconnect attempts');
      return;
    }
    _reconnectAttempts++;
    _reconnectTimer = Timer(
      _reconnectDelay,
      () => connect(device).catchError((_) {}),
    );
  }

  void dispose() {
    disconnect();
    _messageController.close();
    _connectionController.close();
  }
}
