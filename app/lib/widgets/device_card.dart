import 'package:flutter/material.dart';

import '../models/esp32_device.dart';

/// Card widget showing an ESP32-P4 device with connect controls.
class DeviceCard extends StatelessWidget {
  final Esp32Device device;
  final bool isConnected;
  final bool isConnecting;
  final VoidCallback onConnect;

  const DeviceCard({
    super.key,
    required this.device,
    required this.isConnected,
    required this.isConnecting,
    required this.onConnect,
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
                  Text(
                    device.name,
                    style: Theme.of(context).textTheme.titleMedium?.copyWith(
                      fontWeight: FontWeight.w600,
                    ),
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
            FilledButton(
              onPressed: isConnecting ? null : (isConnected ? null : onConnect),
              child: isConnecting
                  ? const SizedBox(
                      width: 20,
                      height: 20,
                      child: CircularProgressIndicator(strokeWidth: 2),
                    )
                  : Text(isConnected ? 'Connected' : 'Connect'),
            ),
          ],
        ),
      ),
    );
  }
}
