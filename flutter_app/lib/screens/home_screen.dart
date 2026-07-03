import 'package:flutter/material.dart';

import '../main.dart';
import '../models/esp32_device.dart';
import '../providers/app_state.dart';
import '../widgets/device_card.dart';
import 'settings_screen.dart';
import 'camera_screen.dart';

/// Home screen: shows discovered devices and allows connection.
class HomeScreen extends StatefulWidget {
  const HomeScreen({super.key});

  @override
  State<HomeScreen> createState() => _HomeScreenState();
}

class _HomeScreenState extends State<HomeScreen> {
  final _ipController = TextEditingController();

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
          ListenableBuilder(
            listenable: _state,
            builder: (context, _) => IconButton(
              icon: const Icon(Icons.delete_sweep),
              tooltip: 'Clear history',
              onPressed:
                  _state.savedConnectedDeviceIds.isNotEmpty
                      ? _showClearHistoryDialog
                      : null,
            ),
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
                ..._buildDeviceSections(_state),
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

  Future<void> _connectToDeviceWeb(Esp32Device device) async {
    print('[HomeScreen] 🔗 Web connecting to ${device.host}:8080...');
    await _state.connectToDeviceWeb(device);
    print('[HomeScreen] connectToDeviceWeb done, isConnected=${_state.isConnected}, mounted=$mounted');
    if (_state.isConnected && mounted) {
      print('[HomeScreen] 🎤 Navigating to SettingsScreen');
      Navigator.of(context).push(
        MaterialPageRoute(builder: (_) => const SettingsScreen()),
      );
    } else if (_state.connectionError != null && mounted) {
      print('[HomeScreen] ⚠️ Error: ${_state.connectionError}');
      ScaffoldMessenger.of(
        context,
      ).showSnackBar(SnackBar(content: Text(_state.connectionError!)));
    } else {
      print('[HomeScreen] ⚠️ Not navigating: isConnected=${_state.isConnected}, mounted=$mounted, error=${_state.connectionError}');
    }
  }

  void _showAddDeviceDialog() {
    showDialog(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('Add Device'),
        content: TextField(
          controller: _ipController,
          decoration: const InputDecoration(
            labelText: 'Hostname or IP',
            hintText: 'esp-web.local or esp-web-XXXXXX.local',
            border: OutlineInputBorder(),
          ),
          keyboardType: TextInputType.url,
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.pop(context),
            child: const Text('Cancel'),
          ),
          FilledButton(
            onPressed: () {
              final host = _ipController.text.trim();
              if (host.isNotEmpty) {
                _state.addManualDevice(host, 80);
              }
              Navigator.pop(context);
            },
            child: const Text('Add'),
          ),
        ],
      ),
    );
  }

  void _showClearHistoryDialog() {
    showDialog(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Text('Clear Connection History'),
        content: const Text(
          'Remove all saved devices? You can re-discover them by scanning again.',
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.pop(ctx),
            child: const Text('Cancel'),
          ),
          FilledButton(
            onPressed: () {
              _state.clearConnectedDevices();
              Navigator.pop(ctx);
            },
            style: FilledButton.styleFrom(
              backgroundColor: Theme.of(context).colorScheme.error,
            ),
            child: const Text('Clear All'),
          ),
        ],
      ),
    );
  }

  List<Widget> _buildDeviceSections(AppState state) {
    final scanned = state.devices.where((d) => d.isFromScan).toList();
    final historical = state.devices.where((d) => !d.isFromScan).toList();
    final widgets = <Widget>[];

    if (scanned.isNotEmpty) {
      widgets.add(
        Padding(
          padding: const EdgeInsets.only(bottom: 8),
          child: Row(
            children: [
              Icon(Icons.wifi_find, size: 18, color: Theme.of(context).colorScheme.primary),
              const SizedBox(width: 6),
              Text(
                'Scanned Devices',
                style: Theme.of(context).textTheme.titleSmall?.copyWith(
                  color: Theme.of(context).colorScheme.primary,
                  fontWeight: FontWeight.w600,
                ),
              ),
              const SizedBox(width: 8),
              Text(
                '${scanned.length}',
                style: Theme.of(context).textTheme.labelSmall?.copyWith(
                  color: Theme.of(context).colorScheme.primary,
                ),
              ),
            ],
          ),
        ),
      );
      for (final device in scanned) {
        widgets.add(_buildDeviceCard(device, state));
      }
    }

    if (historical.isNotEmpty) {
      if (scanned.isNotEmpty) {
        widgets.add(const Divider(height: 32));
      }
      widgets.add(
        Padding(
          padding: const EdgeInsets.only(bottom: 8),
          child: Row(
            children: [
              Icon(Icons.history, size: 18, color: Colors.grey[500]),
              const SizedBox(width: 6),
              Text(
                'History',
                style: Theme.of(context).textTheme.titleSmall?.copyWith(
                  color: Colors.grey[600],
                  fontWeight: FontWeight.w600,
                ),
              ),
              const SizedBox(width: 8),
              Text(
                '${historical.length}',
                style: Theme.of(context).textTheme.labelSmall?.copyWith(
                  color: Colors.grey[500],
                ),
              ),
            ],
          ),
        ),
      );
      for (final device in historical) {
        widgets.add(_buildDeviceCard(device, state));
      }
    }

    return widgets;
  }

  Widget _buildDeviceCard(Esp32Device device, AppState state) {
    return DeviceCard(
      device: device,
      isConnected: state.connectedDevice?.id == device.id,
      isConnecting: state.isConnecting,
      onConnect: () => _connectToDevice(device),
      onConnectWeb: () => _connectToDeviceWeb(device),
    );
  }
}
