import 'dart:io';

import 'package:esp32p4_app/main.dart';
import 'package:esp32p4_app/providers/app_state.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:path_provider_platform_interface/path_provider_platform_interface.dart';
import 'package:plugin_platform_interface/plugin_platform_interface.dart';

class _FakePathProviderPlatform extends Fake
    with MockPlatformInterfaceMixin
    implements PathProviderPlatform {
  @override
  Future<String?> getApplicationDocumentsPath() async =>
      Directory.systemTemp.createTempSync('esp32p4_app_test_').path;
}

void main() {
  setUpAll(() {
    PathProviderPlatform.instance = _FakePathProviderPlatform();
  });

  testWidgets('shows home screen', (WidgetTester tester) async {
    final state = AppState();
    addTearDown(state.dispose);

    await tester.pump();
    await tester.pumpWidget(Esp32P4App(state: state));

    expect(find.text('ESP32-P4 Viewer'), findsOneWidget);
  });
}
