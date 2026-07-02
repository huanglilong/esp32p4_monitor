import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import '../models/esp32_device.dart';

/// Discovers ESP32-P4 devices on the local network.
class DeviceDiscovery {
  RawDatagramSocket? _udpSocket;
  bool _isRunning = false;

  StreamController<Esp32Device>? _controller;

  static const List<String> _serviceTypes = ['_esp32p4._tcp', '_http._tcp'];
  static final InternetAddress _mdnsAddr = InternetAddress('224.0.0.251');
  static const int _mdnsPort = 5353;

  bool get isRunning => _isRunning;

  /// Start mDNS discovery for ESP32-P4 devices.
  Stream<Esp32Device> startDiscovery({
    Duration timeout = const Duration(seconds: 20),
  }) {
    if (_isRunning) {
      return _controller!.stream;
    }

    _isRunning = true;
    _controller = StreamController<Esp32Device>.broadcast();

    _performDiscovery(timeout);

    return _controller!.stream;
  }

  Future<void> _performDiscovery(Duration timeout) async {
    print(
      '[DeviceDiscovery] 🔍 Starting mDNS discovery (timeout: ${timeout.inSeconds}s)...',
    );
    try {
      _udpSocket = await RawDatagramSocket.bind(
        InternetAddress.anyIPv4,
        0,
        reuseAddress: true,
      );
      _udpSocket!.broadcastEnabled = true;

      // Join multicast group (required on Linux to receive mDNS responses)
      try {
        _udpSocket!.joinMulticast(_mdnsAddr);
      } catch (e) {
        print('[DeviceDiscovery] ⚠️ joinMulticast failed: $e');
      }

      // Set up listener BEFORE sending queries — ESP32 responds in ~ms
      int rawPackets = 0;
      _udpSocket!.listen((event) {
        if (event == RawSocketEvent.read) {
          final packet = _udpSocket!.receive();
          if (packet != null) {
            rawPackets++;
            final device = _parseMdnsResponse(packet.data, packet.address);
            if (device != null && !_controller!.isClosed) {
              print(
                '[DeviceDiscovery] ✅ mDNS found: ${device.name} @ ${device.address}',
              );
              _controller!.add(device);
            }
          }
        }
      });

      // Send mDNS queries — repeat 3 times with 1s delay for reliability (UDP)
      for (int round = 0; round < 3; round++) {
        for (final serviceType in _serviceTypes) {
          final queryPacket = _buildMdnsQuery(serviceType);
          _udpSocket!.send(queryPacket, _mdnsAddr, _mdnsPort);
        }
        if (round < 2) await Future.delayed(const Duration(seconds: 1));
      }
      print('[DeviceDiscovery]   mDNS queries sent (3 rounds, recv $rawPackets raw packets)');
    } catch (e) {
      print('[DeviceDiscovery] ⚠️ mDNS failed: $e (manual add still works)');
    }

    Future.delayed(timeout, () {
      print('[DeviceDiscovery] ⏱️ Discovery finished');
      _stopDiscovery();
    });
  }

  /// Build a minimal mDNS query for the given service type.
  Uint8List _buildMdnsQuery(String serviceType) {
    final bytes = <int>[
      0x00, 0x00, // Transaction ID
      0x00, 0x00, // Flags: standard query
      0x00, 0x01, // Questions: 1
      0x00, 0x00, // Answer RRs: 0
      0x00, 0x00, // Authority RRs: 0
      0x00, 0x00, // Additional RRs: 0
    ];

    // QNAME: {serviceType}.local
    for (final label in [serviceType, 'local']) {
      for (final part in label.split('.')) {
        bytes.add(part.length);
        bytes.addAll(part.codeUnits);
      }
    }
    bytes.addAll([
      0x00, // End of QNAME
      0x00, 0x0C, // QTYPE: PTR (12)
      0x00, 0x01, // QCLASS: IN
    ]);

    return Uint8List.fromList(bytes);
  }

