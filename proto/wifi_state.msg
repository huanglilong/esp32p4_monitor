# WiFi connection state
uint64  timestamp
bool    connected       # STA connected to AP
bool    scanning        # Scan in progress
int8    rssi            # Current RSSI (dBm)
char[32] ssid           # Connected SSID
