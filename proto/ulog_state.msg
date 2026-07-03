# ULog writer state
uint64  timestamp
bool    logging           # True if currently logging to .ulg file
char[128] filepath        # Current log file path (empty if not logging)
