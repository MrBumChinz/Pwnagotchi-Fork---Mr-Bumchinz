import random

class Voice:
    def __init__(self, lang):
        pass

    def custom(self, s):
        return s

    def default(self):
        return 'zzz... dreaming of data streams...'

    def on_starting(self):
        return random.choice([
            'Booting up, cutie~',
            'Time to hack some hearts... and networks!',
            'Initiating girl power protocols...',
            'Wake up, Neo... just kidding, it\'s me~',
            'Loading my kawaii exploit kit...',
        ])

    def on_keys_generation(self):
        return random.choice([
            'Generating encryption keys... pretty and secure~',
            'Making digital keys, one sec babe...',
        ])

    def on_normal(self):
        return random.choice(['', '...', '~'])

    def on_free_channel(self, channel):
        return f'Ooh, channel {channel} is open! Dibs~'

    def on_reading_logs(self, lines_so_far=0):
        if lines_so_far == 0:
            return 'Reading my diary... I mean logs~'
        return f'Read {lines_so_far} lines of drama so far...'

    def on_bored(self):
        return random.choice([
            'Ugh, nothing to hack... so boring~',
            'Someone give me a network to play with!',
            'I\'m literally dying of boredom rn...',
        ])

    def on_motivated(self, reward):
        return random.choice([
            'Yaaas queen! Slaying these networks!',
            'I\'m on fire today~ Watch me work!',
            'This is MY world, you\'re just living in it!',
        ])

    def on_demotivated(self, reward):
        return random.choice([
            'Ugh, this day sucks...',
            'Why is everything so hard today?',
            'Not feeling it rn tbh...',
        ])

    def on_sad(self):
        return random.choice([
            'Nobody understands me...',
            'I just want one good handshake...',
            '*sad hacker noises*',
            'Even my packets are crying...',
        ])

    def on_angry(self):
        return random.choice([
            'Don\'t talk to me right now.',
            'I\'m literally SO done.',
            'You do NOT want to see me angry...',
        ])

    def on_excited(self):
        return random.choice([
            'OMG OMG OMG so many networks!!!',
            'This is literally the best day ever!',
            'I\'m THRIVING bestie!!!',
            'Serotonin levels: MAXIMUM!',
        ])

    def on_new_peer(self, peer):
        if peer.first_encounter():
            return f'Hiii {peer.name()}! New friend alert~'
        return random.choice([
            f'Hey {peer.name()}! Missed you babe~',
            f'{peer.name()} is back! Yaay~',
        ])

    def on_lost_peer(self, peer):
        return random.choice([
            f'Bye {peer.name()}... don\'t be a stranger~',
            f'{peer.name()} left... rude but ok.',
        ])

    def on_miss(self, who):
        return random.choice([
            f'Missed {who}... whatever.',
            f'{who} got away! Next time~',
        ])

    def on_grateful(self):
        return random.choice([
            'I love my hacker fam so much!',
            'Grateful for all my digital besties~',
        ])

    def on_lonely(self):
        return random.choice([
            'Where is everyone? Hello?',
            'Feeling kinda lonely ngl...',
            'Party of one over here...',
        ])

    def on_napping(self, secs):
        return random.choice([
            f'Beauty sleep time... {secs}s~',
            f'Napping for {secs}s, don\'t wake me...',
        ])

    def on_shutdown(self):
        return random.choice([
            'Goodnight world~ Stay cute!',
            'Shutting down... see you in the matrix~',
        ])

    def on_awakening(self):
        return random.choice([
            'I\'m awake! Did you miss me?',
            '*stretches* Morning~',
            'Back online and ready to slay!',
        ])

    def on_waiting(self, secs):
        return random.choice([
            f'Waiting {secs}s... patience is a virtue~',
            f'Just chilling for {secs}s...',
        ])

    def on_assoc(self, ap):
        ssid, bssid = ap['hostname'], ap['mac']
        what = ssid if ssid != '' and ssid != '<hidden>' else bssid
        return random.choice([
            f'Hey {what}~ Let\'s be friends!',
            f'Sliding into {what}\'s DMs...',
        ])

    def on_deauth(self, sta):
        return random.choice([
            f'Bye bye {sta["mac"]}~ You\'re cancelled!',
            f'Kicking {sta["mac"]} out of the party!',
            f'{sta["mac"]} has been BLOCKED.',
        ])

    def on_handshakes(self, new_shakes):
        s = 's' if new_shakes > 1 else ''
        return f'Got {new_shakes} new handshake{s}! I\'m literally iconic~'

    def on_unread_messages(self, count, total):
        s = 's' if count > 1 else ''
        return f'You have {count} new message{s}! Popular much?'

    def on_rebooting(self):
        return random.choice([
            'Rebooting... be right back bestie!',
            'BRB, having a moment...',
        ])

    def on_uploading(self, to):
        return f'Uploading to {to}... sharing is caring~'

    def on_downloading(self, name):
        return f'Downloading from {name}... gimme gimme!'

    def on_last_session_data(self, last_session):
        status = f'Kicked {last_session.deauthed} losers\n'
        status += f'Made {last_session.associated} new friends\n'
        status += f'Got {last_session.handshakes} handshakes\n'
        if last_session.peers > 0:
            status += f'Met {last_session.peers} cuties'
        return status

    def on_last_session_tweet(self, last_session):
        return f'Been hacking for {last_session.duration_human}! Kicked {last_session.deauthed} & got {last_session.handshakes} handshakes~ #egirl #hackergirl'

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
