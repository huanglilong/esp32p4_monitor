#!/usr/bin/env python3
"""
ulog_to_video.py — Extract Camera + Audio from ULog (.ulg) and merge into a video file.

Parses ULog binary format, extracts:
  - camera_frame_chunk: JPEG frames (chunk reassembly) → image sequence
  - audio_frame: AAC-ADTS audio → .aac file

Then uses FFmpeg to combine them into a single video file (.mp4/.mkv).

Usage:
    # Default: extract camera+audio, merge to MP4
    python3 ulog_to_video.py log.ulg

    # Specify output path and framerate
    python3 ulog_to_video.py log.ulg -o output.mp4 -r 5

    # Extract only (no ffmpeg merge)
    python3 ulog_to_video.py log.ulg --extract-only

    # Keep intermediate files (JPEG frames + .aac)
    python3 ulog_to_video.py log.ulg --keep

    # Audio only (no camera frames in ULog)
    python3 ulog_to_video.py log.ulg --audio-only

ULog File Format: https://docs.px4.io/main/en/dev_log/ulog_file_format.html
"""

import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

# ── ULog constants ──
ULOG_MAGIC = b"ULog\x01\x12\x35\x01"
ULOG_MSG_HEADER_LEN = 3  # msg_size (2) + msg_type (1)

MSG_TYPE_FORMAT = ord('F')
MSG_TYPE_DATA = ord('D')
MSG_TYPE_INFO = ord('I')
MSG_TYPE_ADD_LOGGED_MSG = ord('A')
MSG_TYPE_SYNC = ord('S')
MSG_TYPE_DROPOUT = ord('O')

# C type → (struct format char, size in bytes)
CTYPE_MAP = {
    'uint8_t':  ('B', 1),
    'uint16_t': ('H', 2),
    'uint32_t': ('I', 4),
    'uint64_t': ('Q', 8),
    'int8_t':   ('b', 1),
    'int16_t':  ('h', 2),
    'int32_t':  ('i', 4),
    'int64_t':  ('q', 8),
    'float':    ('f', 4),
    'float32':  ('f', 4),
    'float64':  ('d', 8),
    'double':   ('d', 8),
    'bool':     ('B', 1),
    'char':     ('c', 1),
}

# ADTS sampling_frequency_index mapping
ADTS_SAMPLE_RATE_MAP = {
    96000: 0, 88200: 1, 64000: 2, 48000: 3, 44100: 4,
    32000: 5, 24000: 6, 22050: 7, 16000: 8, 12000: 9,
    11025: 10, 8000: 11, 7350: 12,
}


def patch_adts_sample_rate(aac_frame: bytearray, target_rate: int) -> bytearray:
    """Patch the sampling_frequency_index in an ADTS frame header.

    ADTS header byte 2 contains the 4-bit sampling_frequency_index at bits [5:2].
    This patches those bits to reflect the actual sample rate,
    fixing playback speed when the encoder was misconfigured.
    """
    if target_rate not in ADTS_SAMPLE_RATE_MAP:
        return aac_frame
    new_index = ADTS_SAMPLE_RATE_MAP[target_rate]
    # Byte 2: bits [5:2] = sampling_frequency_index (4 bits)
    aac_frame[2] = (aac_frame[2] & 0xC3) | ((new_index & 0x0F) << 2)
    return aac_frame


# ── Data classes ──

@dataclass
class TopicFormat:
    """Parsed ULog topic format from F messages."""
    topic_name: str = ""
    fields: dict = field(default_factory=dict)       # fname → (base_type, array_size)
    field_order: list = field(default_factory=list)   # ordered field names


@dataclass
class CameraFrameInfo:
    """Info about extracted camera frames."""
    frame_count: int = 0
    width: int = 0
    height: int = 0
    total_bytes: int = 0
    first_timestamp_us: int = 0
    last_timestamp_us: int = 0
    framerate: float = 0.0
    chunks_per_frame_min: int = 0
    chunks_per_frame_avg: int = 0
    chunks_per_frame_max: int = 0


