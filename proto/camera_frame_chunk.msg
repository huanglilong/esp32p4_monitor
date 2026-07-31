# Camera JPEG frame chunk for ULog recording
# Each complete JPEG frame is split into 1024-byte chunks and published as
# multiple camera_frame_chunk messages. The extraction tool reassembles
# chunks by frame_index to reconstruct the complete JPEG.
#
# Chunk size 1024 bytes: balances waste vs publish overhead.
# - Dark scene (7.3KB JPEG): 8 chunks, 15% waste (vs 35% with 4KB)
# - Normal scene (30-50KB JPEG): 30-50 chunks, 5-6% waste (vs 8-13% with 4KB)
# - ORB_QUEUE_LENGTH=32 buffers 1-4 frames depending on JPEG size
#
# Per-frame metadata (width, height, format) is included here so that
# camera_frame_chunk is self-contained — extraction tools can reconstruct
# JPEG frames without needing a separate metadata message.
# These fields are only meaningful for chunk_index==0 but are present in
# every chunk for fixed-size struct compatibility.
uint64  timestamp           # Timestamp (µs since boot)
uint32  frame_index         # Sequential frame counter
uint16  chunk_index         # Chunk index within this frame (0, 1, 2, ...)
uint16  chunks_total        # Total number of chunks for this frame
uint16  chunk_size          # Actual data bytes in this chunk (<= 1024, last chunk may be smaller)
uint16  width               # Image width in pixels (valid for chunk_index==0, e.g. 640)
uint16  height              # Image height in pixels (valid for chunk_index==0, e.g. 480)
uint8   format              # Pixel format: 0=JPEG, 1=RGB565, 2=BGR888, 3=YUV422, 4=YUV420
uint8[1024] chunk_data      # Chunk data (max 1024 bytes per chunk, last chunk may be smaller)

uint8 ORB_QUEUE_LENGTH = 32