/// ULog binary format parser — extracts camera JPEG frames from .ulg files.
///
/// Ported from tools/ulog_extract_frames.py.
/// Handles PX4 ULog format including o_size_no_padding convention
/// where trailing _padding bytes are omitted from DATA messages.

import 'dart:io';
import 'dart:typed_data';

/// A single extracted camera frame.
class UlogFrame {
  final int frameIndex;
  final int width;
  final int height;
  final int jpegSize;
  final int format;
  final int timestamp;
  final Uint8List jpegData;

  const UlogFrame({
    required this.frameIndex,
    required this.width,
    required this.height,
    required this.jpegSize,
    required this.format,
    required this.timestamp,
    required this.jpegData,
  });
}

/// Result of ULog parsing.
class UlogParseResult {
  final List<UlogFrame> frames;
  final String? error;

  const UlogParseResult({required this.frames, this.error});

  bool get isSuccess => error == null;
  int get count => frames.length;
  int get totalBytes => frames.fold(0, (sum, f) => sum + f.jpegSize);
}

/// Parses ULog binary format to extract camera_frame JPEG data.
class UlogParser {
  static const _ulogMagic = [0x55, 0x4C, 0x6F, 0x67, 0x01, 0x12, 0x35, 0x01];

  /// Parse a ULog file and extract all camera_frame JPEG data.
  static UlogParseResult parseFile(String path) {
    final file = File(path);
    if (!file.existsSync()) {
      return const UlogParseResult(frames: [], error: 'File not found');
    }

    Uint8List data;
    try {
      data = file.readAsBytesSync();
    } catch (e) {
      return UlogParseResult(frames: [], error: 'Read failed: $e');
    }

    return parseBytes(data);
  }

  /// Parse ULog binary data and extract all camera_frame JPEG data.
  static UlogParseResult parseBytes(Uint8List data) {
    if (data.length < 16) {
      return UlogParseResult(frames: [], error: 'File too small (${data.length} bytes)');
    }

    // Check for valid ULog magic header
    bool hasUlogMagic = true;
    for (int i = 0; i < 8; i++) {
      if (data[i] != _ulogMagic[i]) {
        hasUlogMagic = false;
        break;
      }
    }

    if (!hasUlogMagic) {
      final hexHeader = data.take(8).map((b) => b.toRadixString(16).padLeft(2, '0')).join(' ');
      return UlogParseResult(
        frames: [],
        error: 'Not a valid ULog file (header: $hexHeader, expected: 55 4c 6f 67 01 12 35 01)',
      );
    }

    // Parse definitions section — find camera_frame subscription
    final subscriptions = <int, String>{};
    int pos = 16;

    while (pos + 3 <= data.length) {
      final msgSize = _readUint16(data, pos);
      final msgType = data[pos + 2];

      if (msgSize > 200000) break;

      if (msgType == 0x41) {
        // 'A' — subscription message
        final msgId = _readUint16(data, pos + 4);
        final nameBytes = data.sublist(pos + 6, pos + 3 + msgSize);
        subscriptions[msgId] = String.fromCharCodes(nameBytes);
      }

      // Stop at first DATA or SYNC message
      if (msgType == 0x44 || msgType == 0x53) break;

      final nextPos = pos + 3 + msgSize;
      if (nextPos <= pos) break;
      pos = nextPos;
    }

    // Find camera_frame msg_id
    int? cameraFrameMsgId;
    for (final entry in subscriptions.entries) {
      if (entry.value == 'camera_frame') {
        cameraFrameMsgId = entry.key;
        break;
      }
    }

    if (cameraFrameMsgId == null) {
      return const UlogParseResult(
        frames: [],
        error: 'No camera_frame topic found in ULog file',
      );
    }

    // Scan entire file for camera_frame DATA messages
    final frames = <UlogFrame>[];
    int scanPos = 16;

    while (scanPos + 3 <= data.length) {
      final msgSize = _readUint16(data, scanPos);
      final msgType = data[scanPos + 2];

      if (msgSize > 200000) break;

      final nextPos = scanPos + 3 + msgSize;
      if (nextPos > data.length || nextPos <= scanPos) break;

      if (msgType == 0x44 && msgSize >= 5) {
        // 'D' — data message
        final msgId = _readUint16(data, scanPos + 3);

        if (msgId == cameraFrameMsgId) {
          // camera_frame payload (after msg_id, starting at scanPos+5):
          //   uint64_t timestamp     (offset 0, 8 bytes)
          //   uint32_t frame_index   (offset 8, 4 bytes)
          //   uint16_t width         (offset 12, 2 bytes)
          //   uint16_t height        (offset 14, 2 bytes)
          //   uint16_t jpeg_size     (offset 16, 2 bytes)
          //   uint8_t  format        (offset 18, 1 byte)
          //   uint8_t  jpeg_data[]   (offset 19, up to jpeg_size bytes)
          final payloadStart = scanPos + 5;
          final payloadEnd = scanPos + 3 + msgSize;
          final payloadLength = payloadEnd - payloadStart;

          if (payloadLength < 19) {
            scanPos = nextPos;
            continue;
          }

          final timestamp = _readUint64(data, payloadStart);
          final frameIndex = _readUint32(data, payloadStart + 8);
          final width = _readUint16(data, payloadStart + 12);
          final height = _readUint16(data, payloadStart + 14);
          final jpegSize = _readUint16(data, payloadStart + 16);
          final format = data[payloadStart + 18];

          if (jpegSize > 0 && 19 + jpegSize <= payloadLength) {
            final jpegStart = payloadStart + 19;
            final jpegData = data.sublist(jpegStart, jpegStart + jpegSize);

            // Verify JPEG magic (FFD8)
            if (jpegData.length >= 2 &&
                jpegData[0] == 0xFF &&
                jpegData[1] == 0xD8) {
              frames.add(UlogFrame(
                frameIndex: frameIndex,
                width: width,
                height: height,
                jpegSize: jpegSize,
                format: format,
                timestamp: timestamp,
                jpegData: Uint8List.fromList(jpegData),
              ));
            }
          }
        }
      }

      scanPos = nextPos;
    }

    return UlogParseResult(frames: frames);
  }

  static int _readUint16(Uint8List data, int offset) =>
      data[offset] | (data[offset + 1] << 8);

  static int _readUint32(Uint8List data, int offset) =>
      data[offset] |
      (data[offset + 1] << 8) |
      (data[offset + 2] << 16) |
      (data[offset + 3] << 24);

  static int _readUint64(Uint8List data, int offset) {
    int value = 0;
    for (int i = 0; i < 8; i++) {
      value |= (data[offset + i] << (8 * i));
    }
    return value;
  }
}
