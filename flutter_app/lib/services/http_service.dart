import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import '../models/esp32_device.dart';
import '../models/esp32_message.dart';

/// HTTP-based service for ESP32-P4 communication.
/// Uses Dart HttpClient which handles chunked transfer encoding automatically.
/// Read a complete HTTP response from a raw socket. Handles ESP32 keep-alive
/// (server doesn't close connection) by reading until Content-Length bytes.
Future<String> _readHttpResponse(Socket socket) async {
  final completer = Completer<String>();
  final buf = StringBuffer();
  int? contentLength;
  int bodyStart = -1;
  int bodyBytes = 0;

  socket.cast<List<int>>().transform(utf8.decoder).listen(
    (chunk) {
      buf.write(chunk);
      if (contentLength == null) {
        final i = buf.toString().indexOf('\r\n\r\n');
        if (i >= 0) {
          // Parse Content-Length from headers
          final headers = buf.toString().substring(0, i);
          final clMatch = RegExp(r'Content-Length:\s*(\d+)', caseSensitive: false)
              .firstMatch(headers);
          if (clMatch != null) {
            contentLength = int.parse(clMatch.group(1)!);
            bodyStart = i + 4;
          }
        }
      }
      if (contentLength != null) {
        bodyBytes = buf.length - bodyStart!;
        if (bodyBytes >= contentLength!) {
          if (!completer.isCompleted) completer.complete(buf.toString());
        }
      }
    },
    onError: (e) { if (!completer.isCompleted) completer.completeError(e); },
    onDone: () { if (!completer.isCompleted) completer.complete(buf.toString()); },
    cancelOnError: true,
  );
  return completer.future.timeout(const Duration(seconds: 5));
}

class Esp32HttpService {
  Esp32Device? _connectedDevice;

  HttpClient? _client;
  StreamSubscription? _responseSub;
  Timer? _reconnectTimer;
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
  bool get isConnected => _connectedDevice != null;

  Future<void> connect(Esp32Device device) async {
    print('$TAG 🔌 Connecting to ${device.host}:81/stream...');
    disconnect();

    _connectedDevice = device;

    try {
      // Verify device — don't fail if camera info unavailable (headless/WIFI6)
      try {
        await _fetchCameraInfo();
      } catch (_) {
        print('$TAG ⚠️ Camera info unavailable — continuing (headless mode)');
      }

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
      print('$TAG ⚠️ Camera stream unavailable: $e');
      // Keep device reference for web API access (settings/audio on :8080)
      _client?.close();
      _client = null;
    }

    _connectionController.add(device);
    print('$TAG ✅ Connected to ${device.host}');
  }

  /// Connect to device for web API only (port 8080) — no camera stream needed.
  /// Use this for settings, audio recording, and playback on headless boards.
  Future<void> connectWeb(Esp32Device device) async {
    print('$TAG 🌐 Web connect to ${device.host}:8080...');
    disconnect();

    // Use raw Socket.connect() for reliability — HttpClient can fail with
    // EHOSTDOWN on macOS sandboxed apps even when the host is reachable.
    try {
      final socket = await Socket.connect(
        device.host,
        8080,
        timeout: const Duration(seconds: 5),
      );
      socket.write(
        'GET /api/status HTTP/1.0\r\n'
        'Host: ${device.host}:8080\r\n'
        '\r\n',
      );
      await socket.flush();
      final resp = await _readHttpResponse(socket);
      socket.close();
      if (!resp.contains('200 OK')) {
        throw Exception('Server returned non-200');
      }
    } catch (e) {
      print('$TAG ❌ Web connect failed: $e');
      rethrow;
    }

    _connectedDevice = device;
    _connectionController.add(device);
    print('$TAG ✅ Web connected to ${device.host}');
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
      final resp = await _readHttpResponse(socket);
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
    try {
      final socket = await Socket.connect(
        device.host,
        8080,
        timeout: const Duration(seconds: 5),
      );
      socket.write(
        'GET /api/status HTTP/1.0\r\n'
        'Host: ${device.host}:8080\r\n'
        '\r\n',
      );
      await socket.flush();
      final resp = await _readHttpResponse(socket);
      socket.close();
      // Find JSON body after headers (double \r\n)
      final bodyStart = resp.indexOf('\r\n\r\n');
      if (bodyStart < 0) throw Exception('No response body');
      return jsonDecode(resp.substring(bodyStart + 4)) as Map<String, dynamic>;
    } catch (e) {
      print('$TAG ❌ fetchSettings failed: $e');
      rethrow;
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
      final resp = await _readHttpResponse(socket);
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
      final resp = await _readHttpResponse(socket);
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
  /// Throws on failure (e.g. WiFi not connected).
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
      final resp = await _readHttpResponse(socket);
      socket.close();
      final bodyStart = resp.indexOf('\r\n\r\n');
      if (bodyStart < 0) throw Exception('No response body');
      final json = jsonDecode(resp.substring(bodyStart + 4)) as Map<String, dynamic>;
      if (json['ok'] != 1) throw Exception(json['error'] ?? 'Toggle failed');
      print('$TAG ✅ Camera stream ${enable ? "started" : "stopped"}');
    } catch (e) {
      print('$TAG ⚠️ Camera stream toggle failed: $e');
      rethrow;
    }
  }

