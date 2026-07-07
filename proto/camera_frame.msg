# Camera JPEG frame for ULog recording
# Records JPEG-encoded camera frames to .ulg files on SD card.
# PPA path: 300x300 BGR888 → JPEG ~5-8KB (fits in jpeg_data[8192])
# Full-res path: 800x800 RGB565 → JPEG ~30-50KB (needs larger buffer)
# Variable-length JPEG: jpeg_size indicates actual data length, jpeg_data[] is a fixed-size
# buffer that may be partially filled. ULog writer uses o_size_no_padding to avoid writing
# trailing _padding, but still writes the full jpeg_data[8192] array (including unused bytes).
# Future optimization: only write jpeg_size bytes of jpeg_data (requires ULog format change).
uint64  timestamp          # Timestamp (µs since boot)
uint32  frame_index        # Sequential frame counter
uint16  width              # Image width (pixels)
uint16  height             # Image height (pixels)
uint16  jpeg_size          # Actual JPEG data length in jpeg_data[]
uint8   format             # Pixel format: 0=JPEG, 1=RGB565, 2=BGR888
uint8[8192] jpeg_data      # JPEG frame data (PPA path ≤8KB)

uint8 ORB_QUEUE_LENGTH = 2
