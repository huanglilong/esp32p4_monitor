#!/usr/bin/env python3
"""
Extract camera JPEG frames from ULog (.ulg) files and optionally create video.

This script parses ULog binary format directly to extract JPEG frames
from camera_frame_chunk (chunked format) or camera_frame (legacy format)
topics. It handles the ULog format correctly, including topics where the
ULog writer omits trailing _padding bytes (PX4 o_size_no_padding convention).

Usage:
    python3 ulog_extract_frames.py input.ulg [--output-dir frames/] [--verbose]
    python3 ulog_extract_frames.py input.ulg --video              # extract + create MP4
    python3 ulog_extract_frames.py input.ulg --video --framerate 5 # custom framerate

Output:
    - Extracted JPEG files: frame_NNNNN.jpg
    - Summary: frame count, dimensions, sizes
    - Optional: MP4 video via FFmpeg
"""

import struct
import argparse
import os
import sys
import shutil
import subprocess


def extract_camera_frames(ulg_path, output_dir, verbose=False):
    """Parse ULog file and extract all camera_frame JPEG data."""

    with open(ulg_path, 'rb') as f:
        data = f.read()

    # Verify magic
    if data[:8] != b'ULog\x01\x12\x35\x01':
        print(f"Error: Not a valid ULog file: {ulg_path}")
        return 0

    # Parse definitions section
    pos = 16
    formats = {}        # topic_name -> format_str
    subscriptions = {}  # msg_id -> (topic_name, payload_size)
    # payload_size = sizeof(struct) = msg_size - 2 (msg_id)

    while pos + 3 <= len(data):
        msg_size = struct.unpack('<H', data[pos:pos+2])[0]
        msg_type = data[pos+2]

        if msg_size > 200000:
            break

        if chr(msg_type) == 'F':
            fmt = data[pos+3:pos+3+msg_size].decode('ascii', errors='replace')
            topic_name = fmt.split(':')[0]
            formats[topic_name] = fmt

        elif chr(msg_type) == 'A':
            multi_id = data[pos+3]
            msg_id = struct.unpack('<H', data[pos+4:pos+6])[0]
            name_bytes = data[pos+6:pos+3+msg_size]
            name = name_bytes.decode('ascii', errors='replace')
            # payload_size will be determined from actual DATA messages
            subscriptions[msg_id] = name

        # Stop at first DATA or SYNC message (data section starts)
        if chr(msg_type) in ('D', 'S'):
            break

        next_pos = pos + 3 + msg_size
        if next_pos <= pos:
            break
        pos = next_pos

    # Find camera_frame_chunk or camera_frame msg_id
    camera_frame_msg_id = None
    is_chunked = False
    for msg_id, name in subscriptions.items():
        if name == 'camera_frame_chunk':
            camera_frame_msg_id = msg_id
            is_chunked = True
            break
        elif name == 'camera_frame':
            camera_frame_msg_id = msg_id
            break

    if camera_frame_msg_id is None:
        print("No camera_frame/camera_frame_chunk topic found in ULog file")
        return 0

    if verbose:
        print(f"{'camera_frame_chunk' if is_chunked else 'camera_frame'} msg_id = {camera_frame_msg_id}")
        topic_name = 'camera_frame_chunk' if is_chunked else 'camera_frame'
        if topic_name in formats:
            fmt = formats.get(topic_name, '')
            print(f"{topic_name} format: {fmt[:100]}...")

    # Chunk reassembly buffer (for camera_frame_chunk format)
    chunk_buffers = {}  # frame_index -> {chunks: {idx: data}, chunks_total, width, height, timestamp}

    # Parse entire file for camera_frame / camera_frame_chunk DATA messages
    # (skip definition section by scanning for DATA messages directly)
    os.makedirs(output_dir, exist_ok=True)

    frame_count = 0
    total_bytes = 0
    scan_pos = 16  # skip file header

    while scan_pos + 3 <= len(data):
        msg_size = struct.unpack('<H', data[scan_pos:scan_pos+2])[0]
        msg_type = data[scan_pos+2]

        if msg_size > 200000:
            # Corrupted or end of data
            break

        next_pos = scan_pos + 3 + msg_size
        if next_pos > len(data) or next_pos <= scan_pos:
            break

        if chr(msg_type) == 'D' and msg_size >= 5:
            msg_id = struct.unpack('<H', data[scan_pos+3:scan_pos+5])[0]

            if msg_id == camera_frame_msg_id:
                payload = data[scan_pos+5:scan_pos+3+msg_size]

                if is_chunked:
                    # ── camera_frame_chunk format ──
                    # Layout: timestamp(8) + frame_index(4) + chunk_index(2) +
                    #         chunks_total(2) + chunk_size(2) + width(2) +
                    #         height(2) + format(1) + chunk_data(1024)
                    if len(payload) < 21:
                        scan_pos = next_pos
                        continue

                    frame_idx = struct.unpack('<I', payload[8:12])[0]
                    chunk_idx = struct.unpack('<H', payload[12:14])[0]
                    chunks_total = struct.unpack('<H', payload[14:16])[0]
                    chunk_size = struct.unpack('<H', payload[16:18])[0]
                    width = struct.unpack('<H', payload[18:20])[0]
                    height = struct.unpack('<H', payload[20:22])[0]

                    if chunk_size == 0 or chunks_total == 0:
                        scan_pos = next_pos
                        continue

                    chunk_data = payload[22:22+chunk_size]

                    if frame_idx not in chunk_buffers:
                        chunk_buffers[frame_idx] = {
                            'chunks': {},
                            'chunks_total': chunks_total,
                            'width': width,
                            'height': height,
                            'timestamp': struct.unpack('<Q', payload[0:8])[0],
                        }
                    buf = chunk_buffers[frame_idx]
                    buf['chunks'][chunk_idx] = chunk_data

                    # Check if all chunks received
                    if len(buf['chunks']) == buf['chunks_total']:
                        jpeg_data = b''
                        for i in range(buf['chunks_total']):
                            jpeg_data += buf['chunks'][i]

                        if jpeg_data[:2] == b'\xff\xd8':
                            filename = f"frame_{frame_count:06d}.jpg"
                            filepath = os.path.join(output_dir, filename)
                            with open(filepath, 'wb') as f:
                                f.write(jpeg_data)

                            if verbose or frame_count <= 5:
                                print(f"  Frame {frame_idx}: {buf['width']}x{buf['height']}, {len(jpeg_data)} bytes, ts={buf['timestamp']}")

                            frame_count += 1
                            total_bytes += len(jpeg_data)

                        chunk_buffers.pop(frame_idx, None)

                else:
                    # ── camera_frame (old single-buffer format) ──
                    # Layout (after msg_id, i.e. starting at scan_pos+5):
                    #   uint64_t timestamp     (offset 0, 8 bytes)
                    #   uint32_t frame_index   (offset 8, 4 bytes)
                    #   uint16_t width         (offset 12, 2 bytes)
                    #   uint16_t height        (offset 14, 2 bytes)
                    #   uint16_t jpeg_size     (offset 16, 2 bytes)
                    #   uint8_t  format        (offset 18, 1 byte)
                    #   uint8_t  jpeg_data[15360] (offset 19, up to 15360 bytes)
                    if len(payload) < 19:
                        scan_pos = next_pos
                        continue

                    timestamp = struct.unpack('<Q', payload[0:8])[0]
                    frame_index = struct.unpack('<I', payload[8:12])[0]
                    width = struct.unpack('<H', payload[12:14])[0]
                    height = struct.unpack('<H', payload[14:16])[0]
                    jpeg_size = struct.unpack('<H', payload[16:18])[0]
                    fmt_type = payload[18]

                    if jpeg_size > 0 and 19 + jpeg_size <= len(payload):
                        jpeg_data = payload[19:19+jpeg_size]

                        if jpeg_data[:2] == b'\xff\xd8':
                            filename = f"frame_{frame_count:06d}.jpg"
                            filepath = os.path.join(output_dir, filename)
                            with open(filepath, 'wb') as f:
                                f.write(jpeg_data)

                            if verbose or frame_count <= 5:
                                print(f"  Frame {frame_index}: {width}x{height}, {jpeg_size} bytes, ts={timestamp}")

                            frame_count += 1
                            total_bytes += jpeg_size
                        else:
                            if verbose:
                                print(f"  Frame {frame_index}: invalid JPEG magic ({jpeg_data[:4].hex()}), skipping")

        scan_pos = next_pos

    return frame_count