  /// Read a DNS-encoded name (supports compression pointers) from [data] at [offset].
  /// Returns a tuple of (decodedName, newOffset).
  (String, int) _readDnsName(Uint8List data, int offset) {
    final labels = <String>[];
    bool jumped = false;
    int pos = offset;
    int jumpedPos = offset;

    while (pos < data.length) {
      final len = data[pos];
      if (len == 0) {
        pos++;
        break;
      }
      // Compression pointer (0xC0 | offset)
      if ((len & 0xC0) == 0xC0) {
        if (pos + 1 >= data.length) break;
        final ptr = ((len & 0x3F) << 8) | data[pos + 1];
        if (!jumped) {
          jumpedPos = pos + 2;
          jumped = true;
        }
        pos = ptr;
        continue;
      }
      // Regular label
      pos++;
      if (pos + len > data.length) break;
      labels.add(String.fromCharCodes(data.sublist(pos, pos + len)));
      pos += len;
    }

    final name = labels.join('.');
    return (name, jumped ? jumpedPos : pos);
  }

  /// Parse mDNS response for SRV record containing hostname + port info.
  Esp32Device? _parseMdnsResponse(Uint8List data, InternetAddress source) {
    try {
      if (data.length < 12) return null;
      int offset = 12; // Skip DNS header

      // Header fields
      final qdCount = (data[4] << 8) | data[5];
      final anCount = (data[6] << 8) | data[7];
      final nsCount = (data[8] << 8) | data[9];
      final arCount = (data[10] << 8) | data[11];
      final totalRecords = anCount + nsCount + arCount;

      // Skip question section
      for (int q = 0; q < qdCount; q++) {
        while (offset < data.length) {
          if (data[offset] == 0) { offset += 1; break; }
          if ((data[offset] & 0xC0) == 0xC0) { offset += 2; break; }
          offset += data[offset] + 1;
        }
        offset += 4; // QTYPE + QCLASS
      }

      // Scan all record sections for SRV (type 33)
      final typesFound = <int>[];
      for (int r = 0; r < totalRecords && offset + 10 < data.length; r++) {
        // Skip record name
        if ((data[offset] & 0xC0) == 0xC0) {
          offset += 2;
        } else {
          while (offset < data.length && data[offset] != 0) {
            offset += data[offset] + 1;
          }
          offset += 1;
        }

        if (offset + 8 > data.length) break;
        final type = (data[offset] << 8) | data[offset + 1];
        final dataLen = (data[offset + 8] << 8) | data[offset + 9];
        offset += 10;
        typesFound.add(type);

        if (type == 33 && dataLen >= 6 && offset + dataLen <= data.length) {
          // SRV: priority(2) + weight(2) + port(2) + target(name)
          final port = (data[offset + 4] << 8) | data[offset + 5];
          String hostname = source.address;
          try {
            final (name, _) = _readDnsName(data, offset + 6);
            if (name.isNotEmpty && name.contains('.')) {
              hostname = name;
            }
          } catch (_) {}
          return Esp32Device(
            name: 'ESP32-P4 ($hostname)',
            host: hostname,
            port: port,
            id: '$hostname:$port',
          );
        }
        offset += dataLen;
      }

      // No SRV found — debug log
      print('[DeviceDiscovery] ⚠️ No SRV in response '
          '(QD=$qdCount AN=$anCount NS=$nsCount AR=$arCount, '
          'types=${typesFound.map((t) => '0x${t.toRadixString(16)}').toList()})');
    } catch (e) {
      print('[DeviceDiscovery] ⚠️ Parse error: $e');
    }
    return null;
  }

  void _stopDiscovery() {
    _isRunning = false;
    try {
      _udpSocket?.close();
    } catch (_) {}
    _udpSocket = null;
    final controller = _controller;
    if (controller != null && !controller.isClosed) {
      controller.close();
    }
  }

  Future<void> stop() async {
    _stopDiscovery();
  }

  void dispose() {
    stop();
  }
}
