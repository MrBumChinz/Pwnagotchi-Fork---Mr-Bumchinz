# pwnhub_stats.py - Pet System & Battle Stats for PwnHub
# 
# Version: 2.1.0
# Author: James
# License: GPL3 (Open Source)
#
# This plugin provides:
# - XP / Level / Stage tracking (infinite progression)
# - Macro system (PROTEIN/FAT/CARBS = +99% XP max)
# - Mood calculation (affects XP multiplier)
# - Coin balance (prestige currency)
# - GET_STATS, GET_ID, APPLY_UPDATE API
# - Display integration via PwnaUI
#
# DEPENDENCIES:
#   Requires pwnhub.py for driver stability (optional but recommended)
#
# CONFIG:
#   [main.plugins.pwnhub_stats]
#   enabled = true
#   stats_file = "/root/pwnhub_stats.json"
#   cooldown_seconds = 30
#   max_history = 100
#   show_on_display = true
#   compact_mode = false

import logging
import os
import json
import time
import random
import secrets
import math
import shutil

import pwnagotchi
import pwnagotchi.plugins as plugins


# =============================================================================
# PWNAUI IPC FOR DISPLAY INTEGRATION
# =============================================================================

PWNAUI_SOCKET = '/var/run/pwnaui.sock'

def _send_pwnaui_command(cmd):
    """Send one-shot command to PwnaUI daemon."""
    import socket as _sock
    import os as _os
    if not _os.path.exists(PWNAUI_SOCKET):
        return False
    try:
        sock = _sock.socket(_sock.AF_UNIX, _sock.SOCK_STREAM)
        sock.settimeout(0.5)
        sock.connect(PWNAUI_SOCKET)
        if not cmd.endswith('\n'):
            cmd += '\n'
        sock.sendall(cmd.encode('utf-8'))
        sock.recv(256)
        sock.close()
        return True
    except Exception:
        return False


# =============================================================================
# CONSTANTS
# =============================================================================

PLUGIN_VERSION = '2.1.0'
PLUGIN_NAME = 'pwnhub_stats'

# Stage definitions: (min_level, max_level, title)
STAGES = [
    (1, 4, "Hatchling"),
    (5, 9, "Newborn"),
    (10, 19, "Rookie"),
    (20, 34, "Apprentice"),
    (35, 54, "Hunter"),
    (55, 79, "Stalker"),
    (80, 119, "Predator"),
    (120, 174, "Elite"),
    (175, 249, "Veteran"),
    (250, 399, "Master"),
    (400, 599, "Legendary"),
    (600, float('inf'), "Mythic"),
]

# Mood states: (min_score, max_score, state_name, xp_modifier)
MOOD_STATES = [
    (90, 100, "ecstatic", 1.20),
    (70, 89, "happy", 1.10),
    (40, 69, "content", 1.00),
    (20, 39, "bored", 0.95),
    (0, 19, "sad", 0.90),
]


# =============================================================================
# HELPER FUNCTIONS
# =============================================================================

def clamp(value, min_val, max_val):
    """Clamp value between min and max."""
    return max(min_val, min(max_val, value))


def xp_for_level(level: int) -> int:
    """XP required to reach this level. Formula: 100 * (level ^ 1.65)"""
    return int(100 * (level ** 1.65))  # Steeper curve for slower progression


def level_from_xp(xp: int) -> int:
    """Calculate level from total XP."""
    level = 1
    while xp_for_level(level + 1) <= xp:
        level += 1
    return level


def get_stage(level: int) -> tuple:
    """Get (stage_number, title) for a level."""
    for i, (min_lvl, max_lvl, title) in enumerate(STAGES, 1):
        if min_lvl <= level <= max_lvl:
            return i, title
    return 12, "Mythic"


def get_mood_state(score: int) -> tuple:
    """Get (state_name, xp_modifier) for mood score."""
    for min_s, max_s, state, modifier in MOOD_STATES:
        if min_s <= score <= max_s:
            return state, modifier
    return "content", 1.0


# =============================================================================
# PET STATS MANAGER
# =============================================================================

