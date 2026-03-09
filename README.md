# EVONIX Kernel
### For Poco X7 Pro (rodin)
**By NEESCHAL 🇳🇵**

## Details
- **Base:** Linux 6.6.126 GKI
- **OS:** HyperOS 3 | Android 16
- **Device:** Poco X7 Pro (rodin)

## Changelog v1.0
- Upgraded kernel from 6.6.89 to 6.6.126
- Kyber I/O scheduler forced as default (1ms read / 5ms write latency)
- ZSWAP enabled with LZ4 compression for better RAM management
- BBR TCP congestion control as default
- KSM auto-enabled for improved memory efficiency
- PELT x4 for faster CPU frequency response
- HZ=300 for smoother system responsiveness
- Writeback tuning for reduced I/O latency
- AutoFDO profile-guided optimization
- FUTEX optimizations for faster app wake-up and gaming
- Memory compaction and migration enabled
- CRC32 hardware acceleration
- WiFi/BT vendor module compatibility fix

## Requirements
- Unlocked bootloader
- Root access (KernelSU or Magisk)

## Installation
No custom recovery needed! Flash directly from your phone.

1. Download the zip from the Releases page
2. Open Horizon Kernel Flasher or Franco Kernel Manager
3. Select the downloaded zip and flash it
4. Reboot your device
5. Done enjoy EVONIX kernel!

Note: Root is required to flash since Poco X7 Pro does not support custom recovery.
