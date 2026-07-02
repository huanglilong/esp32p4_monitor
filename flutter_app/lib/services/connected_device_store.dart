import 'dart:convert';
import 'dart:io';

import 'package:path_provider/path_provider.dart';

import '../models/esp32_device.dart';

/// Persists successfully connected ESP32 devices as a JSON file.
class ConnectedDeviceStore {
  static const String _fileName = 'connected_devices.json';
  static const int _maxDevices = 20;

  Future<File> _file() async {
    final directory = await getApplicationDocumentsDirectory();
    return File('${directory.path}/$_fileName');
  }

  Future<List<Esp32Device>> loadDevices() async {
    final file = await _file();
    if (!await file.exists()) return [];

    final data = jsonDecode(await file.readAsString());
    final rawDevices = data is Map<String, dynamic> ? data['devices'] : data;
    if (rawDevices is! List) return [];

    return rawDevices
        .whereType<Map<String, dynamic>>()
        .map(Esp32Device.fromJson)
        .where((device) => device.host.isNotEmpty)
        .toList();
  }

  Future<void> saveConnectedDevice(Esp32Device device) async {
    final devices = await loadDevices();
    devices.removeWhere((saved) => saved.id == device.id);
    devices.insert(0, device);

    final file = await _file();
    await file.parent.create(recursive: true);
    final payload = {
      'updatedAt': DateTime.now().toIso8601String(),
      'devices': devices.take(_maxDevices).map((d) => d.toJson()).toList(),
    };
    await file.writeAsString(
      const JsonEncoder.withIndent('  ').convert(payload),
    );
  }
}