@dataclass
class AudioFrameInfo:
    """Info about extracted audio."""
    frame_count: int = 0
    total_bytes: int = 0
    sample_rate: int = 0
    channels: int = 0
    bits_per_sample: int = 0
    duration_seconds: float = 0.0
    first_timestamp_us: int = 0
    last_timestamp_us: int = 0


# ── ULog parser ──

def parse_format_message(payload: bytes) -> TopicFormat:
    """Parse a ULog Format (F) message."""
    text = payload.decode('utf-8', errors='replace')
    colon_pos = text.find(':')
    if colon_pos < 0:
        return TopicFormat()

    topic_name = text[:colon_pos]
    fields_str = text[colon_pos + 1:]

    fields = {}
    field_order = []
    for part in fields_str.split(';'):
        part = part.strip()
        if not part:
            continue
        space_pos = part.find(' ')
        if space_pos < 0:
            continue
        ftype = part[:space_pos]
        fname = part[space_pos + 1:]
        array_size = 1
        base_type = ftype
        bracket_pos = ftype.find('[')
        if bracket_pos >= 0:
            base_type = ftype[:bracket_pos]
            try:
                array_size = int(ftype[bracket_pos + 1:ftype.index(']')])
            except (ValueError, IndexError):
                array_size = 1
        fields[fname] = (base_type, array_size)
        field_order.append(fname)

    return TopicFormat(topic_name=topic_name, fields=fields, field_order=field_order)


def _build_header_struct(topic_fmt: TopicFormat, blob_field: str):
    """Build struct format for header fields (everything before the blob field).

    Returns (struct_format_str, header_size, blob_offset, blob_max_size).
    """
    fmt_parts = ['<']
    offset = 0
    blob_offset = 0
    blob_max_size = 0
    header_size = 0
    header_field_names = []

    for fname in topic_fmt.field_order:
        if fname.startswith('_padding'):
            continue
        base_type, array_size = topic_fmt.fields[fname]

        if fname == blob_field:
            blob_offset = offset
            blob_max_size = array_size
            header_size = offset
            offset += array_size * CTYPE_MAP.get(base_type, ('B', 1))[1]
            continue

        type_info = CTYPE_MAP.get(base_type)
        if not type_info:
            raise ValueError(f"Unknown C type: {base_type}")
        fmt_char, type_size = type_info
        if array_size > 1:
            fmt_parts.append(f'{array_size}{fmt_char}')
        else:
            fmt_parts.append(fmt_char)
        offset += type_size * array_size
        header_field_names.append(fname)

    return ''.join(fmt_parts), header_size, blob_offset, blob_max_size, header_field_names


def _find_field_offset(topic_fmt: TopicFormat, field_name: str) -> int:
    """Find byte offset of a field in the packed struct."""
    offset = 0
    for fname in topic_fmt.field_order:
        if fname.startswith('_padding'):
            continue
        if fname == field_name:
            return offset
        base_type, array_size = topic_fmt.fields[fname]
        type_size = CTYPE_MAP.get(base_type, ('B', 1))[1]
        offset += type_size * array_size
    return -1


