# Changelog

All notable changes to this project will be documented in this file.

## [v2.9.5.3-bumchinz.1] - 2026-01-22

### Added
- **PwnaUI** - Native C display renderer
  - 10-30× faster than PIL-based rendering
  - Reduces CPU usage from 10-30% to 1-3%
  - Unix socket IPC with Python
  - Native memtemp, battery, bluetooth plugins
  - Theme system with runtime switching

- **Theme System**
  - 15+ pre-installed themes
  - WebUI theme switcher
  - Custom face pack support

- **Update Infrastructure**
  - `install.sh` - One-click installation
  - `update.sh` - Update to any version
  - `rollback.sh` - Restore from backup
  - Automatic backups before updates

- **Enhanced WebUI**
  - Single-page dashboard design
  - Plugin management cards
  - Theme switching

### Changed
- Display rendering moved from Python/PIL to C
- Improved nexmon stability (lower CPU stress)
- Better performance on Pi Zero

### Based On
- jayofelony/pwnagotchi v2.9.5.3
- evilsocket/pwnagotchi

### License
GPL-3.0
