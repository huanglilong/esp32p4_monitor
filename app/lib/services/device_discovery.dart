import 'dart:async';
import 'dart:convert';
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
    Duration timeout = const Duration(seconds: 30),
    List<Esp32Device> prioritizedDevices = const [],
  }) {
    if (_isRunning) {
      return _controller!.stream;
    }

    _isRunning = true;
    _controller = StreamController<Esp32Device>.broadcast();

    _performDiscovery(timeout, prioritizedDevices);

    return _controller!.stream;
  }

  Future<void> _performDiscovery(
    Duration timeout,
    List<Esp32Device> prioritizedDevices,
  ) async {
    print(
      '[DeviceDiscovery] 🔍 Starting discovery (timeout: ${timeout.inSeconds}s)...',
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

      // Send mDNS queries for all known service types
      for (final serviceType in _serviceTypes) {
        print('[DeviceDiscovery]   Sending mDNS query for $serviceType');
        final queryPacket = _buildMdnsQuery(serviceType);
        _udpSocket!.send(queryPacket, _mdnsAddr, _mdnsPort);
      }

      _udpSocket!.listen((event) {
        if (event == RawSocketEvent.read) {
          final packet = _udpSocket!.receive();
          if (packet != null) {
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
    } catch (e) {
      print('[DeviceDiscovery] ⚠️ mDNS failed: $e (manual add still works)');
    }

    if (prioritizedDevices.isNotEmpty) {
      print('[DeviceDiscovery] 🔎 Checking saved connected devices first...');
      await _probePrioritizedDevices(prioritizedDevices);
    }

    print('[DeviceDiscovery] 🔎 Starting HTTP port probe...');
    _probeCommonPorts()
        .then((_) => print('[DeviceDiscovery] ✅ HTTP probe complete'))
        .catchError((e) => print('[DeviceDiscovery] ⚠️ HTTP probe error: $e'));

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

  /// Parse mDNS response for SRV record containing port info.
  Esp32Device? _parseMdnsResponse(Uint8List data, InternetAddress source) {
    try {
      int offset = 12; // Skip DNS header

      // Skip question section
      while (offset < data.length) {
        if (data[offset] == 0) {
          offset += 5; // null label + QTYPE + QCLASS
          break;
        }
        offset += data[offset] + 1;
      }

      // Scan answer records for SRV record (type 33)
      while (offset + 10 < data.length) {
        // Name pointer (2 bytes, compressed)
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

        if (type == 33 && dataLen >= 6 && offset + dataLen <= data.length) {
          // SRV record: priority(2) + weight(2) + port(2) + target
          final port = (data[offset + 4] << 8) | data[offset + 5];
          return Esp32Device(
            name: 'ESP32-P4 (${source.address})',
            host: source.address,
            port: port,
            id: '${source.address}:$port',
          );
        }
        offset += dataLen;
      }
    } catch (_) {}
    return null;
  }

  /// Check whether [addr] is in a private IPv4 range.
  bool _isPrivateIPv4(InternetAddress addr) {
    if (addr.type != InternetAddressType.IPv4) return false;
    final parts = addr.address.split('.');
    if (parts.length != 4) return false;
    final b0 = int.tryParse(parts[0]) ?? -1;
    final b1 = int.tryParse(parts[1]) ?? -1;
    // 10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16
    return b0 == 10 ||
        (b0 == 172 && b1 >= 16 && b1 <= 31) ||
        (b0 == 192 && b1 == 168);
  }

  /// Detect the local subnet by opening a TCP connection to an external host.
  /// Used as a fallback when NetworkInterface.list() returns no private IPs.
  Future<String?> _detectLocalSubnet() async {
    try {
      final socket = await Socket.connect(
        '1.1.1.1',
        80,
        timeout: const Duration(seconds: 2),
      );
      final localAddr = socket.address.address;
      socket.close();
      if (_isPrivateIPv4(InternetAddress(localAddr))) {
        return localAddr.substring(0, localAddr.lastIndexOf('.'));
      }
    } catch (_) {}
    return null;
  }

  /// Probe common HTTP ports to find ESP32 devices.
  /// Uses Dart HttpClient (same as the app's connection code) for reliability.
  Future<void> _probeCommonPorts() async {
    int probed = 0;
    final subnets = <String>{};
    try {
      final interfaces = await NetworkInterface.list();
      for (final interface in interfaces) {
        for (final addr in interface.addresses) {
          if (!_isPrivateIPv4(addr)) continue;
          subnets.add(addr.address.substring(0, addr.address.lastIndexOf('.')));
        }
      }
    } catch (_) {}

    // Fallback: detect subnet via external connection
    // (NetworkInterface.list() may return empty on Linux without permissions)
    if (subnets.isEmpty) {
      final subnet = await _detectLocalSubnet();
      if (subnet != null) {
        print('[DeviceDiscovery] 📡 Fallback subnet: $subnet.0/24');
        subnets.add(subnet);
      }
    }

    for (final subnet in subnets) {
      print('[DeviceDiscovery] 📡 Scanning subnet $subnet.0/24...');

      final targets = <String>[];
      for (int i = 1; i <= 254; i++) {
        targets.add('$subnet.$i');
      }
      probed += targets.length;

      final client = HttpClient()
        ..connectionTimeout = const Duration(seconds: 3)
        ..findProxy = (url) => 'DIRECT';
      try {
        final semaphore = <Future<void>>[];
        for (final host in targets) {
          semaphore.add(_tryHttpProbe(client, host));
          if (semaphore.length >= 20) {
            await Future.wait(semaphore);
            semaphore.clear();
          }
        }
        if (semaphore.isNotEmpty) {
          await Future.wait(semaphore);
        }
      } finally {
        client.close(force: true);
      }
    }

    print('[DeviceDiscovery] 🔎 HTTP probe: scanned $probed endpoints');
  }

  Future<void> _tryHttpProbe(HttpClient client, String host) async {
    try {
      final req = await client.getUrl(
        Uri.parse('http://$host:80/api/get_camera_info'),
      );
      final resp = await req.close();
      final response = await resp.transform(utf8.decoder).join();

      if (response.contains('currentResolution') ||
          response.contains('cameras') ||
          response.contains('ESP32') ||
          response.contains('esp32')) {
        if (!_controller!.isClosed) {
          print('[DeviceDiscovery] ✅ ESP32 found at $host');
          _controller!.add(
            Esp32Device(
              name: 'ESP32-P4 ($host)',
              host: host,
              port: 80,
              id: '$host:80',
            ),
          );
        }
      }
    } catch (_) {}
  }

  Future<void> _probePrioritizedDevices(List<Esp32Device> devices) async {
    final client = HttpClient()
      ..connectionTimeout = const Duration(seconds: 1)
      ..findProxy = (url) => 'DIRECT';
    try {
      for (final device in devices) {
        await _tryHttpProbe(client, device.host);
      }
    } finally {
      client.close(force: true);
    }
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