def parse_ulog(ulg_path: str, frames_dir: str, aac_output_path: str,
               verbose: bool = False, fix_sample_rate: int = 0) -> tuple[CameraFrameInfo, AudioFrameInfo]:
    """Parse ULog file and extract camera frames + audio.

    Returns (camera_info, audio_info).
    """
    cam_info = CameraFrameInfo()
    aud_info = AudioFrameInfo()

    with open(ulg_path, 'rb') as f:
        # ── File header ──
        magic = f.read(8)
        if magic != ULOG_MAGIC:
            print(f"Error: Not a ULog file (magic: {magic!r})", file=sys.stderr)
            return cam_info, aud_info
        _timestamp = struct.unpack('<Q', f.read(8))[0]

        # ── Definition section ──
        formats: dict[str, TopicFormat] = {}
        subscriptions: dict[int, str] = {}  # msg_id → topic_name
        camera_chunk_msg_id: int | None = None
        audio_msg_id: int | None = None

        while True:
            hdr = f.read(ULOG_MSG_HEADER_LEN)
            if len(hdr) < ULOG_MSG_HEADER_LEN:
                break
            msg_size = struct.unpack('<H', hdr[:2])[0]
            msg_type = hdr[2]
            payload = f.read(msg_size)
            if len(payload) < msg_size:
                break

            if msg_type == MSG_TYPE_FORMAT:
                tfmt = parse_format_message(payload)
                if tfmt.topic_name:
                    formats[tfmt.topic_name] = tfmt

            elif msg_type == MSG_TYPE_ADD_LOGGED_MSG:
                if msg_size < 3:
                    continue
                _multi_id = payload[0]
                msg_id = struct.unpack('<H', payload[1:3])[0]
                topic_name = payload[3:].decode('utf-8', errors='replace')
                subscriptions[msg_id] = topic_name
                if topic_name == 'camera_frame_chunk':
                    camera_chunk_msg_id = msg_id
                elif topic_name == 'audio_frame':
                    audio_msg_id = msg_id

            elif msg_type == MSG_TYPE_DATA:
                # Definition section over — seek back
                f.seek(-(ULOG_MSG_HEADER_LEN + msg_size), 1)
                break

        if verbose:
            print(f"  Topics: {list(formats.keys())}")
            print(f"  Subscriptions: {subscriptions}")
            print(f"  camera_frame_chunk msg_id: {camera_chunk_msg_id}")
            print(f"  audio_frame msg_id: {audio_msg_id}")

        # ── Prepare format structs ──
        # camera_frame_chunk: chunk reassembly
        cam_chunk_frame_index_offset = -1
        cam_chunk_chunk_index_offset = -1
        cam_chunk_chunks_total_offset = -1
        cam_chunk_chunk_size_offset = -1
        cam_chunk_chunk_data_offset = -1
        cam_chunk_width_offset = -1
        cam_chunk_height_offset = -1
        cam_chunk_format_offset = -1
        if camera_chunk_msg_id is not None and 'camera_frame_chunk' in formats:
            ck_fmt = formats['camera_frame_chunk']
            cam_chunk_frame_index_offset = _find_field_offset(ck_fmt, 'frame_index')
            cam_chunk_chunk_index_offset = _find_field_offset(ck_fmt, 'chunk_index')
            cam_chunk_chunks_total_offset = _find_field_offset(ck_fmt, 'chunks_total')
            cam_chunk_chunk_size_offset = _find_field_offset(ck_fmt, 'chunk_size')
            cam_chunk_chunk_data_offset = _find_field_offset(ck_fmt, 'chunk_data')
            cam_chunk_width_offset = _find_field_offset(ck_fmt, 'width')
            cam_chunk_height_offset = _find_field_offset(ck_fmt, 'height')
            cam_chunk_format_offset = _find_field_offset(ck_fmt, 'format')

        # audio_frame: find aac_size and aac_data offsets
        aud_aac_size_offset = -1
        aud_aac_data_offset = -1
        aud_header_struct = None
        aud_header_field_names = []
        if audio_msg_id is not None and 'audio_frame' in formats:
            aud_fmt = formats['audio_frame']
            aud_aac_size_offset = _find_field_offset(aud_fmt, 'aac_size')
            aud_aac_data_offset = _find_field_offset(aud_fmt, 'aac_data')
            _, aud_header_size, _, _, aud_header_field_names = _build_header_struct(aud_fmt, 'aac_data')
            header_fmt_parts = ['<']
            for fname in aud_fmt.field_order:
                if fname.startswith('_padding') or fname == 'aac_data':
                    continue
                base_type, array_size = aud_fmt.fields[fname]
                fmt_char, type_size = CTYPE_MAP[base_type]
                if array_size > 1:
                    header_fmt_parts.append(f'{array_size}{fmt_char}')
                else:
                    header_fmt_parts.append(fmt_char)
            aud_header_struct = struct.Struct(''.join(header_fmt_parts))

        # ── Data section ──
        os.makedirs(frames_dir, exist_ok=True)
        aac_file = open(aac_output_path, 'wb') if audio_msg_id is not None else None

        # Chunk reassembly buffer: frame_index → { chunks: {index: data}, chunks_total, metadata }
        chunk_buffers: dict[int, dict] = {}
        _chunks_per_frame: list[int] = []

        try:
            while True:
                hdr = f.read(ULOG_MSG_HEADER_LEN)
                if len(hdr) < ULOG_MSG_HEADER_LEN:
                    break
                msg_size = struct.unpack('<H', hdr[:2])[0]
                msg_type = hdr[2]

                if msg_size > 200000:
                    # Likely corrupted — skip
                    f.read(msg_size)
                    continue

                if msg_type == MSG_TYPE_DATA:
                    payload = f.read(msg_size)
                    if len(payload) < msg_size:
                        break
                    msg_id = struct.unpack('<H', payload[:2])[0]
                    data_payload = payload[2:]  # skip msg_id

                    # ── Camera frame chunk ──
                    if (camera_chunk_msg_id is not None and msg_id == camera_chunk_msg_id
                          and cam_chunk_frame_index_offset >= 0):
                        if len(data_payload) < cam_chunk_chunk_data_offset:
                            continue
                        frame_idx = struct.unpack('<I',
                            data_payload[cam_chunk_frame_index_offset:cam_chunk_frame_index_offset + 4])[0]
                        chunk_idx = struct.unpack('<H',
                            data_payload[cam_chunk_chunk_index_offset:cam_chunk_chunk_index_offset + 2])[0]
                        chunks_total = struct.unpack('<H',
                            data_payload[cam_chunk_chunks_total_offset:cam_chunk_chunks_total_offset + 2])[0]
                        chunk_size = struct.unpack('<H',
                            data_payload[cam_chunk_chunk_size_offset:cam_chunk_chunk_size_offset + 2])[0]

                        if chunk_size == 0 or chunk_size > 1024 or chunks_total == 0:
                            continue

                        chunk_data = data_payload[cam_chunk_chunk_data_offset:
                                                   cam_chunk_chunk_data_offset + chunk_size]

                        # Initialize buffer for this frame if needed
                        if frame_idx not in chunk_buffers:
                            chunk_buffers[frame_idx] = {
                                'chunks': {},
                                'chunks_total': chunks_total,
                                'timestamp': 0,
                                'width': 0,
                                'height': 0,
                            }
                        buf = chunk_buffers[frame_idx]
                        buf['chunks'][chunk_idx] = chunk_data

                        # Parse per-frame metadata from first chunk (chunk_index==0)
                        if chunk_idx == 0:
                            buf['timestamp'] = struct.unpack('<Q',
                                data_payload[0:8])[0]
                            if cam_chunk_width_offset >= 0:
                                buf['width'] = struct.unpack('<H',
                                    data_payload[cam_chunk_width_offset:cam_chunk_width_offset + 2])[0]
                            if cam_chunk_height_offset >= 0:
                                buf['height'] = struct.unpack('<H',
                                    data_payload[cam_chunk_height_offset:cam_chunk_height_offset + 2])[0]

                        # Check if all chunks received
                        if len(buf['chunks']) == buf['chunks_total']:
                            # Reassemble JPEG
                            jpeg_data = b''
                            for i in range(buf['chunks_total']):
                                jpeg_data += buf['chunks'][i]

                            if jpeg_data[:2] != b'\xff\xd8':
                                if verbose:
                                    print(f"  Invalid JPEG magic at frame {cam_info.frame_count}, skipping")
                                chunk_buffers.pop(frame_idx, None)
                                continue

                            filepath = os.path.join(frames_dir, f"frame_{cam_info.frame_count:06d}.jpg")
                            with open(filepath, 'wb') as jf:
                                jf.write(jpeg_data)

                            if cam_info.frame_count == 0:
                                cam_info.width = buf['width']
                                cam_info.height = buf['height']
                                cam_info.first_timestamp_us = buf['timestamp']
                            cam_info.last_timestamp_us = buf['timestamp']

                            cam_info.frame_count += 1
                            cam_info.total_bytes += len(jpeg_data)
                            _chunks_per_frame.append(buf['chunks_total'])
                            chunk_buffers.pop(frame_idx, None)

                    # ── Audio frame ──
                    elif msg_id == audio_msg_id and aud_aac_size_offset >= 0 and aac_file:
                        if len(data_payload) < aud_aac_data_offset:
                            continue
                        aac_size = struct.unpack('<H',
                            data_payload[aud_aac_size_offset:aud_aac_size_offset + 2])[0]
                        if aac_size == 0 or aac_size > 8192:
                            continue
                        aac_data = data_payload[aud_aac_data_offset:
                                                 aud_aac_data_offset + aac_size]
                        if fix_sample_rate > 0:
                            aac_data = patch_adts_sample_rate(bytearray(aac_data), fix_sample_rate)
                        aac_file.write(aac_data)

                        # Parse header for metadata
                        if aud_header_struct and len(data_payload) >= aud_header_struct.size:
                            vals = aud_header_struct.unpack_from(data_payload)
                            field_map = dict(zip(aud_header_field_names, vals))
                            if aud_info.frame_count == 0:
                                aud_info.sample_rate = field_map.get('sample_rate', 0)
                                aud_info.channels = field_map.get('channel', 0)
                                aud_info.bits_per_sample = field_map.get('bits_per_sample', 0)
                                aud_info.first_timestamp_us = field_map.get('timestamp', 0)
                            aud_info.last_timestamp_us = field_map.get('timestamp', 0)

                        aud_info.frame_count += 1
                        aud_info.total_bytes += aac_size

                elif msg_type in (MSG_TYPE_SYNC, MSG_TYPE_DROPOUT, MSG_TYPE_INFO):
                    f.read(msg_size)
                else:
                    # Unknown or definition message in data section — skip
                    if msg_size > 0:
                        f.read(msg_size)

        finally:
            if aac_file:
                aac_file.close()

    # ── Compute derived stats ──
    if cam_info.frame_count > 1 and cam_info.last_timestamp_us > cam_info.first_timestamp_us:
        elapsed_s = (cam_info.last_timestamp_us - cam_info.first_timestamp_us) / 1_000_000.0
        cam_info.framerate = cam_info.frame_count / elapsed_s if elapsed_s > 0 else 0.0

    if _chunks_per_frame:
        cam_info.chunks_per_frame_min = min(_chunks_per_frame)
        cam_info.chunks_per_frame_avg = sum(_chunks_per_frame) // len(_chunks_per_frame)
        cam_info.chunks_per_frame_max = max(_chunks_per_frame)

    if aud_info.frame_count > 0 and aud_info.last_timestamp_us > aud_info.first_timestamp_us:
        # Use timestamp span for real-time duration (more accurate than frame count,
        # especially when resampling is involved — e.g. 48kHz I2S → 16kHz AAC).
        aud_info.duration_seconds = (aud_info.last_timestamp_us - aud_info.first_timestamp_us) / 1_000_000.0
    elif aud_info.sample_rate > 0 and aud_info.frame_count > 0:
        # Fallback: AAC frame = 1024 samples
        aud_info.duration_seconds = (aud_info.frame_count * 1024) / aud_info.sample_rate

    return cam_info, aud_info


