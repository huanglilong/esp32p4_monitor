/// Data message types received from ESP32-P4.
enum MessageType { frame, text, status }

/// Parsed message from an ESP32-P4 device.
class Esp32Message {
  final MessageType type;
  final dynamic data;
  final DateTime timestamp;

  Esp32Message({required this.type, required this.data, DateTime? timestamp})
    : timestamp = timestamp ?? DateTime.now();

  /// Parse a raw WebSocket/JSON message.
  factory Esp32Message.fromJson(Map<String, dynamic> json) {
    final type = switch (json['type'] as String? ?? '') {
      'frame' => MessageType.frame,
      'text' => MessageType.text,
      'status' => MessageType.status,
      _ => MessageType.text,
    };
    return Esp32Message(
      type: type,
      data: json['data'],
      timestamp: json['timestamp'] != null
          ? DateTime.fromMillisecondsSinceEpoch(json['timestamp'] as int)
          : DateTime.now(),
    );
  }

  Map<String, dynamic> toJson() => {
    'type': type.name,
    'data': data,
    'timestamp': timestamp.millisecondsSinceEpoch,
  };
}
