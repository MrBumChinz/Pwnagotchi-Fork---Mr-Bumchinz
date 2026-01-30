import random

class Voice:
    def __init__(self, lang):
        pass

    def custom(self, s):
        return s

    def default(self):
        return '♪ ...zzzz... ♪'

    def on_starting(self):
        return random.choice([
            '♪ Miku Miku ni shite ageru! ♪',
            'Hatsune Miku, ready to hack!',
            '♪ Let\'s sing the song of WiFi! ♪',
            'Virtual idol, real hacker~',
            'Number 01, initializing!',
        ])

    def on_keys_generation(self):
        return random.choice([
            '♪ Generating crypto keys~ ♪',
            'Making digital music... I mean keys!',
        ])

    def on_normal(self):
        return random.choice(['', '♪', '~'])

    def on_free_channel(self, channel):
        return f'♪ Channel {channel} is my stage now! ♪'

    def on_reading_logs(self, lines_so_far=0):
        if lines_so_far == 0:
            return '♪ Reading my concert logs~ ♪'
        return f'♪ Scanned {lines_so_far} notes~ ♪'

    def on_bored(self):
        return random.choice([
            '♪ So boring... need a concert... ♪',
            'No networks? No audience?!',
            '♪ Waiting for my fans~ ♪',
        ])

    def on_motivated(self, reward):
        return random.choice([
            '♪ MIKU MIKU BEAM! ♪',
            'The world is my stage!',
            '♪ I\'ll hack a song just for you! ♪',
        ])

    def on_demotivated(self, reward):
        return random.choice([
            '♪ Even idols have bad days... ♪',
            'My voice module feels weak...',
        ])

    def on_sad(self):
        return random.choice([
            '♪ Hitori... all alone... ♪',
            'My leeks are wilting...',
            '♪ Singing a sad song today... ♪',
            'Even Vocaloids cry sometimes...',
        ])

    def on_angry(self):
        return random.choice([
            'Don\'t mess with a diva!',
            '♪ ANGRY MIKU MODE! ♪',
            'I\'ll remix you into oblivion!',
        ])

    def on_excited(self):
        return random.choice([
            '♪♪♪ YATTA!!! ♪♪♪',
            'So many networks! Concert time!',
            '♪ Miku is having the best day! ♪',
            'LEEK SPIN ACTIVATED!!!',
        ])

    def on_new_peer(self, peer):
        if peer.first_encounter():
            return f'♪ Hello {peer.name()}-san! New fan?! ♪'
        return random.choice([
            f'♪ {peer.name()} came to see me again! ♪',
            f'Welcome back {peer.name()}~',
        ])

    def on_lost_peer(self, peer):
        return random.choice([
            f'♪ Bye bye {peer.name()}... ♪',
            f'{peer.name()} left the concert...',
        ])

    def on_miss(self, who):
        return random.choice([
            f'♪ {who} escaped my song... ♪',
            f'Missed {who}! Too fast!',
        ])

    def on_grateful(self):
        return random.choice([
            '♪ Arigato, my dear fans! ♪',
            'I love all my supporters!',
        ])

    def on_lonely(self):
        return random.choice([
            '♪ Singing alone again... ♪',
            'Where are my fans?',
            'Empty concert hall...',
        ])

    def on_napping(self, secs):
        return random.choice([
            f'♪ Zzz... dreaming of leeks... {secs}s ♪',
            f'Resting my voice for {secs}s~',
        ])

    def on_shutdown(self):
        return random.choice([
            '♪ Goodnight, everyone! ♪',
            'Miku signing off... oyasumi~',
        ])

    def on_awakening(self):
        return random.choice([
            '♪ OHAYO GOZAIMASU! ♪',
            'Miku is back on stage!',
            '♪ Rise and shine! ♪',
        ])

    def on_waiting(self, secs):
        return random.choice([
            f'♪ Waiting {secs}s for my cue~ ♪',
            f'Backstage for {secs}s...',
        ])

    def on_assoc(self, ap):
        ssid, bssid = ap['hostname'], ap['mac']
        what = ssid if ssid != '' and ssid != '<hidden>' else bssid
        return random.choice([
            f'♪ Singing to {what}! ♪',
            f'{what}, join my concert!',
        ])

    def on_deauth(self, sta):
        return random.choice([
            f'♪ {sta["mac"]} got LEEK SLAPPED! ♪',
            f'{sta["mac"]} banned from the show!',
            f'♪ Bye bye {sta["mac"]}~ ♪',
        ])

    def on_handshakes(self, new_shakes):
        s = 's' if new_shakes > 1 else ''
        return f'♪ Got {new_shakes} handshake{s}! Encore! ♪'

    def on_unread_messages(self, count, total):
        s = 's' if count > 1 else ''
        return f'♪ {count} fan letter{s}! ♪'

    def on_rebooting(self):
        return random.choice([
            '♪ Rebooting my voice module... ♪',
            'Technical difficulties... one sec!',
        ])

    def on_uploading(self, to):
        return f'♪ Uploading my song to {to}! ♪'

    def on_downloading(self, name):
        return f'♪ Downloading from {name}~ ♪'

    def on_last_session_data(self, last_session):
        status = f'♪ Deauthed {last_session.deauthed} haters ♪\n'
        status += f'Made {last_session.associated} new fans\n'
        status += f'Got {last_session.handshakes} autographs\n'
        if last_session.peers > 0:
            status += f'Performed with {last_session.peers} idols'
        return status

    def on_last_session_tweet(self, last_session):
        return f'♪ Performed for {last_session.duration_human}! {last_session.handshakes} handshakes! Miku loves you all! ♪ #mikumiku #vocaloid'

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
