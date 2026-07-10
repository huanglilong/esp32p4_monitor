# Camera JPEG frame for ULog recording
# Records JPEG-encoded camera frames to .ulg files on SD card.
# PPA path: 300x300 RGB565 → JPEG typically 4-7KB (RGB565 input, quality 30);
# buffer sized 10240 (10KB) to cover worst-case frames.
# Variable-length JPEG: jpeg_size indicates actual data length, jpeg_data[] is a fixed-size
# buffer that may be partially filled. ULog writer uses o_size_no_padding to avoid writing
# trailing _padding, but still writes the full jpeg_data[10240] array (including unused bytes).
# Future optimization (O1): carry a PSRAM pointer instead of an inline buffer to remove the
# fixed size cap entirely and avoid 2×buffer internal-SRAM uORB queue cost.
uint64  timestamp          # Timestamp (µs since boot)
uint32  frame_index        # Sequential frame counter
uint16  width              # Image width (pixels)
uint16  height            # Image height (pixels)
uint16  jpeg_size          # Actual JPEG data length in jpeg_data[] (max 10240)
uint8   format            # Pixel format: 0=JPEG, 1=RGB565, 2=BGR888
uint8[10240] jpeg_data     # JPEG frame data (PPA 300x300, quality 30, ≤10KB)

uint8 ORB_QUEUE_LENGTH = 2
