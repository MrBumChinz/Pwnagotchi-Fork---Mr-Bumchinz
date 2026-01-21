# Recovery & Rollback Guide

> **This guide ensures you can always restore your Pwnagotchi to a stable state.**

## Backup Strategy

The fork has **3-tier backup** to protect against failures:

### 1. **Automatic Backups (On Install/Update)**
Every time you run `install.sh` or `update.sh`, backups are created:
```bash
/etc/pwnagotchi/backups/backup-YYYYMMDD-HHMMSS.tar.gz
```

These contain the full Pwnagotchi installation.

### 2. **GitHub Release Tags**
Stable versions are tagged in Git:
- `v2.9.5.3-bumchinz.1` = Initial stable release
- Each tag points to tested, working code

### 3. **Local Git History**
Your Pi's Git clone (`/home/pi/.pwn/.git` if Git installed) can recover any commit.

---

## Rollback Procedures

### **Quick Rollback (Recent Backup)**
```bash
sudo /home/pi/.pwn/scripts/rollback.sh latest
```
Restores to the most recent backup.

### **List Available Backups**
```bash
sudo /home/pi/.pwn/scripts/rollback.sh
```
Shows all available backups with timestamps.

### **Restore Specific Backup**
```bash
sudo /home/pi/.pwn/scripts/rollback.sh /etc/pwnagotchi/backups/backup-20260122-143022.tar.gz
```

### **Rollback to Specific Version**
```bash
sudo /home/pi/.pwn/scripts/update.sh v2.9.5.3-bumchinz.1
```
Downloads and installs tagged version from GitHub.

---

## Recovery Scenarios

### **Pwnagotchi won't start**
```bash
# 1. Check logs
sudo journalctl -u pwnagotchi -n 50

# 2. Rollback to last known good
sudo /home/pi/.pwn/scripts/rollback.sh latest

# 3. Restart service
sudo systemctl restart pwnagotchi
```

### **Corrupted config**
```bash
# Reset to default config
sudo cp /etc/pwnagotchi/config.toml /etc/pwnagotchi/config.toml.bak
sudo cat > /etc/pwnagotchi/config.toml << 'EOF'
[main]
name = "pwnagotchi"
debug = false

[ai]
enabled = false

[ui]
display = "waveshare2in13_V3"
EOF

sudo systemctl restart pwnagotchi
```

### **Services won't restart**
```bash
# Check service status
sudo systemctl status pwnagotchi
sudo systemctl status pwnaui

# Restart both
sudo systemctl restart pwnagotchi pwnaui

# Enable auto-start
sudo systemctl enable pwnagotchi pwnaui
```

### **Need to downgrade version**
```bash
# List available versions
git tag -l

# Install specific version
sudo /home/pi/.pwn/scripts/update.sh v2.9.5.3-bumchinz.1
```

### **Complete system reset**
```bash
# Remove everything and reinstall clean
sudo rm -rf /home/pi/.pwn/lib/python3.11/site-packages/pwnagotchi
sudo bash /home/pi/.pwn/scripts/install.sh

# Or install from GitHub
curl https://raw.githubusercontent.com/MrBumChinz/Pwnagotchi-Fork---Mr-Bumchinz/main/scripts/install.sh | sudo bash
```

---

## Manual Recovery (No Scripts)

If scripts fail, recover manually:

### **From Backup Tar**
```bash
sudo systemctl stop pwnagotchi
sudo rm -rf /home/pi/.pwn/lib/python3.11/site-packages/pwnagotchi
sudo tar -xzf /etc/pwnagotchi/backups/backup-TIMESTAMP.tar.gz -C /home/pi/.pwn/lib/python3.11/site-packages/
sudo systemctl start pwnagotchi
```

### **From GitHub**
```bash
cd /tmp
wget https://github.com/MrBumChinz/Pwnagotchi-Fork---Mr-Bumchinz/archive/refs/tags/v2.9.5.3-bumchinz.1.tar.gz
tar -xzf v2.9.5.3-bumchinz.1.tar.gz
sudo cp -r Pwnagotchi-Fork---Mr-Bumchinz-v2.9.5.3-bumchinz.1/pwnagotchi /home/pi/.pwn/lib/python3.11/site-packages/
sudo systemctl restart pwnagotchi
```

---

## Backup Management

### **Check backup size**
```bash
du -sh /etc/pwnagotchi/backups/
ls -lh /etc/pwnagotchi/backups/
```

### **Clean old backups (keep last 5)**
```bash
ls -t /etc/pwnagotchi/backups/backup-*.tar.gz | tail -n +6 | xargs rm -f
```

### **Archive backups to USB**
```bash
sudo cp /etc/pwnagotchi/backups/*.tar.gz /media/usb/pwnagotchi-backups/
```

---

## GitHub Recovery

If your Pi is completely dead but you have GitHub access:

### **Clone fresh installation**
```bash
git clone https://github.com/MrBumChinz/Pwnagotchi-Fork---Mr-Bumchinz.git pwnagotchi-fork
cd pwnagotchi-fork
sudo bash scripts/install.sh
```

### **Check release history**
Visit: https://github.com/MrBumChinz/Pwnagotchi-Fork---Mr-Bumchinz/releases

All versions are tagged and downloadable.

---

## Prevention

### **Regular Backups**
```bash
# Backup to external storage weekly
sudo cp -r /etc/pwnagotchi/backups /mnt/usb/pwnagotchi-backup-$(date +%Y%m%d)
```

### **Test Rollback**
```bash
# Periodically verify rollback works
sudo /home/pi/.pwn/scripts/rollback.sh latest
sudo systemctl status pwnagotchi
```

### **Monitor Logs**
```bash
# Watch for issues in real-time
sudo journalctl -u pwnagotchi -f
```

---

## Emergency Contacts

If recovery fails:

1. **Reinstall Pi OS** - Reformat SD card, start fresh
2. **Check GitHub Issues** - https://github.com/MrBumChinz/Pwnagotchi-Fork---Mr-Bumchinz/issues
3. **Use Mobile App** - Reinstall via phone SSH when Pi is accessible again

---

**Last Updated:** January 22, 2026  
**Version:** v2.9.5.3-bumchinz.1
