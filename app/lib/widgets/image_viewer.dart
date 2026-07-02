import 'dart:typed_data';

import 'package:flutter/material.dart';

/// Displays a raw JPEG image frame from bytes.
class ImageViewer extends StatelessWidget {
  final Uint8List imageBytes;

  const ImageViewer({super.key, required this.imageBytes});

  @override
  Widget build(BuildContext context) {
    return Image.memory(
      imageBytes,
      fit: BoxFit.contain,
      width: double.infinity,
      height: double.infinity,
      filterQuality: FilterQuality.low,
      gaplessPlayback: true,
    );
  }
}
