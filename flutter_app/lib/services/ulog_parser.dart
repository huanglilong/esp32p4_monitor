/// ULog binary format parser — extracts camera JPEG frames and AAC audio from .ulg files.
///
/// Handles PX4 ULog format including o_size_no_padding convention
/// where trailing _padding bytes are omitted from DATA messages.
/// Supports:
///   - camera_frame_chunk (chunked JPEG frames, reassembled by frame_index)
///   - audio_frame (AAC-ADTS audio frames)
///   - camera_frame (legacy non-chunked format)

import 'dart:io';
import 'dart:typed_data';

/// A single extracted camera frame (reassembled from chunks if needed).
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

/// A single extracted AAC audio frame.
class UlogAudioFrame {
  final int frameIndex;
  final int timestamp;
  final int sampleRate;
  final int channel;
  final int bitsPerSample;
  final int aacSize;
  final Uint8List aacData;

  const UlogAudioFrame({
    required this.frameIndex,
    required this.timestamp,
    required this.sampleRate,
    required this.channel,
    required this.bitsPerSample,
    required this.aacSize,
    required this.aacData,
  });
}

/// Result of ULog parsing.
class UlogParseResult {
  final List<UlogFrame> frames;
  final List<UlogAudioFrame> audioFrames;
  final String? error;

  const UlogParseResult({required this.frames, this.audioFrames = const [], this.error});

  bool get isSuccess => error == null;
  int get count => frames.length;
  int get totalBytes => frames.fold(0, (sum, f) => sum + f.jpegSize);
  int get audioFrameCount => audioFrames.length;
  int get audioTotalBytes => audioFrames.fold(0, (sum, f) => sum + f.aacSize);
}

/// Parses ULog binary format to extract camera JPEG frames and AAC audio.
class UlogParser {
  static const _ulogMagic = [0x55, 0x4C, 0x6F, 0x67, 0x01, 0x12, 0x35, 0x01];

  /// Parse a ULog file and extract all camera JPEG frames + AAC audio.
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

  /// Parse ULog binary data and extract all camera JPEG frames + AAC audio.
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

    // Parse definitions section — find topic subscriptions
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

    // Find topic msg_ids
    int? cameraFrameMsgId;       // legacy camera_frame
    int? cameraFrameChunkMsgId;  // camera_frame_chunk
    int? audioFrameMsgId;        // audio_frame
    for (final entry in subscriptions.entries) {
      if (entry.value == 'camera_frame') {
        cameraFrameMsgId = entry.key;
      } else if (entry.value == 'camera_frame_chunk') {
        cameraFrameChunkMsgId = entry.key;
      } else if (entry.value == 'audio_frame') {
        audioFrameMsgId = entry.key;
      }
    }

    if (cameraFrameMsgId == null && cameraFrameChunkMsgId == null && audioFrameMsgId == null) {
      return const UlogParseResult(
        frames: [],
        error: 'No camera_frame, camera_frame_chunk, or audio_frame topic found in ULog file',
      );
    }

    // Scan entire file for DATA messages
    final frames = <UlogFrame>[];
    final audioFrames = <UlogAudioFrame>[];

    // For chunk reassembly: frame_index -> list of (chunk_index, chunk_data)
    final chunkMap = <int, Map<int, Uint8List>>{};

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
        final payloadStart = scanPos + 5;
        final payloadEnd = scanPos + 3 + msgSize;
        final payloadLength = payloadEnd - payloadStart;