# ── FFmpeg merge ──

def merge_video_audio(frames_dir: str, aac_path: str, output_path: str,
                       framerate: float, cam_info: CameraFrameInfo,
                       aud_info: AudioFrameInfo, verbose: bool = False) -> bool:
    """Merge JPEG frames + AAC audio into a video file using FFmpeg."""
    ffmpeg = shutil.which('ffmpeg')
    if ffmpeg is None:
        print("Error: ffmpeg not found in PATH. Install FFmpeg to create video.", file=sys.stderr)
        return False

    has_camera = cam_info.frame_count > 0
    has_audio = aud_info.frame_count > 0 and os.path.getsize(aac_path) > 0

    if not has_camera and not has_audio:
        print("Error: No camera or audio data to merge.", file=sys.stderr)
        return False

    cmd = [ffmpeg, '-y']

    if has_camera:
        cmd += [
            '-framerate', str(framerate),
            '-i', os.path.join(frames_dir, 'frame_%06d.jpg'),
        ]
        video_input_idx = 0
    else:
        video_input_idx = -1

    if has_audio:
        cmd += ['-i', aac_path]
        audio_input_idx = 1 if has_camera else 0
    else:
        audio_input_idx = -1

    # Video codec
    if has_camera:
        # Try libx264 first, fall back to default encoder
        h264_available = False
        probe = subprocess.run([ffmpeg, '-encoders'], capture_output=True, text=True, timeout=10)
        if probe.returncode == 0 and 'libx264' in probe.stdout:
            h264_available = True
        if h264_available:
            cmd += ['-c:v', 'libx264', '-pix_fmt', 'yuv420p', '-movflags', '+faststart']
        else:
            cmd += ['-c:v', 'mpeg4', '-q:v', '5']
    else:
        # Audio only — no video codec needed
        pass

    # Audio codec
    if has_audio:
        cmd += ['-c:a', 'aac', '-b:a', '128k']

    # Map inputs
    if has_camera and has_audio:
        cmd += ['-map', f'{video_input_idx}:v', '-map', f'{audio_input_idx}:a']
    elif has_audio and not has_camera:
        cmd += ['-map', f'{audio_input_idx}:a']

    # Shortest flag — stop when the shorter stream ends
    if has_camera and has_audio:
        cmd += ['-shortest']

    cmd.append(output_path)

    if verbose:
        print(f"\nRunning: {' '.join(cmd)}")

    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
        if result.returncode == 0:
            size_kb = os.path.getsize(output_path) / 1024
            print(f"Video created: {output_path} ({size_kb:.1f} KB)")
            return True
        else:
            print(f"FFmpeg error:\n{result.stderr[-1000:]}" if result.stderr else "FFmpeg failed",
                  file=sys.stderr)
            return False
    except subprocess.TimeoutExpired:
        print("Error: FFmpeg timed out after 300 seconds", file=sys.stderr)
        return False
    except Exception as e:
        print(f"Error running FFmpeg: {e}", file=sys.stderr)
        return False


