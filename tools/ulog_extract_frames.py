#!/usr/bin/env python3
"""
Extract camera JPEG frames from ULog (.ulg) files.

This script works around a pyulog bug where trailing _padding fields
cause max_data_size to be underestimated, marking valid data as corrupt.

Usage:
    python3 ulog_extract_frames.py input.ulg [--output-dir frames/] [--verbose]

Output:
    - Extracted JPEG files: frame_NNNNN.jpg
    - Summary: frame count, dimensions, sizes
    - Optional: FFmpeg command to create video
"""

import struct
import argparse
import os
import sys


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

    # Find camera_frame msg_id
    camera_frame_msg_id = None
    for msg_id, name in subscriptions.items():
        if name == 'camera_frame':
            camera_frame_msg_id = msg_id
            break

    if camera_frame_msg_id is None:
        print("No camera_frame topic found in ULog file")
        return 0

    if verbose:
        print(f"camera_frame msg_id = {camera_frame_msg_id}")
        if camera_frame_msg_id in formats or 'camera_frame' in formats:
            fmt = formats.get('camera_frame', '')
            print(f"camera_frame format: {fmt[:100]}...")

    # Parse entire file for camera_frame DATA messages
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
                # Parse camera_frame payload
                # Layout (after msg_id, i.e. starting at scan_pos+5):
                #   uint64_t timestamp     (offset 0, 8 bytes)
                #   uint32_t frame_index   (offset 8, 4 bytes)
                #   uint16_t width         (offset 12, 2 bytes)
                #   uint16_t height        (offset 14, 2 bytes)
                #   uint16_t jpeg_size     (offset 16, 2 bytes)
                #   uint8_t  format        (offset 18, 1 byte)
                #   uint8_t  jpeg_data[8192] (offset 19, up to 8192 bytes)
                #   uint8_t  _padding0[5]  (tail padding)
                payload = data[scan_pos+5:scan_pos+3+msg_size]

                if len(payload) < 19:
                    scan_pos = next_pos
                    continue

                timestamp = struct.unpack('<Q', payload[0:8])[0]
                frame_index = struct.unpack('<I', payload[8:12])[0]
                width = struct.unpack('<H', payload[12:14])[0]
                height = struct.unpack('<H', payload[14:16])[0]
                jpeg_size = struct.unpack('<H', payload[16:18])[0]
                fmt_type = payload[18]

                # Extract JPEG data
                if jpeg_size > 0 and 19 + jpeg_size <= len(payload):
                    jpeg_data = payload[19:19+jpeg_size]

                    # Verify JPEG magic
                    if jpeg_data[:2] == b'\xff\xd8':
                        filename = f"frame_{frame_index:06d}.jpg"
                        filepath = os.path.join(output_dir, filename)
                        with open(filepath, 'wb') as f:
                            f.write(jpeg_data)
                        frame_count += 1
                        total_bytes += jpeg_size

                        if verbose or frame_count <= 5:
                            print(f"  Frame {frame_index}: {width}x{height}, {jpeg_size} bytes, ts={timestamp}")
                    else:
                        if verbose:
                            print(f"  Frame {frame_index}: invalid JPEG magic ({jpeg_data[:4].hex()}), skipping")

        scan_pos = next_pos

    return frame_count


def main():
    parser = argparse.ArgumentParser(description='Extract JPEG frames from ULog camera recording')
    parser.add_argument('input', help='Input ULog (.ulg) file path')
    parser.add_argument('--output-dir', '-o', default=None,
                        help='Output directory for JPEG frames (default: <input>_frames/)')
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
        print(f"\nTo create video with FFmpeg:")
        print(f'  ffmpeg -framerate 2 -i {output_dir}/frame_%06d.jpg -c:v libx264 -pix_fmt yuv420p output.mp4')
    else:
        print("\nNo camera frames found in ULog file")
        sys.exit(1)


if __name__ == '__main__':
    main()
