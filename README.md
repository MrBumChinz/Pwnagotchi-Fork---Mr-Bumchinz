# 🐾 Pwnagotchi Fork - Mr. Bumchinz Edition

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)

A high-performance Pwnagotchi fork featuring **PwnaUI** - a blazing fast C-based display renderer, WebUI updates, and enhanced stability.

## ✨ Key Features

### ⚡ PwnaUI - Native C Display Renderer
- **10-30× faster** than stock Python/PIL rendering
- Reduces CPU usage from **10-30% to 1-3%**
- **Improves nexmon stability** (lower CPU stress = fewer firmware crashes)
- Native C plugins: memtemp, battery, bluetooth status

### 🔄 WebUI Updates
- Check for updates from web interface
- **One-click update** to any version
- Automatic backup before updates
- Easy rollback if something breaks

### 🎨 Theme System
- Switch themes from WebUI
- Upload custom face packs
- 15+ pre-installed themes

## 🚀 Quick Install

```bash
# On your Pwnagotchi (via SSH)
curl -sSL https://raw.githubusercontent.com/MrBumChinz/Pwnagotchi-Fork---Mr-Bumchinz/main/scripts/install.sh | sudo bash
```

## 💻 Compatibility

| Device | Status |
|--------|--------|
| Pi Zero W | ✅ Tested |
| Pi Zero 2 W | ✅ Tested |
| Pi 3/4 | ⚠️ Should work |

## 📖 Documentation

- [Installation Guide](docs/INSTALL.md)
- [Troubleshooting](docs/TROUBLESHOOTING.md)

## 🤝 Contributing

We welcome pull requests, themes, and bug reports!

## ⚖️ License

GPL-3.0 - Same as original Pwnagotchi

---

**Based on:** jayofelony/pwnagotchi v2.9.5.3  
**Enhancements:** PwnaUI, themes, update infrastructure