class PetStats:
    """
    Pet stats manager for PwnHub.
    
    Handles:
    - XP / Level / Stage progression
    - Macro system (PROTEIN/FAT/CARBS)
    - Mood calculation
    - Coins
    - Battle history
    """
    
    def __init__(self, stats_file: str = '/root/pwnhub_stats.json', max_history: int = 100):
        self.stats_file = stats_file
        self.max_history = max_history
        self.stats = None
        self._load_or_create()
    
    def _load_or_create(self):
        """Load existing stats or create new ones."""
        if os.path.exists(self.stats_file):
            try:
                with open(self.stats_file, 'r') as f:
                    self.stats = json.load(f)
                
                # Migrate v1 to v2 if needed
                if self.stats.get('version', 1) < 2:
                    self._migrate_v1_to_v2()
                
                logging.info(f"[{PLUGIN_NAME}] Loaded stats: {self.stats['device_id']} "
                           f"Level {self.stats['progression']['level']} "
                           f"({self.stats['progression']['stage_title']})")
                return
            except Exception as e:
                logging.error(f"[{PLUGIN_NAME}] Failed to load stats: {e}")
        
        # Create new stats
        self.stats = self._create_default_stats()
        self._save()
        logging.info(f"[{PLUGIN_NAME}] Created new stats: {self.stats['device_id']}")
    
    def _create_default_stats(self) -> dict:
        """Create default stats structure.
        
        NOTE: We DON'T store handshakes/epochs/etc here!
        Those are read LIVE from pwnagotchi's agent.
        We only store PwnHub-specific data (macros, battles, coins, etc.)
        """
        device_id = f"pwn_{secrets.token_hex(6)}"
        now = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
        
        return {
            "version": 2,
            "device_id": device_id,
            "name": "pwnagotchi",
            "created": now,
            "updated": now,
            
            # === PWNHUB-SPECIFIC DATA (stored in JSON) ===
            
            "progression": {
                "level": 1,
                "xp": 0,
                "xp_to_next": 100,
                "stage": 1,
                "stage_title": "Hatchling",
                "coins": 0
            },
            
            "macros": {
                "protein": {"charges": 25, "max": 50},
                "fat": {"charges": 25, "max": 50},
                "carbs": {"charges": 0, "max": 50}
            },
            
            "mood": {
                "score": 50,
                "state": "content"
            },
            
            "battles": {
                "total": 0,
                "wins": 0,
                "losses": 0,
                "win_streak": 0,
                "best_streak": 0
            },
            
            # Session tracking (for macro gains, not duplicating pwnagotchi)
            "session": {
                "handshakes_at_start": 0,  # To calculate session delta
                "epochs_at_start": 0
            },
            
            "achievements": [],
            "battle_history": [],
            "seen_battle_ids": [],
            "last_battle_time": 0,
            "last_epoch_time": 0
        }
    
    def _migrate_v1_to_v2(self):
        """Migrate v1 stats to v2 format."""
        logging.info(f"[{PLUGIN_NAME}] Migrating stats from v1 to v2...")
        
        old = self.stats
        new = self._create_default_stats()
        
        # Preserve device_id and dates
        new['device_id'] = old.get('device_id', new['device_id'])
        new['created'] = old.get('created', new['created'])
        
        # Migrate old stats structure
        if 'stats' in old:
            s = old['stats']
            new['progression']['level'] = s.get('level', 1)
            new['progression']['xp'] = s.get('xp', 0)
            new['progression']['coins'] = s.get('coins', 0)
            new['battles']['total'] = s.get('battles_total', 0)
            new['battles']['wins'] = s.get('battles_won', 0)
            new['battles']['losses'] = s.get('battles_lost', 0)
            new['battles']['win_streak'] = s.get('win_streak', 0)
            new['battles']['best_streak'] = s.get('best_streak', 0)
            new['achievements'] = s.get('achievements', [])
        
        # Migrate battle history
        new['battle_history'] = old.get('battle_history', [])
        new['seen_battle_ids'] = old.get('seen_battle_ids', [])
        new['last_battle_time'] = old.get('last_battle_time', 0)
        
        # Recalculate derived values
        level = new['progression']['level']
        new['progression']['xp_to_next'] = xp_for_level(level + 1)
        stage, title = get_stage(level)
        new['progression']['stage'] = stage
        new['progression']['stage_title'] = title
        
        self.stats = new
        logging.info(f"[{PLUGIN_NAME}] Migration complete!")
    
    def _save(self):
        """Save stats to file with backup."""
        self.stats['updated'] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
        
        try:
            # Backup existing file
            if os.path.exists(self.stats_file):
                shutil.copy(self.stats_file, f"{self.stats_file}.bak")
            
            # Write new stats
            with open(self.stats_file, 'w') as f:
                json.dump(self.stats, f, indent=2)
                
        except Exception as e:
            logging.error(f"[{PLUGIN_NAME}] Failed to save stats: {e}")
    
    # ========== MACRO SYSTEM ==========
    
    def add_macro(self, macro_type: str, amount: float):
        """Add charges to a macro slot."""
        if macro_type not in self.stats['macros']:
            return
        macro = self.stats['macros'][macro_type]
        macro['charges'] = min(macro['max'], macro['charges'] + amount)
    
    def consume_macro(self, macro_type: str, amount: float):
        """Consume charges from a macro slot."""
        if macro_type not in self.stats['macros']:
            return
        macro = self.stats['macros'][macro_type]
        macro['charges'] = max(0, macro['charges'] - amount)
    
    def get_xp_multiplier(self) -> float:
        """
        Calculate XP multiplier from all macros.
        
        Each macro contributes:
        - +33% when charges >= 20 (fed)
        - +16% when charges 1-19 (hungry)
        - -10% when charges = 0 (starving)
        
        Max: Capped at 1.50 (50% max bonus)
        Min: 1.0 - 0.10 - 0.10 - 0.10 = 0.70 (30% penalty)
        """
        xp_mult = 1.0
        
        for macro_name in ['protein', 'fat', 'carbs']:
            charges = self.stats['macros'][macro_name]['charges']
            if charges >= 20:
                xp_mult += 0.33  # Full buff
            elif charges >= 1:
                xp_mult += 0.16  # Half buff
            else:
                xp_mult -= 0.10  # Penalty
        
        return min(xp_mult, 1.50)  # Cap at 150%
    
    def get_macro_summary(self) -> str:
        """Get human-readable macro status."""
        parts = []
        for name in ['protein', 'fat', 'carbs']:
            charges = int(self.stats['macros'][name]['charges'])
            max_charges = self.stats['macros'][name]['max']
            parts.append(f"{name.upper()[0]}:{charges}/{max_charges}")
        return " ".join(parts)
    
    # ========== MOOD SYSTEM ==========
    
    def calculate_mood(self, recent_captures: int = 0, ai_reward_avg: float = 0.0) -> int:
        """
        Calculate mood score (0-100).
        
        Factors:
        - Macro state (are we well-fed?)
        - Recent captures
        - Win streak
        - AI happiness
        """
        score = 50  # Baseline
        
        # Macro impact (-30 to +40)
        xp_mult = self.get_xp_multiplier()
        score += int((xp_mult - 1.0) * 40)
        
        # Recent success (+0 to +25)
        score += min(recent_captures * 5, 25)
        
        # Win streak (+0 to +15)
        score += min(self.stats['battles']['win_streak'] * 3, 15)
        
        # AI happiness (+0 to +10)
        if ai_reward_avg > 0:
            score += min(int(ai_reward_avg * 10), 10)
        
        score = clamp(score, 0, 100)
        
        # Update mood state
        state, _ = get_mood_state(score)
        self.stats['mood']['score'] = score
        self.stats['mood']['state'] = state
        
        return score
    
    def get_total_xp_multiplier(self) -> float:
        """Get combined XP multiplier from macros + mood."""
        macro_mult = self.get_xp_multiplier()
        _, mood_mult = get_mood_state(self.stats['mood']['score'])
        return macro_mult * mood_mult
    
    # ========== XP / LEVEL SYSTEM ==========
    
    def add_xp(self, base_xp: int) -> dict:
        """
        Add XP with multipliers applied.
        Returns dict with XP gained and level up info.
        """
        multiplier = self.get_total_xp_multiplier()
        final_xp = int(base_xp * multiplier)
        
        old_level = self.stats['progression']['level']
        self.stats['progression']['xp'] += final_xp
        
        # Recalculate level
        new_level = level_from_xp(self.stats['progression']['xp'])
        self.stats['progression']['level'] = new_level
        self.stats['progression']['xp_to_next'] = xp_for_level(new_level + 1)
        
        # Update stage
        stage, title = get_stage(new_level)
        self.stats['progression']['stage'] = stage
        self.stats['progression']['stage_title'] = title
        
        # Check for level up
        leveled_up = new_level > old_level
        
        # Coin milestone (every 50 levels)
        if leveled_up and new_level % 50 == 0:
            self.stats['progression']['coins'] += 25
        
        return {
            'base_xp': base_xp,
            'multiplier': multiplier,
            'final_xp': final_xp,
            'old_level': old_level,
            'new_level': new_level,
            'leveled_up': leveled_up,
            'stage': stage,
            'title': title
        }
    
    def add_coins(self, amount: int):
        """Add coins to balance."""
        self.stats['progression']['coins'] += amount
    
    # ========== EVENT HANDLERS ==========
    
    def on_epoch(self, blind: bool = False, aggression: float = 0.5) -> dict:
        """
        Called each epoch.
        Consumes macros (activity-based drain).
        
        Returns:
            dict with xp_gained and level info
        """
        if blind:
            # Blind epoch = no drain, no XP
            return {'xp_gained': 0, 'leveled_up': False}
        
        # NO passive XP - hunting is the only way to gain XP!
        result = {'xp_gained': 0, 'leveled_up': False, 'final_xp': 0}
        
        # Base consumption
        self.consume_macro('protein', 1)
        self.consume_macro('fat', 1)
        self.consume_macro('carbs', 1)
        
        # Extra drain for high aggression
        if aggression > 0.7:
            self.consume_macro('protein', 0.5)
            self.consume_macro('fat', 0.5)
        
        # Track for session (pwnagotchi tracks total epochs itself)
        self.stats['last_epoch_time'] = time.time()
        
        return result
    
    def on_handshake(self, handshake_count: int = 0) -> dict:
        """
        Called when handshake captured.
        Gains XP, macros, maybe coins.
        
        Args:
            handshake_count: Current total handshakes from pwnagotchi agent
        """
        # XP gain
        result = self.add_xp(50)  # Reduced for year-long progression
        
        # Macro gains
        self.add_macro('protein', random.randint(8, 12))
        self.add_macro('fat', random.randint(3, 5))
        self.add_macro('carbs', random.randint(3, 5))
        
        # Coin milestone (every 10 handshakes) - use pwnagotchi's count
        if handshake_count > 0 and handshake_count % 10 == 0:
            self.add_coins(5)
        
        return result
    
    def on_pmkid(self) -> dict:
        """Called when PMKID captured (harder, more reward)."""
        result = self.add_xp(250)
        
        self.add_macro('protein', random.randint(12, 18))
        self.add_macro('fat', random.randint(5, 8))
        self.add_macro('carbs', random.randint(5, 8))
        
        # PMKIDs are rare, always give coins
        self.add_coins(2)
        
        return result
    
    def on_new_ap(self) -> dict:
        """Called when new AP discovered."""
        result = self.add_xp(50)
        
        self.add_macro('protein', random.randint(3, 5))
        self.add_macro('fat', random.randint(3, 5))
        self.add_macro('carbs', random.randint(8, 12))
        
        return result
    
    def on_association(self):
        """Called on association."""
        self.add_macro('protein', random.randint(4, 6))
        self.add_macro('fat', random.randint(4, 6))
        self.add_macro('carbs', random.randint(4, 6))
    
    def on_deauth(self):
        """Called when deauth sent."""
        self.consume_macro('protein', 0.1)
    
    def on_battle(self, won: bool, xp_delta: int) -> dict:
        """Called after battle resolved."""
        # Update battle stats
        self.stats['battles']['total'] += 1
        
        if won:
            self.stats['battles']['wins'] += 1
            self.stats['battles']['win_streak'] += 1
            if self.stats['battles']['win_streak'] > self.stats['battles']['best_streak']:
                self.stats['battles']['best_streak'] = self.stats['battles']['win_streak']
            
            # Macro gains from win
            self.add_macro('protein', 5)
            self.add_macro('fat', 5)
            self.add_macro('carbs', 5)
            
            # Coins from win
            self.add_coins(3)
            
            # Win streak bonuses
            streak = self.stats['battles']['win_streak']
            if streak >= 3:
                self.add_macro('protein', random.randint(3, 5))
                self.add_macro('fat', random.randint(3, 5))
                self.add_macro('carbs', random.randint(3, 5))
            if streak == 5:
                self.add_coins(5)
            if streak == 10:
                self.add_coins(15)
        else:
            self.stats['battles']['losses'] += 1
            self.stats['battles']['win_streak'] = 0
            
            # Small macro gains from loss (participation)
            self.add_macro('protein', 2)
            self.add_macro('fat', 2)
            self.add_macro('carbs', 2)
        
        # XP from battle
        result = self.add_xp(xp_delta)
        
        return result
    
    # ========== API METHODS ==========
    
    def get_stats(self, agent=None) -> dict:
        """GET_STATS API - Returns full stats for app.
        
        Combines:
        - PwnHub data from JSON (macros, battles, coins, etc.)
        - Live data from pwnagotchi agent (handshakes, epochs, etc.)
        """
        # Update mood before returning
        self.calculate_mood()
        
        response = {
            "status": "ok",
            "device_id": self.stats['device_id'],
            "name": self.stats.get('name', 'pwnagotchi'),
            "version": self.stats['version'],
            
            # PwnHub-specific (from JSON)
            "progression": self.stats['progression'].copy(),
            "macros": self.stats['macros'].copy(),
            "mood": self.stats['mood'].copy(),
            "battles": self.stats['battles'].copy(),
            "xp_multiplier": round(self.get_total_xp_multiplier(), 2),
            "achievements": self.stats['achievements'].copy(),
            
            "last_battle": self.stats.get('last_battle_time', 0),
            "created": self.stats['created'],
            "updated": self.stats['updated']
        }
        
        # Live stats from pwnagotchi agent (NOT stored in our JSON)
        if agent:
            try:
                # Read directly from agent - this is pwnagotchi's source of truth
                handshakes = 0
                epochs = 0
                
                if hasattr(agent, 'handshakes'):
                    handshakes = agent.handshakes() if callable(agent.handshakes) else agent.handshakes
                if hasattr(agent, 'epoch'):
                    epochs = agent.epoch
                
                # Try to get more stats from agent's brain/session
                unique_aps = 0
                deauths = 0
                if hasattr(agent, '_view') and agent._view:
                    # Could read from view state if available
                    pass
                
                response['capture_stats'] = {
                    'handshakes': handshakes,
                    'epochs': epochs,
                    'unique_aps': unique_aps,
                    'deauths_sent': deauths
                }
            except Exception as e:
                logging.debug(f"[{PLUGIN_NAME}] Could not read agent stats: {e}")
                response['capture_stats'] = {
                    'handshakes': 0,
                    'epochs': 0,
                    'unique_aps': 0,
                    'deauths_sent': 0
                }
        
        return response
    
    def get_id(self) -> dict:
        """GET_ID API - Returns device identity."""
        return {
            "status": "ok",
            "device_id": self.stats['device_id'],
            "name": self.stats.get('name', 'pwnagotchi')
        }
    
    def apply_update(self, battle: dict, cooldown_seconds: int = 30) -> dict:
        """APPLY_UPDATE API - Validates and applies battle result."""
        # Validation
        battle_id = battle.get('battle_id')
        if not battle_id:
            return {"status": "error", "code": "MISSING_BATTLE_ID"}
        
        if battle_id in self.stats['seen_battle_ids']:
            return {"status": "error", "code": "DUPLICATE_BATTLE"}
        
        if time.time() - self.stats['last_battle_time'] < cooldown_seconds:
            return {"status": "error", "code": "COOLDOWN_ACTIVE"}
        
        result_type = battle.get('result')
        if result_type not in ('win', 'loss'):
            return {"status": "error", "code": "INVALID_RESULT"}
        
        xp_delta = battle.get('xp_delta', 0)
        if result_type == 'win' and not (50 <= xp_delta <= 500):
            return {"status": "error", "code": "INVALID_DELTA"}
        if result_type == 'loss' and not (10 <= xp_delta <= 100):
            return {"status": "error", "code": "INVALID_DELTA"}
        
        # Apply battle
        won = result_type == 'win'
        self.on_battle(won, xp_delta)
        
        # Record battle
        self.stats['seen_battle_ids'].append(battle_id)
        self.stats['battle_history'].append({
            'battle_id': battle_id,
            'opponent_id': battle.get('opponent_id', 'unknown'),
            'opponent_name': battle.get('opponent_name', 'unknown'),
            'result': result_type,
            'xp_gained': xp_delta,
            'timestamp': time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
        })
        self.stats['last_battle_time'] = time.time()
        
        # Trim history
        if len(self.stats['battle_history']) > self.max_history:
            self.stats['battle_history'] = self.stats['battle_history'][-self.max_history:]
        if len(self.stats['seen_battle_ids']) > self.max_history * 2:
            self.stats['seen_battle_ids'] = self.stats['seen_battle_ids'][-self.max_history:]
        
        self._save()
        
        return {
            "status": "ok",
            "new_stats": {
                "level": self.stats['progression']['level'],
                "xp": self.stats['progression']['xp'],
                "stage": self.stats['progression']['stage'],
                "title": self.stats['progression']['stage_title'],
                "wins": self.stats['battles']['wins'],
                "win_streak": self.stats['battles']['win_streak']
            }
        }
    
    def save(self):
        """Public save method."""
        self._save()
    
    # ========== DISPLAY HELPERS ==========
    
    def get_display_data(self) -> dict:
        """Get data formatted for PwnaUI display."""
        prog = self.stats['progression']
        macros = self.stats['macros']
        battles = self.stats['battles']
        
        # Calculate XP percentage
        current_level_xp = xp_for_level(prog['level'])
        next_level_xp = xp_for_level(prog['level'] + 1)
        xp_in_level = prog['xp'] - current_level_xp
        xp_needed = next_level_xp - current_level_xp
        xp_percent = int((xp_in_level / xp_needed) * 100) if xp_needed > 0 else 100
        
        return {
            'protein': int(macros['protein']['charges']),
            'fat': int(macros['fat']['charges']),
            'carbs': int(macros['carbs']['charges']),
            'xp_percent': xp_percent,
            'level': prog['level'],
            'title': prog['stage_title'],
            'wins': battles['wins'],
            'total_battles': battles['total'],
            'mood': self.stats['mood']['state'],
            'xp_mult': round(self.get_total_xp_multiplier(), 2)
        }


