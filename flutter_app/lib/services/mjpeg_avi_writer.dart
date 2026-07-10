/// MJPEG AVI video writer — pure Dart, zero native dependencies.
///
/// Generates an AVI file with MJPEG-compressed video stream.
/// Compatible with VLC, QuickTime, Windows Media Player, and most players.
///
/// AVI is a RIFF container format:
///   RIFF('AVI '
///     LIST('hdrl'
///       avih(MainAVIHeader)
///       LIST('strl'
///         strh(AVIStreamHeader)
///         strf(BITMAPINFOHEADER)
///       )
///     )
///     LIST('movi'
///       00dc(JPEG frame 1)
///       00dc(JPEG frame 2)
///       ...
///     )
///   )
///
/// Reference: Microsoft AVI RIFF format specification

import 'dart:io';
import 'dart:typed_data';

class MjpegAviWriter {
  final int width;
  final int height;
  final int fps;

  MjpegAviWriter({
    required this.width,
    required this.height,
    required this.fps,
  });

  /// Write JPEG frames to an AVI file.
  /// Returns the output file size in bytes.
  Future<int> write(String outputPath, List<Uint8List> jpegFrames) async {
    final frameCount = jpegFrames.length;
    if (frameCount == 0) return 0;

    // Calculate total video data size (each frame padded to 2-byte boundary)
    int moviDataSize = 0;
    final paddedSizes = <int>[];
    for (final frame in jpegFrames) {
      final padded = (frame.length + 1) & ~1; // pad to even
      paddedSizes.add(padded);
      moviDataSize += 8 + padded; // '00dc' chunk header (8) + data
    }

    // AVI stream format: MJPEG fourcc = 'MJPG'

    // Header sizes
    const strhSize = 56;
    const strfSize = 40;
    const avihSize = 56;
    const strlListSize = 4 + (8 + strhSize) + (8 + strfSize);
    const hdrlListSize = 4 + (8 + avihSize) + (8 + strlListSize);
    final moviListSize = 4 + moviDataSize;

    // RIFF size = everything after 'RIFF' + size field
    final riffSize = 4 + // 'AVI '
        (8 + hdrlListSize) + // LIST('hdrl'...)
        (8 + moviListSize); // LIST('movi'...)

    final file = File(outputPath);
    final sink = file.openWrite();

    try {
      // --- RIFF header ---
      _writeFourCC(sink, 'RIFF');
      _writeLE32(sink, riffSize);
      _writeFourCC(sink, 'AVI ');

      // --- hdrl LIST ---
      _writeFourCC(sink, 'LIST');
      _writeLE32(sink, hdrlListSize);
      _writeFourCC(sink, 'hdrl');

      // --- avih (Main AVI Header) ---
      _writeFourCC(sink, 'avih');
      _writeLE32(sink, avihSize);

      final microSecPerFrame = 1000000 ~/ fps;
      final maxBytesPerSec = (jpegFrames.fold<int>(0, (sum, f) => sum + f.length) * fps) ~/ frameCount;
      _writeLE32(sink, microSecPerFrame); // dwMicroSecPerFrame
      _writeLE32(sink, maxBytesPerSec); // dwMaxBytesPerSec
      _writeLE32(sink, 0); // dwPaddingGranularity
      _writeLE32(sink, 0x10); // dwFlags = AVIF_HASINDEX
      _writeLE32(sink, frameCount); // dwTotalFrames
      _writeLE32(sink, 0); // dwInitialFrames
      _writeLE32(sink, 1); // dwStreams
      _writeLE32(sink, jpegFrames[0].length); // dwSuggestedBufferSize
      _writeLE32(sink, width); // dwWidth
      _writeLE32(sink, height); // dwHeight
      _writeLE32(sink, 0); // dwReserved[0]
      _writeLE32(sink, 0); // dwReserved[1]
      _writeLE32(sink, 0); // dwReserved[2]
      _writeLE32(sink, 0); // dwReserved[3]

      // --- strl LIST ---
      _writeFourCC(sink, 'LIST');
      _writeLE32(sink, strlListSize);
      _writeFourCC(sink, 'strl');

      // --- strh (Stream Header) ---
      _writeFourCC(sink, 'strh');
      _writeLE32(sink, strhSize);
      _writeFourCC(sink, 'vids'); // fccType
      _writeFourCC(sink, 'MJPG'); // fccHandler
      _writeLE32(sink, 0); // dwFlags
      _writeLE16(sink, 0); // wPriority
      _writeLE16(sink, 0); // wLanguage
      _writeLE32(sink, 0); // dwInitialFrames
      _writeLE32(sink, 1); // dwScale
      _writeLE32(sink, fps); // dwRate
      _writeLE32(sink, 0); // dwStart
      _writeLE32(sink, frameCount); // dwLength
      _writeLE32(sink, jpegFrames[0].length); // dwSuggestedBufferSize
      _writeLE32(sink, 0); // dwQuality
      _writeLE32(sink, 0); // dwSampleSize
      _writeLE16(sink, 0); // rcFrame.left
      _writeLE16(sink, 0); // rcFrame.top
      _writeLE16(sink, width); // rcFrame.right
      _writeLE16(sink, height); // rcFrame.bottom

      // --- strf (BITMAPINFOHEADER) ---
      _writeFourCC(sink, 'strf');
      _writeLE32(sink, strfSize);
      _writeLE32(sink, strfSize); // biSize
      _writeLE32(sink, width); // biWidth
      _writeLE32(sink, height); // biHeight (positive = bottom-up, but MJPG ignores this)
      _writeLE16(sink, 1); // biPlanes
      _writeLE16(sink, 24); // biBitCount
      _writeFourCC(sink, 'MJPG'); // biCompression
      _writeLE32(sink, width * height * 3); // biSizeImage
      _writeLE32(sink, 0); // biXPelsPerMeter
      _writeLE32(sink, 0); // biYPelsPerMeter
      _writeLE32(sink, 0); // biClrUsed
      _writeLE32(sink, 0); // biClrImportant

      // --- movi LIST ---
      _writeFourCC(sink, 'LIST');
      _writeLE32(sink, moviListSize);
      _writeFourCC(sink, 'movi');

      // Write each JPEG frame as a '00dc' chunk
      for (int i = 0; i < frameCount; i++) {
        _writeFourCC(sink, '00dc');
        _writeLE32(sink, jpegFrames[i].length);
        sink.add(jpegFrames[i]);
        // Pad to even boundary
        if (jpegFrames[i].length.isOdd) {
          sink.add([0]);
        }
      }

      await sink.flush();
      await sink.close();

      return file.lengthSync();
    } catch (_) {
      await sink.close();
      rethrow;
    }
  }

  void _writeFourCC(IOSink sink, String fourCC) {
    sink.add(fourCC.codeUnits);
  }

  void _writeLE32(IOSink sink, int value) {
    sink.add([
      value & 0xFF,
      (value >> 8) & 0xFF,
      (value >> 16) & 0xFF,
      (value >> 24) & 0xFF,
    ]);
  }

  void _writeLE16(IOSink sink, int value) {
    sink.add([
      value & 0xFF,
      (value >> 8) & 0xFF,
    ]);
  }
}