# ── Main ──

def main():
    parser = argparse.ArgumentParser(
        description='Extract Camera + Audio from ULog (.ulg) and merge into video')
    parser.add_argument('input', help='Input .ulg file path')
    parser.add_argument('-o', '--output', default=None,
                        help='Output video file path (default: <input>.mp4)')
    parser.add_argument('-r', '--framerate', type=float, default=5,
                        help='Camera framerate in fps (default: 5)')
    parser.add_argument('--extract-only', action='store_true',
                        help='Only extract JPEG frames + AAC audio, no FFmpeg merge')
    parser.add_argument('--audio-only', action='store_true',
                        help='Only extract audio (no video generation)')
    parser.add_argument('--keep', action='store_true',
                        help='Keep intermediate files (JPEG frames + .aac)')
    parser.add_argument('--workdir', default=None,
                        help='Working directory for intermediate files (default: temp dir)')
    parser.add_argument('-v', '--verbose', action='store_true',
                        help='Print detailed extraction info')
    parser.add_argument('--fix-sample-rate', type=int, default=0, metavar='HZ',
                        help='Patch ADTS headers to this sample rate (e.g. 48000). '
                             'Fixes firmware bug where 48kHz PCM was encoded as 16kHz AAC, '
                             'causing 3x slow playback.')
    args = parser.parse_args()

    ulg_path = Path(args.input)
    if not ulg_path.exists():
        print(f"Error: File not found: {ulg_path}", file=sys.stderr)
        sys.exit(1)

    # Output path
    output_path = args.output
    if output_path is None:
        output_path = str(ulg_path.with_suffix('.mp4'))

    # Work directory for intermediate files
    if args.workdir:
        work_dir = args.workdir
        os.makedirs(work_dir, exist_ok=True)
        cleanup = False
    else:
        work_dir = tempfile.mkdtemp(prefix='ulog2video_')
        cleanup = not args.keep

    frames_dir = os.path.join(work_dir, 'frames')
    aac_path = os.path.join(work_dir, 'audio.aac')

    try:
        print(f"ULog file: {ulg_path}")
        print(f"Work dir:  {work_dir}")
        print()

        # ── Parse and extract ──
        cam_info, aud_info = parse_ulog(str(ulg_path), frames_dir, aac_path, args.verbose,
                                        fix_sample_rate=args.fix_sample_rate)

        # ── Print stats ──
        print(f"\n{'═' * 50}")
        print(f"  Camera frames: {cam_info.frame_count}")
        if cam_info.frame_count > 0:
            print(f"    Resolution:  {cam_info.width}x{cam_info.height}")
            print(f"    Total JPEG:  {cam_info.total_bytes / 1024:.1f} KB")
            print(f"    Duration:    {(cam_info.last_timestamp_us - cam_info.first_timestamp_us) / 1_000_000:.1f} s")
            print(f"    Framerate:   {cam_info.framerate:.1f} fps (actual)")
            if cam_info.chunks_per_frame_max > 0:
                print(f"    Chunks/frame: min={cam_info.chunks_per_frame_min} avg={cam_info.chunks_per_frame_avg} max={cam_info.chunks_per_frame_max}")
        print(f"  Audio frames:  {aud_info.frame_count}")
        if aud_info.frame_count > 0:
            print(f"    Sample rate: {aud_info.sample_rate} Hz")
            print(f"    Channels:    {aud_info.channels}")
            print(f"    AAC bytes:   {aud_info.total_bytes / 1024:.1f} KB")
            print(f"    Duration:    {aud_info.duration_seconds:.1f} s")
        print(f"{'═' * 50}")

        if cam_info.frame_count == 0 and aud_info.frame_count == 0:
            print("\nNo camera_frame_chunk or audio_frame data found in ULog file.")
            sys.exit(1)

        # ── Audio-only mode ──
        if args.audio_only:
            final_aac = str(ulg_path.with_suffix('.aac'))
            if os.path.exists(aac_path) and os.path.getsize(aac_path) > 0:
                shutil.copy2(aac_path, final_aac)
                print(f"\nAudio extracted: {final_aac} ({os.path.getsize(final_aac) / 1024:.1f} KB)")
            else:
                print("\nNo audio data found.")
            return

        # ── Extract-only mode ──
        if args.extract_only:
            # Copy intermediate files next to the ulg file
            if cam_info.frame_count > 0:
                target_frames = str(ulg_path.with_suffix('')) + '_frames'
                if target_frames != frames_dir:
                    if os.path.exists(target_frames):
                        shutil.rmtree(target_frames)
                    shutil.copytree(frames_dir, target_frames)
                print(f"\nCamera frames: {target_frames}/ ({cam_info.frame_count} files)")
            if aud_info.frame_count > 0 and os.path.getsize(aac_path) > 0:
                target_aac = str(ulg_path.with_suffix('.aac'))
                shutil.copy2(aac_path, target_aac)
                print(f"Audio file:     {target_aac}")
            return

        # ── Merge with FFmpeg ──
        if cam_info.frame_count > 0:
            # Use actual framerate if detected, otherwise use specified
            effective_fps = cam_info.framerate if cam_info.framerate > 0 else args.framerate
            print(f"\nMerging with FFmpeg (framerate={effective_fps:.1f} fps)...")
            success = merge_video_audio(frames_dir, aac_path, output_path,
                                         effective_fps, cam_info, aud_info, args.verbose)
        elif aud_info.frame_count > 0:
            # Audio only — just convert AAC to M4A/MP4
            print("\nNo camera frames — extracting audio only...")
            success = merge_video_audio(frames_dir, aac_path, output_path,
                                         args.framerate, cam_info, aud_info, args.verbose)
        else:
            success = False

        if not success:
            # Print manual commands
            print(f"\nManual FFmpeg commands:")
            if cam_info.frame_count > 0:
                print(f"  # Video from JPEG frames:")
                print(f'  ffmpeg -framerate {args.framerate} -i {frames_dir}/frame_%06d.jpg '
                      f'-c:v libx264 -pix_fmt yuv420p video_only.mp4')
            if aud_info.frame_count > 0 and os.path.getsize(aac_path) > 0:
                print(f"  # Audio from AAC:")
                print(f'  ffmpeg -i {aac_path} -c:a aac audio_only.m4a')
            if cam_info.frame_count > 0 and aud_info.frame_count > 0:
                print(f"  # Merge video + audio:")
                print(f'  ffmpeg -i video_only.mp4 -i audio_only.m4a -c copy -shortest {output_path}')

    finally:
        if cleanup and os.path.exists(work_dir):
            shutil.rmtree(work_dir, ignore_errors=True)


if __name__ == '__main__':
    main()
