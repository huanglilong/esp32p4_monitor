# Camera JPEG frame for ULog recording
# Records JPEG-encoded camera frames to .ulg files on SD card.
# PPA path: 300x300 BGR888 → JPEG typically 5-8KB, occasionally up to ~9-12KB at
# default quality 30; buffer sized 12288 to cover the vast majority of frames.
# Variable-length JPEG: jpeg_size indicates actual data length, jpeg_data[] is a fixed-size
# buffer that may be partially filled. ULog writer uses o_size_no_padding to avoid writing
# trailing _padding, but still writes the full jpeg_data[12288] array (including unused bytes).
# Future optimization (O1): carry a PSRAM pointer instead of an inline buffer to remove the
# fixed size cap entirely and avoid 2×buffer internal-SRAM uORB queue cost.
uint64  timestamp          # Timestamp (µs since boot)
uint32  frame_index        # Sequential frame counter
uint16  width             # Image width (pixels)
uint16  height            # Image height (pixels)
uint16  jpeg_size         # Actual JPEG data length in jpeg_data[] (max 12288)
uint8   format            # Pixel format: 0=JPEG, 1=RGB565, 2=BGR888
uint8[12288] jpeg_data     # JPEG frame data (PPA 300x300, quality 30, typically ≤12KB)

uint8 ORB_QUEUE_LENGTH = 2
