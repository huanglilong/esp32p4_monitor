import 'package:flutter_test/flutter_test.dart';
import 'package:esp32p4_app/models/esp32_device.dart';
import 'package:esp32p4_app/models/esp32_message.dart';

void main() {
  group('Esp32Device', () {
    test('should create device with correct fields', () {
      final device = Esp32Device(
        name: 'ESP32-CAM-1',
        host: '192.168.1.100',
        port: 80,
        id: '192.168.1.100:80',
      );

      expect(device.name, 'ESP32-CAM-1');
      expect(device.host, '192.168.1.100');
      expect(device.port, 80);
      expect(device.address, '192.168.1.100:80');
    });

    test('equality should be based on id', () {
      final a = Esp32Device(name: 'A', host: '1', port: 80, id: '1:80');
      final b = Esp32Device(name: 'B', host: '1', port: 80, id: '1:80');
      expect(a, equals(b));
    });

    test('different ids should not be equal', () {
      final a = Esp32Device(name: 'A', host: '1', port: 80, id: '1:80');
      final b = Esp32Device(name: 'B', host: '2', port: 80, id: '2:80');
      expect(a, isNot(equals(b)));
    });

    test('should round-trip through JSON', () {
      final original = Esp32Device(
        name: 'ESP32-CAM-1',
        host: '192.168.1.100',
        port: 80,
        id: '192.168.1.100:80',
      );

      final restored = Esp32Device.fromJson(original.toJson());

      expect(restored.name, original.name);
      expect(restored.host, original.host);
      expect(restored.port, original.port);
      expect(restored.id, original.id);
    });
  });

  group('Esp32Message', () {
    test('should parse frame message from JSON', () {
      final json = {
        'type': 'frame',
        'data': 'base64data',
        'timestamp': 1000000,
      };
      final message = Esp32Message.fromJson(json);

      expect(message.type, MessageType.frame);
      expect(message.data, 'base64data');
      expect(message.timestamp.millisecondsSinceEpoch, 1000000);
    });

    test('should parse text message from JSON', () {
      final json = {'type': 'text', 'data': 'Temperature: 28.5°C'};
      final message = Esp32Message.fromJson(json);

      expect(message.type, MessageType.text);
      expect(message.data, 'Temperature: 28.5°C');
    });

    test('should round-trip through JSON', () {
      final original = Esp32Message(type: MessageType.text, data: 'Hello');
      final json = original.toJson();
      final restored = Esp32Message.fromJson(json);

      expect(restored.type, original.type);
      expect(restored.data, original.data);
    });
  });
}
