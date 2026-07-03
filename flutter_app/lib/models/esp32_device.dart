/// Represents an ESP32-P4 device discovered on the local network.
class Esp32Device {
  final String name;
  final String host;
  final int port;
  final String id;
  final DateTime discoveredAt;

  /// Whether this device was found during the current scan session.
  final bool isFromScan;

  /// Whether the device is reachable on the network (TCP connect check).
  final bool isReachable;

  Esp32Device({
    required this.name,
    required this.host,
    required this.port,
    required this.id,
    DateTime? discoveredAt,
    this.isFromScan = true,
    this.isReachable = false,
  }) : discoveredAt = discoveredAt ?? DateTime.now();

  String get address => '$host:$port';

  factory Esp32Device.fromJson(Map<String, dynamic> json) {
    final host = json['host'] as String? ?? '';
    final port = json['port'] as int? ?? 80;
    return Esp32Device(
      name: json['name'] as String? ?? 'ESP32-P4 ($host)',
      host: host,
      port: port,
      id: json['id'] as String? ?? '$host:$port',
      discoveredAt: DateTime.tryParse(json['discoveredAt'] as String? ?? ''),
      isFromScan: json['isFromScan'] as bool? ?? false,
      isReachable: json['isReachable'] as bool? ?? false,
    );
  }

  Map<String, dynamic> toJson() => {
    'name': name,
    'host': host,
    'port': port,
    'id': id,
    'discoveredAt': discoveredAt.toIso8601String(),
    'isFromScan': isFromScan,
    'isReachable': isReachable,
  };

  /// Create a copy with updated fields.
  Esp32Device copyWith({
    String? name,
    String? host,
    int? port,
    String? id,
    DateTime? discoveredAt,
    bool? isFromScan,
    bool? isReachable,
  }) {
    return Esp32Device(
      name: name ?? this.name,
      host: host ?? this.host,
      port: port ?? this.port,
      id: id ?? this.id,
      discoveredAt: discoveredAt ?? this.discoveredAt,
      isFromScan: isFromScan ?? this.isFromScan,
      isReachable: isReachable ?? this.isReachable,
    );
  }

  @override
  bool operator ==(Object other) =>
      identical(this, other) ||
      other is Esp32Device &&
          runtimeType == other.runtimeType &&
          id == other.id;

  @override
  int get hashCode => id.hashCode;

  @override
  String toString() => 'ESP32Device($name @ $address, scan=$isFromScan, reachable=$isReachable)';
}
