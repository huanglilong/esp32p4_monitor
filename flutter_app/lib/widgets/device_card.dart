import 'package:flutter/material.dart';

import '../models/esp32_device.dart';

/// Card widget showing an ESP32-P4 device with connect controls.
class DeviceCard extends StatelessWidget {
  final Esp32Device device;
  final bool isConnected;
  final bool isConnecting;
  final VoidCallback onConnect;
  final VoidCallback? onConnectWeb;  // Settings/Audio (port 8080 only)

  const DeviceCard({
    super.key,
    required this.device,
    required this.isConnected,
    required this.isConnecting,
    required this.onConnect,
    this.onConnectWeb,
  });

  @override
  Widget build(BuildContext context) {
    return Card(
      margin: const EdgeInsets.only(bottom: 12),
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Row(
          children: [
            Container(
              width: 48,
              height: 48,
              decoration: BoxDecoration(
                color: (isConnected ? Colors.green : Colors.grey).withAlpha(30),
                borderRadius: BorderRadius.circular(12),
              ),
              child: Icon(
                isConnected ? Icons.camera_alt : Icons.videocam,
                color: isConnected ? Colors.green : Colors.grey[600],
              ),
            ),
            const SizedBox(width: 16),
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Row(
                    children: [
                      Flexible(
                        child: Text(
                          device.name,
                          style: Theme.of(context).textTheme.titleMedium?.copyWith(
                            fontWeight: FontWeight.w600,
                          ),
                          overflow: TextOverflow.ellipsis,
                        ),
                      ),
                      const SizedBox(width: 8),
                      _StatusBadge(device: device, isConnected: isConnected),
                    ],
                  ),
                  const SizedBox(height: 4),
                  Text(
                    device.address,
                    style: Theme.of(context).textTheme.bodySmall?.copyWith(
                      color: Colors.grey[600],
                      fontFamily: 'monospace',
                    ),
                  ),
                ],
              ),
            ),
            Row(
              mainAxisSize: MainAxisSize.min,
              children: [
                if (onConnectWeb != null)
                  Padding(
                    padding: const EdgeInsets.only(right: 4),
                    child: FilledButton.icon(
                      onPressed: isConnecting ? null : onConnectWeb,
                      icon: const Icon(Icons.settings, size: 18),
                      label: const Text('Settings', style: TextStyle(fontSize: 14)),
                    ),
                  ),
                FilledButton.icon(
                  onPressed: isConnecting ? null : onConnect,
                  icon: isConnecting
                      ? const SizedBox(width: 18, height: 18, child: CircularProgressIndicator(strokeWidth: 2))
                      : const Icon(Icons.videocam, size: 18),
                  label: Text(isConnecting ? '' : 'Camera', style: const TextStyle(fontSize: 14)),
                ),
              ],
            ),
          ],
        ),
      ),
    );
  }
}

/// Shows device reachability and scan/history status as a badge.
class _StatusBadge extends StatelessWidget {
  final Esp32Device device;
  final bool isConnected;

  const _StatusBadge({required this.device, required this.isConnected});

  @override
  Widget build(BuildContext context) {
    if (isConnected) {
      return _chip(Icons.link, 'Connected', Colors.green);
    }

    if (device.isReachable) {
      return _chip(Icons.wifi, 'Reachable', Colors.blue);
    }

    // Not reachable — show as offline/historical
    if (device.isFromScan) {
      return _chip(Icons.wifi_off, 'Offline', Colors.orange);
    }
    return _chip(Icons.history, 'History', Colors.grey);
  }

  Widget _chip(IconData icon, String label, Color color) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 2),
      decoration: BoxDecoration(
        color: color.withAlpha(25),
        borderRadius: BorderRadius.circular(8),
        border: Border.all(color: color.withAlpha(80)),
      ),
      child: Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          Icon(icon, size: 12, color: color),
          const SizedBox(width: 3),
          Text(
            label,
            style: TextStyle(fontSize: 10, color: color, fontWeight: FontWeight.w600),
          ),
        ],
      ),
    );
  }
}
