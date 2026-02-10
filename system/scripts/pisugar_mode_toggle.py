#!/usr/bin/env python3
import os, time, fcntl, subprocess, logging
logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(message)s')
log = logging.getLogger('pisugar')

I2C_SLAVE = 0x0703
PISUGAR3_ADDR = 0x57
REG_CTR2 = 0x03

def read_reg(fd, reg):
    os.write(fd, bytes([reg]))
    return os.read(fd, 1)[0]

def get_btn(fd):
    try:
        val = read_reg(fd, REG_CTR2)
        if val & 0x01:
            os.write(fd, bytes([REG_CTR2, val & ~0x01]))
            return 'tap'
    except: pass
    return None

def toggle():
    af, mf = '/root/.pwnagotchi-auto', '/root/.pwnagotchi-manual'
    if os.path.exists(af):
        os.remove(af)
        open(mf,'w').close()
        mode = 'MANUAL'
    elif os.path.exists(mf):
        os.remove(mf)
        open(af,'w').close()
        mode = 'AUTO'
    else:
        open(af,'w').close()
        mode = 'AUTO'
    subprocess.run(['systemctl','restart','pwnagotchi'])
    return mode

fd = os.open('/dev/i2c-1', os.O_RDWR)
fcntl.ioctl(fd, I2C_SLAVE, PISUGAR3_ADDR)
log.info('PiSugar3 mode toggle ready - tap button to switch')
while True:
    if get_btn(fd) == 'tap':
        log.info('Toggling mode...')
        log.info(f'Now: {toggle()}')
    time.sleep(0.2)