        if (msgId == cameraFrameMsgId && payloadLength >= 19) {
          // Legacy camera_frame format (non-chunked):
          //   uint64_t timestamp     (offset 0, 8 bytes)
          //   uint32_t frame_index   (offset 8, 4 bytes)
          //   uint16_t width         (offset 12, 2 bytes)
          //   uint16_t height        (offset 14, 2 bytes)
          //   uint16_t jpeg_size     (offset 16, 2 bytes)
          //   uint8_t  format        (offset 18, 1 byte)
          //   uint8_t  jpeg_data[]   (offset 19)
          _parseLegacyCameraFrame(data, payloadStart, payloadLength, frames);
        } else if (msgId == cameraFrameChunkMsgId && payloadLength >= 23) {
          // camera_frame_chunk format (see _parseCameraFrameChunk for full layout):
          //   uint64_t timestamp, uint32_t frame_index, uint16_t chunk_index,
          //   uint16_t chunks_total, uint16_t chunk_size, uint16_t width,
          //   uint16_t height, uint8_t format, uint8_t[1024] chunk_data
          _parseCameraFrameChunk(data, payloadStart, payloadLength, chunkMap);
        } else if (msgId == audioFrameMsgId && payloadLength >= 18) {
          // audio_frame format:
          //   uint64_t timestamp       (offset 0, 8 bytes)
          //   uint32_t frame_index     (offset 8, 4 bytes)
          //   uint16_t sample_rate     (offset 12, 2 bytes)
          //   uint8_t  channel         (offset 14, 1 byte)
          //   uint8_t  bits_per_sample (offset 15, 1 byte)
          //   uint16_t aac_size        (offset 16, 2 bytes)
          //   uint8_t  aac_data[]      (offset 18, up to 1536 bytes)
          _parseAudioFrame(data, payloadStart, payloadLength, audioFrames);
        }
      }

      scanPos = nextPos;
    }

    // Reassemble camera_frame_chunk into full frames
    _reassembleChunks(chunkMap, frames);

    return UlogParseResult(frames: frames, audioFrames: audioFrames);
  }

  /// Parse legacy camera_frame (non-chunked) format.
  static void _parseLegacyCameraFrame(
      Uint8List data, int payloadStart, int payloadLength, List<UlogFrame> frames) {
    final timestamp = _readUint64(data, payloadStart);
    final frameIndex = _readUint32(data, payloadStart + 8);
    final width = _readUint16(data, payloadStart + 12);
    final height = _readUint16(data, payloadStart + 14);
    final jpegSize = _readUint16(data, payloadStart + 16);
    final format = data[payloadStart + 18];

    final remaining = payloadLength - 19;
    final size = jpegSize > 0 && jpegSize <= remaining ? jpegSize : remaining;
    if (size > 0) {
      final jpegData = data.sublist(payloadStart + 19, payloadStart + 19 + size);
      // Verify JPEG magic (FFD8)
      if (jpegData.length >= 2 && jpegData[0] == 0xFF && jpegData[1] == 0xD8) {
        frames.add(UlogFrame(
          frameIndex: frameIndex,
          width: width,
          height: height,
          jpegSize: size,
          format: format,
          timestamp: timestamp,
          jpegData: Uint8List.fromList(jpegData),
        ));
      }
    }
  }

  /// Parse camera_frame_chunk — store in chunk map for reassembly.
  ///
  /// Binary layout (matches camera_frame_chunk_s):
  ///   offset  0: uint64 timestamp      (8 bytes)
  ///   offset  8: uint32 frame_index     (4 bytes)
  ///   offset 12: uint16 chunk_index     (2 bytes)
  ///   offset 14: uint16 chunks_total    (2 bytes)
  ///   offset 16: uint16 chunk_size      (2 bytes)
  ///   offset 18: uint16 width           (2 bytes)
  ///   offset 20: uint16 height          (2 bytes)
  ///   offset 22: uint8  format          (1 byte)
  ///   offset 23: uint8[1024] chunk_data (variable, up to 1024)
  static void _parseCameraFrameChunk(
      Uint8List data, int payloadStart, int payloadLength, Map<int, Map<int, Uint8List>> chunkMap) {
    final frameIndex = _readUint32(data, payloadStart + 8);
    final chunkIndex = _readUint16(data, payloadStart + 12);
    final width = _readUint16(data, payloadStart + 18);
    final height = _readUint16(data, payloadStart + 20);
    final chunkSize = _readUint16(data, payloadStart + 16);

    if (chunkSize <= 0 || chunkSize > 1024) return;
    final chunkDataStart = payloadStart + 23;
    if (chunkDataStart + chunkSize > payloadStart + payloadLength) return;

    final chunkData = data.sublist(chunkDataStart, chunkDataStart + chunkSize);

    // Store with width/height/format encoded in a special entry at key -1
    chunkMap.putIfAbsent(frameIndex, () => {});
    chunkMap[frameIndex]![-1] = Uint8List.fromList([
      width & 0xFF, (width >> 8) & 0xFF,
      height & 0xFF, (height >> 8) & 0xFF,
    ]);
    // Use chunk_index from the message as key for correct reassembly order
    chunkMap[frameIndex]![chunkIndex] = Uint8List.fromList(chunkData);
  }

  /// Reassemble camera_frame_chunk chunks into full JPEG frames.
  static void _reassembleChunks(
      Map<int, Map<int, Uint8List>> chunkMap, List<UlogFrame> frames) {
    for (final entry in chunkMap.entries) {
      final frameIndex = entry.key;
      final chunks = entry.value;

      // Extract frame metadata
      final meta = chunks[-1];
      if (meta == null || meta.length < 4) continue;
      final width = meta[0] | (meta[1] << 8);
      final height = meta[2] | (meta[3] << 8);

      // Concatenate all chunks in order (skip metadata at -1)
      final keys = chunks.keys.where((k) => k >= 0).toList()..sort();
      final allData = <int>[];
      for (final k in keys) {
        allData.addAll(chunks[k]!);
      }

      if (allData.isEmpty) continue;

      final jpegData = Uint8List.fromList(allData);
      // Verify JPEG magic (FFD8)
      if (jpegData.length >= 2 && jpegData[0] == 0xFF && jpegData[1] == 0xD8) {
        frames.add(UlogFrame(
          frameIndex: frameIndex,
          width: width,
          height: height,
          jpegSize: allData.length,
          format: 0,
          timestamp: 0,
          jpegData: jpegData,
        ));
      }
    }
  }

  /// Parse audio_frame — extract AAC-ADTS data.
  static void _parseAudioFrame(
      Uint8List data, int payloadStart, int payloadLength, List<UlogAudioFrame> audioFrames) {
    final timestamp = _readUint64(data, payloadStart);
    final frameIndex = _readUint32(data, payloadStart + 8);
    final sampleRate = _readUint16(data, payloadStart + 12);
    final channel = data[payloadStart + 14];
    final bitsPerSample = data[payloadStart + 15];
    final aacSize = _readUint16(data, payloadStart + 16);

    if (aacSize <= 0 || aacSize > 1536) return;
    final aacDataStart = payloadStart + 18;
    if (aacDataStart + aacSize > payloadStart + payloadLength) return;

    final aacData = data.sublist(aacDataStart, aacDataStart + aacSize);

    // Verify ADTS sync word (0xFFF)
    if (aacData.length >= 2 && (aacData[0] & 0xFF) == 0xFF && (aacData[1] & 0xF0) == 0xF0) {
      audioFrames.add(UlogAudioFrame(
        frameIndex: frameIndex,
        timestamp: timestamp,
        sampleRate: sampleRate,
        channel: channel,
        bitsPerSample: bitsPerSample,
        aacSize: aacSize,
        aacData: Uint8List.fromList(aacData),
      ));
    }
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