# =============================================================================
# PLUGIN CLASS
# =============================================================================

class PwnHubStats(plugins.Plugin):
    __author__ = 'james'
    __version__ = PLUGIN_VERSION
    __license__ = 'GPL3'
    __description__ = 'Pet system & battle stats for PwnHub mobile app'
    __name__ = PLUGIN_NAME
    __help__ = 'XP, Macros, Coins, Battles - Tamagotchi for Pwnagotchi'
    __dependencies__ = []
    __defaults__ = {
        'enabled': False,
        'stats_file': '/root/pwnhub_stats.json',
        'cooldown_seconds': 30,
        'max_history': 100,
        'show_on_display': True,
        'compact_mode': False
    }

    def __init__(self):
        self.pet = None
        self.ready = False
        self._last_handshake_count = 0
        self._unique_aps = set()
        self._agent = None

    def _update_display(self):
        """Send current stats to PwnaUI C daemon for display."""
        if not self.ready or not self.options.get('show_on_display', True):
            return
        if not self.pet:
            return
        try:
            data = self.pet.get_display_data()
            _send_pwnaui_command('SET_PWNHUB_ENABLED 1')
            _send_pwnaui_command('SET_PWNHUB_MACROS {} {} {}'.format(
                data['protein'], data['fat'], data['carbs']))
            _send_pwnaui_command('SET_PWNHUB_XP {}'.format(data['xp_percent']))
            _send_pwnaui_command('SET_PWNHUB_STAGE {} {} {} {}'.format(
                data['title'], data['level'], data['wins'], data['total_battles']))
        except Exception:
            pass  # Silent fail - display is optional

    def on_loaded(self):
        """Called when plugin is loaded."""
        stats_file = self.options.get('stats_file', '/root/pwnhub_stats.json')
        max_history = self.options.get('max_history', 100)
        
        self.pet = PetStats(stats_file, max_history)
        self.ready = True
        
        level = self.pet.stats['progression']['level']
        title = self.pet.stats['progression']['stage_title']
        xp_mult = self.pet.get_total_xp_multiplier()
        
        logging.info(f"[{PLUGIN_NAME}] Loaded! {title} Level {level} ({xp_mult:.0%} XP mult)")
        logging.info(f"[{PLUGIN_NAME}] Macros: {self.pet.get_macro_summary()}")
        
        # Send initial stats to display
        self._update_display()

    def on_ready(self, agent):
        """Called when agent is ready."""
        self._agent = agent
        
        # Update name from agent
        if hasattr(agent, 'name'):
            self.pet.stats['name'] = agent.name()
        
        logging.info(f"[{PLUGIN_NAME}] Ready! Device: {self.pet.stats['device_id']}")

    def on_epoch(self, agent, epoch, epoch_data):
        """Called each epoch - consume macros."""
        if not self.ready:
            return
        
        self._agent = agent
        
        # Check if blind epoch (no hops)
        blind = epoch_data.get('blind_for_epochs', 0) > 0 if epoch_data else False
        
        # Get aggression from agent config
        aggression = 0.5
        try:
            if hasattr(agent, '_config'):
                aggression = agent._config.get('personality', {}).get('aggression', 0.5)
        except:
            pass
        
        result = self.pet.on_epoch(blind=blind, aggression=aggression)
        self.pet.save()
        
        # Log XP gain and macros periodically
        if not blind and result.get('xp_gained', 0) > 0:
            logging.debug(f"[{PLUGIN_NAME}] Epoch {epoch}: +{result['xp_gained']} XP, {self.pet.get_macro_summary()}")

    def on_handshake(self, agent, filename, access_point, client_station):
        """Called when handshake captured."""
        if not self.ready:
            return
        
        self._agent = agent
        
        # Get handshake count from pwnagotchi (source of truth)
        handshake_count = 0
        try:
            if hasattr(agent, 'handshakes'):
                handshake_count = agent.handshakes() if callable(agent.handshakes) else agent.handshakes
        except:
            pass
        
        result = self.pet.on_handshake(handshake_count=handshake_count)
        self.pet.save()
        
        mult_str = f"{result['multiplier']:.0%}"
        logging.info(f"[{PLUGIN_NAME}] Handshake #{handshake_count}! +{result['final_xp']} XP ({mult_str}) "
                    f"-> Level {result['new_level']}")
        
        if result['leveled_up']:
            logging.info(f"[{PLUGIN_NAME}] LEVEL UP! Now {result['title']} {result['new_level']}")
        
        # Update display after handshake
        self._update_display()

    def on_association(self, agent, access_point):
        """Called on association."""
        if not self.ready:
            return
        
        self._agent = agent
        self.pet.on_association()
        
        # Track unique APs
        if access_point:
            bssid = access_point.get('mac', '') if isinstance(access_point, dict) else str(access_point)
            if bssid and bssid not in self._unique_aps:
                self._unique_aps.add(bssid)
                result = self.pet.on_new_ap()
                logging.debug(f"[{PLUGIN_NAME}] New AP! +{result['final_xp']} XP")
        
        self.pet.save()

    def on_deauthentication(self, agent, access_point, client_station):
        """Called when deauth sent."""
        if not self.ready:
            return
        
        self._agent = agent
        self.pet.on_deauth()
        # Don't save on every deauth (too frequent)

    def on_unload(self, ui):
        """Called when plugin unloads."""
        if self.pet:
            self.pet.save()
            logging.info(f"[{PLUGIN_NAME}] Saved and unloaded.")
        
        # Disable display
        _send_pwnaui_command('SET_PWNHUB_ENABLED 0')

    # ========== API HANDLERS ==========
    # These would be called by BLE or HTTP handlers
    
    def api_get_stats(self) -> dict:
        """Handle GET_STATS command."""
        if not self.ready:
            return {"status": "error", "code": "NOT_READY"}
        return self.pet.get_stats(self._agent)
    
    def api_get_id(self) -> dict:
        """Handle GET_ID command."""
        if not self.ready:
            return {"status": "error", "code": "NOT_READY"}
        return self.pet.get_id()
    
    def api_apply_update(self, battle: dict) -> dict:
        """Handle APPLY_UPDATE command."""
        if not self.ready:
            return {"status": "error", "code": "NOT_READY"}
        cooldown = self.options.get('cooldown_seconds', 30)
        result = self.pet.apply_update(battle, cooldown)
        return result
    
    def api_get_display(self) -> dict:
        """Get display data for PwnaUI."""
        if not self.ready:
            return {}
        return self.pet.get_display_data()
