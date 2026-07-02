import 'package:flutter/material.dart';

import 'providers/app_state.dart';
import 'screens/home_screen.dart';

void main() {
  WidgetsFlutterBinding.ensureInitialized();
  final appState = AppState();
  runApp(Esp32P4App(state: appState));
}

/// InheritedWidget to provide AppState through the widget tree.
class AppStateScope extends InheritedWidget {
  final AppState state;

  const AppStateScope({super.key, required this.state, required super.child});

  static AppState of(BuildContext context) {
    final scope = context.dependOnInheritedWidgetOfExactType<AppStateScope>();
    assert(scope != null, 'No AppStateScope found in context');
    return scope!.state;
  }

  @override
  bool updateShouldNotify(AppStateScope oldWidget) => oldWidget.state != state;
}

class Esp32P4App extends StatelessWidget {
  final AppState state;

  const Esp32P4App({super.key, required this.state});

  @override
  Widget build(BuildContext context) {
    return AppStateScope(
      state: state,
      child: MaterialApp(
        title: 'ESP32-P4 Viewer',
        debugShowCheckedModeBanner: false,
        theme: ThemeData(
          colorSchemeSeed: Colors.indigo,
          useMaterial3: true,
          brightness: Brightness.light,
        ),
        darkTheme: ThemeData(
          colorSchemeSeed: Colors.indigo,
          useMaterial3: true,
          brightness: Brightness.dark,
        ),
        themeMode: ThemeMode.system,
        home: const SelectionArea(child: HomeScreen()),
      ),
    );
  }
}
