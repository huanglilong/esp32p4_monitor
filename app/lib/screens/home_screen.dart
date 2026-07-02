import 'package:flutter/material.dart';

import '../main.dart';
import '../models/esp32_device.dart';
import '../providers/app_state.dart';
import '../widgets/device_card.dart';
import 'audio_screen.dart';
import 'camera_screen.dart';

/// Home screen: shows discovered devices and allows connection.
class HomeScreen extends StatefulWidget {
  const HomeScreen({super.key});

  @override
  State<HomeScreen> createState() => _HomeScreenState();
}

class _HomeScreenState extends State<HomeScreen> {
  final _ipController = TextEditingController();
  final _portController = TextEditingController(text: '80');

  AppState get _state => AppStateScope.of(context);

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) {
      _state.startDiscovery();
    });
  }

  @override
  void dispose() {
    _ipController.dispose();
    _portController.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('ESP32-P4 Viewer'),
        backgroundColor: Theme.of(context).colorScheme.primaryContainer,
        actions: [
          IconButton(
            icon: const Icon(Icons.refresh),
            tooltip: 'Scan network',
            onPressed: () => _state.startDiscovery(),
          ),
          IconButton(
            icon: const Icon(Icons.add),
            tooltip: 'Add device manually',
            onPressed: _showAddDeviceDialog,
          ),
          if (_state.isConnected)
            IconButton(
              icon: const Icon(Icons.mic),
              tooltip: 'Audio Recorder',
              onPressed: _openAudioScreen,
            ),
        ],
      ),
      body: ListenableBuilder(
        listenable: _state,
        builder: (context, _) {
          if (_state.devices.isEmpty && !_state.isDiscovering) {
            return Center(
              child: Column(
                mainAxisSize: MainAxisSize.min,
                children: [
                  Icon(Icons.wifi_off, size: 64, color: Colors.grey[400]),
                  const SizedBox(height: 16),
                  Text(
                    'No devices found',
                    style: Theme.of(context).textTheme.titleLarge,
                  ),
                  const SizedBox(height: 8),
                  Text(
                    'Make sure your ESP32-P4 is on the same network',
                    style: Theme.of(
                      context,
                    ).textTheme.bodyMedium?.copyWith(color: Colors.grey[600]),
                  ),
                  const SizedBox(height: 24),
                  FilledButton.icon(
                    onPressed: () => _state.startDiscovery(),
                    icon: const Icon(Icons.search),
                    label: const Text('Scan Again'),
                  ),
                ],
              ),
            );
          }

          return RefreshIndicator(
            onRefresh: () async => _state.startDiscovery(),
            child: ListView(
              padding: const EdgeInsets.all(16),
              children: [
                if (_state.isDiscovering)
                  const Padding(
                    padding: EdgeInsets.only(bottom: 16),
                    child: LinearProgressIndicator(),
                  ),
                ..._state.devices.map(
                  (device) => DeviceCard(
                    device: device,
                    isConnected: _state.connectedDevice?.id == device.id,
                    isConnecting: _state.isConnecting,
                    onConnect: () => _connectToDevice(device),
                  ),
                ),
              ],
            ),
          );
        },
      ),
    );
  }

  Future<void> _connectToDevice(Esp32Device device) async {
    await _state.connectToDevice(device);

    if (_state.isConnected && mounted) {
      Navigator.of(context).push(
        MaterialPageRoute(
          builder: (_) => const CameraScreen(),
        ),
      );
    } else if (_state.connectionError != null && mounted) {
      ScaffoldMessenger.of(
        context,
      ).showSnackBar(SnackBar(content: Text(_state.connectionError!)));
    }
  }

  void _openAudioScreen() {
    if (_state.isConnected) {
      Navigator.of(context).push(
        MaterialPageRoute(builder: (_) => const AudioScreen()),
      );
    }
  }

  void _showAddDeviceDialog() {
    showDialog(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('Add Device Manually'),
        content: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            TextField(
              controller: _ipController,
              decoration: const InputDecoration(
                labelText: 'IP Address',
                hintText: '192.168.1.100',
                border: OutlineInputBorder(),
              ),
              keyboardType: TextInputType.url,
            ),
            const SizedBox(height: 12),
            TextField(
              controller: _portController,
              decoration: const InputDecoration(
                labelText: 'Port',
                hintText: '80',
                border: OutlineInputBorder(),
              ),
              keyboardType: TextInputType.number,
            ),
          ],
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.pop(context),
            child: const Text('Cancel'),
          ),
          FilledButton(
            onPressed: () {
              final ip = _ipController.text.trim();
              final port = int.tryParse(_portController.text.trim()) ?? 80;
              if (ip.isNotEmpty) {
                _state.addManualDevice(ip, port);
              }
              Navigator.pop(context);
            },
            child: const Text('Add'),
          ),
        ],
      ),
    );
  }
}
