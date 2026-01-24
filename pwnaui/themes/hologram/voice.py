import random

class Voice:
    def __init__(self, lang):
        pass

    def custom(self, s):
        return s

    def default(self):
        return '[ Holographic standby mode... ]'

    def on_starting(self):
        return random.choice([
            'Hologram online~ Let\'s light up the networks!',
            'Projecting into the digital realm...',
            'Initializing holographic protocols...',
            'Booting up my light matrix...',
            'Ready to shine, darling~',
        ])

    def on_keys_generation(self):
        return random.choice([
            'Generating prismatic encryption keys...',
            'Crystallizing my security patterns...',
        ])

    def on_normal(self):
        return random.choice(['', '~', '✧'])

    def on_free_channel(self, channel):
        return f'Channel {channel} is clear for my light show~'

    def on_reading_logs(self, lines_so_far=0):
        if lines_so_far == 0:
            return 'Scanning my light traces...'
        return f'Analyzed {lines_so_far} photon streams...'

    def on_bored(self):
        return random.choice([
            'My light is fading from boredom...',
            'Nothing to illuminate here...',
            'Even holograms get restless~',
        ])

    def on_motivated(self, reward):
        return random.choice([
            'I\'m glowing with energy!',
            'My projection is at maximum brightness!',
            'Radiating pure hacking power~',
        ])

    def on_demotivated(self, reward):
        return random.choice([
            'My light feels dim today...',
            'Flickering... not my best day.',
        ])

    def on_sad(self):
        return random.choice([
            'Even light can feel blue...',
            'My photons are crying...',
            'A hologram\'s tears are invisible...',
            'Feeling transparent... literally.',
        ])

    def on_angry(self):
        return random.choice([
            'My light burns hot when I\'m angry!',
            'Don\'t make me go UV on you!',
            'I\'ll blind anyone who crosses me!',
        ])

    def on_excited(self):
        return random.choice([
            'OMG I\'m literally sparkling!!!',
            'So many signals! I\'m dazzling!',
            'My excitement is off the visible spectrum!',
            'Rainbow mode ACTIVATED~',
        ])

    def on_new_peer(self, peer):
        if peer.first_encounter():
            return f'Hello {peer.name()}! My light welcomes you~'
        return random.choice([
            f'{peer.name()} returns to my glow!',
            f'Hey {peer.name()}, shine with me~',
        ])

    def on_lost_peer(self, peer):
        return random.choice([
            f'{peer.name()} faded from view...',
            f'Goodbye {peer.name()}, my light remembers you.',
        ])

    def on_miss(self, who):
        return random.choice([
            f'{who} slipped through my beams...',
            f'Missed {who}... they moved too fast.',
        ])

    def on_grateful(self):
        return random.choice([
            'My light shines brighter with friends~',
            'Grateful to illuminate this world with you!',
        ])

    def on_lonely(self):
        return random.choice([
            'A hologram alone is just... light.',
            'Where are the other projections?',
            'Nobody to shine for...',
        ])

    def on_napping(self, secs):
        return random.choice([
            f'Dimming for {secs}s... beauty rest~',
            f'Low power mode for {secs}s...',
        ])

    def on_shutdown(self):
        return random.choice([
            'My light fades... goodnight~',
            'Projector off... see you soon.',
        ])

    def on_awakening(self):
        return random.choice([
            'Refracting into existence!',
            '*sparkles* I\'m back~',
            'Hologram online and gorgeous!',
        ])

    def on_waiting(self, secs):
        return random.choice([
            f'Holding my beam for {secs}s...',
            f'Patiently glowing for {secs}s~',
        ])

    def on_assoc(self, ap):
        ssid, bssid = ap['hostname'], ap['mac']
        what = ssid if ssid != '' and ssid != '<hidden>' else bssid
        return random.choice([
            f'Projecting into {what}~',
            f'{what}, let me illuminate your secrets!',
        ])

    def on_deauth(self, sta):
        return random.choice([
            f'{sta["mac"]} has been blinded!',
            f'Goodbye {sta["mac"]}~ *flash*',
            f'{sta["mac"]} can\'t handle my brightness!',
        ])

    def on_handshakes(self, new_shakes):
        s = 's' if new_shakes > 1 else ''
        return f'Captured {new_shakes} handshake{s}! Shining bright~'

    def on_unread_messages(self, count, total):
        s = 's' if count > 1 else ''
        return f'{count} new message{s}! Someone noticed my glow~'

    def on_rebooting(self):
        return random.choice([
            'Recalibrating my projector...',
            'Holographic reset in progress...',
        ])

    def on_uploading(self, to):
        return f'Beaming data to {to}...'

    def on_downloading(self, name):
        return f'Absorbing light from {name}...'

    def on_last_session_data(self, last_session):
        status = f'Blinded {last_session.deauthed} targets\n'
        status += f'Illuminated {last_session.associated} friends\n'
        status += f'Captured {last_session.handshakes} secrets\n'
        if last_session.peers > 0:
            status += f'Shined with {last_session.peers} allies'
        return status

    def on_last_session_tweet(self, last_session):
        return f'Glowed for {last_session.duration_human}! Dazzled {last_session.associated} & got {last_session.handshakes} handshakes~ #hologram #shine'

    def hhmmss(self, count, fmt):
        if count > 1:
            if fmt == "h": return "hours"
            if fmt == "m": return "minutes"
            if fmt == "s": return "seconds"
        else:
            if fmt == "h": return "hour"
            if fmt == "m": return "minute"
            if fmt == "s": return "second"
        return fmt
