## Version history

### 1.2.3 (Oct 8th, 2026)
- Fix Settings -> Music SD Card scan crash: added null guards for cross-screen UI widgets.
- Fix directory recursion stack canary overflow by moving path buffers to PSRAM.
- Added O(1) Fast Seek PSRAM Track Offset Table (0ms track access, 0 bytes DRAM).
- Added graceful failure handling when SD card is not present or unmounted.
- Improved playback display with instant clean track title extraction and continuous autoplay queue.

### 1.2.2 (June 17th, 2026)
- Fix RTC return wrong data make app crash for sometime.
- Fix random play the same sequence.
- Stability improved

### 1.2.1 (Feb 10th, 2026)
- Wrong path audio files indexing function fixed

### 1.2.0 (Jan 25th, 2026)
- First release

https://www.youtube.com/watch?v=7HEO5P5ezfM