def generate_video(frames_dir, output_path, framerate, verbose=False):
    """Generate MP4 video from extracted JPEG frames using FFmpeg."""
    ffmpeg = shutil.which('ffmpeg')
    if ffmpeg is None:
        print("Error: ffmpeg not found in PATH. Install FFmpeg to use --video.")
        return False

    cmd = [
        ffmpeg, '-y',
        '-framerate', str(framerate),
        '-i', os.path.join(frames_dir, 'frame_%06d.jpg'),
        '-c:v', 'libx264',
        '-pix_fmt', 'yuv420p',
        '-movflags', '+faststart',
        output_path,
    ]

    if verbose:
        print(f"\nRunning: {' '.join(cmd)}")

    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
        if result.returncode == 0:
            size_kb = os.path.getsize(output_path) / 1024
            print(f"Video created: {output_path} ({size_kb:.1f} KB)")
            return True
        else:
            print(f"FFmpeg error:\n{result.stderr[-500:]}" if result.stderr else "FFmpeg failed")
            return False
    except subprocess.TimeoutExpired:
        print("Error: FFmpeg timed out after 120 seconds")
        return False
    except Exception as e:
        print(f"Error running FFmpeg: {e}")
        return False


def main():
    parser = argparse.ArgumentParser(description='Extract JPEG frames from ULog camera recording')
    parser.add_argument('input', help='Input ULog (.ulg) file path')
    parser.add_argument('--output-dir', '-o', default=None,
                        help='Output directory for JPEG frames (default: <input>_frames/)')
    parser.add_argument('--no-video', action='store_true',
                        help='Skip video generation (only extract JPEG frames)')
    parser.add_argument('--framerate', '-r', type=float, default=5,
                        help='Video framerate in fps (default: 5)')
    parser.add_argument('--verbose', '-v', action='store_true',
                        help='Print detailed frame info')
    args = parser.parse_args()

    if not os.path.exists(args.input):
        print(f"Error: File not found: {args.input}")
        sys.exit(1)

    output_dir = args.output_dir
    if output_dir is None:
        base = os.path.splitext(args.input)[0]
        output_dir = base + '_frames'

    print(f"Extracting frames from: {args.input}")
    print(f"Output directory: {output_dir}")

    frame_count = extract_camera_frames(args.input, output_dir, args.verbose)

    if frame_count > 0:
        print(f"\nExtracted {frame_count} frames to {output_dir}/")

        if not args.no_video:
            base = os.path.splitext(args.input)[0]
            video_path = base + '.mp4'
            if generate_video(output_dir, video_path, args.framerate, args.verbose):
                # Video created successfully — remove extracted JPEG frames to save disk space
                shutil.rmtree(output_dir, ignore_errors=True)
                print(f"Removed extracted frames (video saved to {video_path})")
            else:
                print(f"\nTo create video manually with FFmpeg:")
                print(f'  ffmpeg -framerate {args.framerate} -i {output_dir}/frame_%06d.jpg '
                      f'-c:v libx264 -pix_fmt yuv420p output.mp4')
        else:
            print(f"\nTo create video with FFmpeg:")
            print(f'  ffmpeg -framerate {args.framerate} -i {output_dir}/frame_%06d.jpg '
                  f'-c:v libx264 -pix_fmt yuv420p output.mp4')
    else:
        print("\nNo camera frames found in ULog file")
        sys.exit(1)


if __name__ == '__main__':
    main()