  // ==================== Audio API (port 8080) ====================

  Future<Map<String, dynamic>> _audioGet(String path) async {
    final device = _connectedOrThrow;
    try {
      final socket = await Socket.connect(
        device.host,
        8080,
        timeout: const Duration(seconds: 5),
      );
      socket.write(
        'GET $path HTTP/1.0\r\n'
        'Host: ${device.host}:8080\r\n'
        '\r\n',
      );
      await socket.flush();
      final resp = await _readHttpResponse(socket);
      socket.close();
      final bodyStart = resp.indexOf('\r\n\r\n');
      if (bodyStart < 0) throw Exception('No response body');
      return jsonDecode(resp.substring(bodyStart + 4)) as Map<String, dynamic>;
    } catch (e) {
      print('$TAG ❌ Audio GET $path failed: $e');
      rethrow;
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

  // ==================== ULog API (port 8080) ====================

  /// GET /api/ulog/status — Get ULog recording status.
  Future<Map<String, dynamic>> ulogStatus() => _audioGet('/api/ulog/status');

  /// POST /api/ulog/start — Start ULog recording to SD card.
  Future<Map<String, dynamic>> ulogStart() async {
    final device = _connectedOrThrow;
    try {
      final socket = await Socket.connect(device.host, 8080, timeout: const Duration(seconds: 5));
      socket.write('POST /api/ulog/start HTTP/1.0\r\nHost: ${device.host}:8080\r\n\r\n');
      await socket.flush();
      final resp = await _readHttpResponse(socket);
      socket.close();
      final bodyStart = resp.indexOf('\r\n\r\n');
      if (bodyStart < 0) throw Exception('No response body');
      return jsonDecode(resp.substring(bodyStart + 4)) as Map<String, dynamic>;
    } catch (e) {
      print('$TAG ❌ ULog start failed: $e');
      rethrow;
    }
  }

  /// POST /api/ulog/stop — Stop ULog recording.
  Future<Map<String, dynamic>> ulogStop() async {
    final device = _connectedOrThrow;
    try {
      final socket = await Socket.connect(device.host, 8080, timeout: const Duration(seconds: 5));
      socket.write('POST /api/ulog/stop HTTP/1.0\r\nHost: ${device.host}:8080\r\n\r\n');
      await socket.flush();
      final resp = await _readHttpResponse(socket);
      socket.close();
      final bodyStart = resp.indexOf('\r\n\r\n');
      if (bodyStart < 0) throw Exception('No response body');
      return jsonDecode(resp.substring(bodyStart + 4)) as Map<String, dynamic>;
    } catch (e) {
      print('$TAG ❌ ULog stop failed: $e');
      rethrow;
    }
  }

  // ==================== File Manager API (port 8080) ====================

  /// GET /api/files/list?dir=/ — List files/directories on SD card.
  Future<Map<String, dynamic>> filesList([String dir = '/']) =>
      _audioGet('/api/files/list?dir=${Uri.encodeComponent(dir)}');

  /// GET /api/files/download?path=xxx — Download a file from SD card.
  Future<Uint8List> filesDownload(String path) async {
    final device = _connectedOrThrow;
    Socket? socket;
    try {
      socket = await Socket.connect(
        device.host,
        8080,
        timeout: const Duration(seconds: 10),
      );
      socket.write(
        'GET /api/files/download?path=${Uri.encodeComponent(path)} HTTP/1.0\r\n'
        'Host: ${device.host}:8080\r\n'
        '\r\n',
      );
      await socket.flush();

      // Read entire raw response via onDone (HTTP/1.0 server closes connection when done)
      final rawCompleter = Completer<Uint8List>();
      final rawBuf = BytesBuilder();
      int? contentLength;
      int headerEnd = -1;

      socket.cast<List<int>>().listen(
        (chunk) {
          rawBuf.add(chunk);

          // Try to parse Content-Length from headers once we have them
          if (contentLength == null) {
            final data = rawBuf.toBytes();
            // Find \r\n\r\n
            for (int i = 0; i < data.length - 3; i++) {
              if (data[i] == 13 && data[i+1] == 10 && data[i+2] == 13 && data[i+3] == 10) {
                headerEnd = i + 4;
                final headers = utf8.decode(data.sublist(0, i));
                final clMatch = RegExp(r'Content-Length:\s*(\d+)', caseSensitive: false)
                    .firstMatch(headers);
                if (clMatch != null) {
                  contentLength = int.parse(clMatch.group(1)!);
                }
                break;
              }
            }
          }

          // If we know Content-Length, complete as soon as body is fully received
          if (contentLength != null && headerEnd >= 0 && !rawCompleter.isCompleted) {
            final totalReceived = rawBuf.toBytes().length;
            if (totalReceived >= headerEnd + contentLength!) {
              rawCompleter.complete(rawBuf.toBytes());
            }
          }
        },
        onError: (e) {
          print('$TAG ❌ filesDownload stream error: $e');
          if (!rawCompleter.isCompleted) rawCompleter.complete(Uint8List(0));
        },
        onDone: () {
          if (!rawCompleter.isCompleted) {
            final bytes = rawBuf.toBytes();
            print('$TAG 📥 filesDownload onDone: ${bytes.length} bytes');
            rawCompleter.complete(bytes);
          }
        },
        cancelOnError: true,
      );

      final raw = await rawCompleter.future; // No timeout — wait for onDone or Content-Length
      if (raw.isEmpty) {
        print('$TAG ❌ filesDownload: empty response for $path');
        return Uint8List(0);
      }

      print('$TAG 📥 filesDownload total: ${raw.length} bytes');

      // Find header/body boundary (\r\n\r\n)
      int bodyStart = -1;
      for (int i = 0; i < raw.length - 3; i++) {
        if (raw[i] == 13 && raw[i+1] == 10 && raw[i+2] == 13 && raw[i+3] == 10) {
          bodyStart = i + 4;
          break;
        }
      }
      if (bodyStart < 0) return Uint8List(0);

      final headersStr = utf8.decode(raw.sublist(0, bodyStart - 4));
      print('$TAG 📥 filesDownload headers: ${headersStr.replaceAll('\r\n', ' | ')}');
      print('$TAG 📥 filesDownload body first 20 bytes: ${raw.sublist(bodyStart, (bodyStart + 20).clamp(0, raw.length)).map((b) => b.toRadixString(16).padLeft(2, '0')).join(' ')}');

      // Detect chunked encoding: either explicit header or body starts with hex chunk size
      // IMPORTANT: chunked takes priority over Content-Length (RFC 7230)
      final isChunkedHeader = RegExp(r'Transfer-Encoding:\s*chunked', caseSensitive: false)
          .hasMatch(headersStr);
      final bodyLooksChunked = _isChunkedBody(raw, bodyStart);

      if (isChunkedHeader || bodyLooksChunked) {
        print('$TAG 📥 filesDownload: dechunking (header=$isChunkedHeader, detected=$bodyLooksChunked)');
        return _dechunk(raw, bodyStart);
      }

      // Content-Length only if NOT chunked
      final clMatch = RegExp(r'Content-Length:\s*(\d+)', caseSensitive: false)
          .firstMatch(headersStr);
      if (clMatch != null) {
        final contentLength = int.parse(clMatch.group(1)!);
        if (bodyStart + contentLength <= raw.length) {
          return Uint8List.fromList(raw.sublist(bodyStart, bodyStart + contentLength));
        }
      }

      // Fallback: return all bytes after headers (HTTP/1.0 style)
      return Uint8List.fromList(raw.sublist(bodyStart));
    } catch (e) {
      print('$TAG ❌ File download failed: $e');
      return Uint8List(0);
    } finally {
      socket?.close();
    }
  }

  /// Check if body starts with a chunked encoding pattern (hex size + \r\n).
  static bool _isChunkedBody(Uint8List data, int bodyStart) {
    if (bodyStart >= data.length) return false;
    // Find first \r\n after bodyStart
    int lineEnd = -1;
    for (int i = bodyStart; i < data.length - 1; i++) {
      if (data[i] == 13 && data[i + 1] == 10) {
        lineEnd = i;
        break;
      }
    }
    if (lineEnd <= bodyStart) return false;
    // Check if the line is a valid hex number
    final line = String.fromCharCodes(data.sublist(bodyStart, lineEnd));
    final hexPart = line.split(';').first.trim();
    if (hexPart.isEmpty || hexPart.length > 8) return false;
    return int.tryParse(hexPart, radix: 16) != null;
  }

  /// Decode chunked transfer encoding body.
  static Uint8List _dechunk(Uint8List data, int start) {
    final result = BytesBuilder();
    int pos = start;

    while (pos < data.length) {
      // Read chunk size line (hex number + \r\n)
      int lineEnd = -1;
      for (int i = pos; i < data.length - 1; i++) {
        if (data[i] == 13 && data[i + 1] == 10) {
          lineEnd = i;
          break;
        }
      }
      if (lineEnd < 0) break;

      final sizeStr = utf8.decode(data.sublist(pos, lineEnd)).trim();
      // Ignore chunk extensions (after semicolon)
      final chunkSize = int.parse(sizeStr.split(';').first, radix: 16);
      if (chunkSize == 0) break; // terminal chunk

      final chunkStart = lineEnd + 2;
      final chunkEnd = chunkStart + chunkSize;
      if (chunkEnd + 2 > data.length) break; // incomplete

      result.add(data.sublist(chunkStart, chunkEnd));
      pos = chunkEnd + 2; // skip trailing \r\n
    }

    return result.toBytes();
  }

  /// POST /api/files/delete — Delete a file from SD card.
  Future<Map<String, dynamic>> filesDelete(String path) async {
    final device = _connectedOrThrow;
    final body = jsonEncode({'path': path});
    Socket? socket;
    try {
      socket = await Socket.connect(
        device.host,
        8080,
        timeout: const Duration(seconds: 5),
      );
      socket.write(
        'POST /api/files/delete HTTP/1.0\r\n'
        'Host: ${device.host}:8080\r\n'
        'Content-Type: application/json\r\n'
        'Content-Length: ${body.length}\r\n'
        '\r\n'
        '$body',
      );
      await socket.flush();
      final resp = await _readHttpResponse(socket);
      final bodyStart = resp.indexOf('\r\n\r\n');
      if (bodyStart < 0) throw Exception('No response body');
      return jsonDecode(resp.substring(bodyStart + 4)) as Map<String, dynamic>;
    } catch (e) {
      print('$TAG ❌ File delete failed: $e');
      rethrow;
    } finally {
      socket?.close();
    }
  }

  void dispose() {
    disconnect();
    _messageController.close();
    _connectionController.close();
  }
}
