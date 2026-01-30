"""
PwnaUI Themes Plugin for Pwnagotchi

Integrates the PwnaUI theme system with Pwnagotchi:
- Adds theme selection to web UI
- Replaces default voice with themed voice (PROPERLY!)
- Syncs theme state with C daemon

Installation:
    1. Copy to /home/pi/pwnaui/plugins/pwnaui_themes.py
    2. Enable in config.toml:
       [main.plugins.pwnaui_themes]
       enabled = true
       theme = "egirl-pwnagotchi"  # optional default

The FIX: This plugin monkey-patches pwnagotchi's view._voice 
to use the theme's voice messages. No file copying, no restart needed.
"""

import os
import sys
import logging
import pwnagotchi.plugins as plugins
from flask import render_template_string

# Add pwnaui python path
sys.path.insert(0, '/home/pi/pwnaui/python')
sys.path.insert(0, '/home/pi/pwnaui')

try:
    from themes import ThemeManager, get_theme_manager, ThemeVoice
    HAS_THEME_MANAGER = True
except ImportError as e:
    logging.error(f"[pwnaui_themes] Import failed: {e}")
    HAS_THEME_MANAGER = False
    ThemeManager = None


class VoiceAdapter:
    """
    Adapts PwnaUI ThemeVoice to pwnagotchi's Voice interface.
    
    This is the KEY to proper theme voice support:
    - Wraps ThemeVoice and implements ALL pwnagotchi Voice methods
    - Adds [TAG] prefix if not already present
    - Falls back to default messages for missing methods
    - Handles both direct Voice class calls AND ThemeVoice.get_message() style
    """
    
    def __init__(self, theme_voice, theme_name, lang='en'):
        self.theme_voice = theme_voice
        self.theme_name = theme_name
        self.lang = lang
        self.tag = f"[{theme_name.upper().replace('-', '')}]"
        
        # Check if we have direct voice_instance (from ThemeVoice wrapper)
        # or a direct Voice class
        if hasattr(theme_voice, 'voice_instance') and theme_voice.voice_instance:
            self._voice = theme_voice.voice_instance
        else:
            self._voice = theme_voice
    
    def _call_voice(self, method_name, *args, **kwargs):
        """Try to call voice method - handles both direct and wrapped styles"""
        try:
            # First try direct call on voice instance
            if hasattr(self._voice, method_name):
                method = getattr(self._voice, method_name)
                # Try with args first, then kwargs, then no args
                try:
                    if args:
                        return self._ensure_tag(method(*args))
                    elif kwargs:
                        return self._ensure_tag(method(**kwargs))
                    else:
                        return self._ensure_tag(method())
                except TypeError:
                    # Method signature mismatch, try without args
                    try:
                        return self._ensure_tag(method())
                    except:
                        pass
            
            # Fall back to ThemeVoice.get_message() style
            if hasattr(self.theme_voice, 'get_message'):
                msg = self.theme_voice.get_message(method_name, **kwargs)
                return self._ensure_tag(msg)
                
        except Exception as e:
            logging.debug(f"[pwnaui_themes] Voice method {method_name} failed: {e}")
        
        return f"{self.tag} ..."
    
    def _ensure_tag(self, msg):
        """Ensure message has theme tag prefix"""
        if msg and not msg.startswith('['):
            return f"{self.tag} {msg}"
        return msg or f"{self.tag} ..."
    
    # ============ ALL PWNAGOTCHI VOICE METHODS ============
    
    def custom(self, s):
        return self._ensure_tag(s)
    
    def default(self):
        return self._call_voice('default')
    
    def on_starting(self):
        return self._call_voice('on_starting')
    
    def on_keys_generation(self):
        return self._call_voice('on_keys_generation')
    
    def on_normal(self):
        return self._call_voice('on_normal')
    
    def on_free_channel(self, channel):
        return self._call_voice('on_free_channel', channel)
    
    def on_reading_logs(self, lines_so_far=0):
        return self._call_voice('on_reading_logs', lines_so_far)
    
    def on_bored(self):
        return self._call_voice('on_bored')
    
    def on_motivated(self, reward):
        return self._call_voice('on_motivated', reward)
    
    def on_demotivated(self, reward):
        return self._call_voice('on_demotivated', reward)
    
    def on_sad(self):
        return self._call_voice('on_sad')
    
    def on_angry(self):
        return self._call_voice('on_angry')
    
    def on_excited(self):
        return self._call_voice('on_excited')
    
    def on_new_peer(self, peer):
        name = peer.name() if hasattr(peer, 'name') else str(peer)
        return self._call_voice('on_new_peer', name)
    
    def on_lost_peer(self, peer):
        name = peer.name() if hasattr(peer, 'name') else str(peer)
        return self._call_voice('on_lost_peer', name)
    
    def on_miss(self, who):
        return self._call_voice('on_miss', who)
    
    def on_grateful(self):
        return self._call_voice('on_grateful')
    
    def on_lonely(self):
        return self._call_voice('on_lonely')
    
    def on_napping(self, secs):
        return self._call_voice('on_napping', secs)
    
    def on_shutdown(self):
        return self._call_voice('on_shutdown')
    
    def on_awakening(self):
        return self._call_voice('on_awakening')
    
    def on_waiting(self, secs):
        return self._call_voice('on_waiting', secs)
    
    def on_assoc(self, ap):
        return self._call_voice('on_assoc', ap)
    
    def on_deauth(self, sta):
        return self._call_voice('on_deauth', sta)
    
    def on_handshakes(self, new_shakes):
        return self._call_voice('on_handshakes', new_shakes)
    
    def on_unread_messages(self, count, total):
        return self._call_voice('on_unread_messages', count, total)
    
    def on_rebooting(self):
        return self._call_voice('on_rebooting')
    
    def on_uploading(self, to):
        return self._call_voice('on_uploading', to)
    
    def on_downloading(self, name):
        return self._call_voice('on_downloading', name)
    
    def on_last_session_data(self, last_session):
        return self._call_voice('on_last_session_data', last_session)
    
    def on_last_session_tweet(self, last_session):
        return self._call_voice('on_last_session_tweet', last_session)
    
    def on_ai_ready(self):
        return self._call_voice('on_ai_ready')
    
    def on_epoch(self, epoch, epoch_data):
        return self._call_voice('on_epoch', epoch, epoch_data)
    
    def on_wait(self, secs):
        return self._call_voice('on_wait', secs)
    
    def on_sleep(self, secs):
        return self._call_voice('on_sleep', secs)
    
    def on_wake(self):
        return self._call_voice('on_wake')
    
    def on_manual_mode(self, last_session):
        return self._call_voice('on_manual_mode', last_session)
    
    def hhmmss(self, count, fmt):
        """Time formatting - pass through to default behavior"""
        # This is used for duration formatting, not themed messages
        if fmt == 'h':
            return 'hour' if count == 1 else 'hours'
        elif fmt == 'm':
            return 'min' if count == 1 else 'mins'
        elif fmt == 's':
            return 'sec' if count == 1 else 'secs'
        return fmt


# HTML template for theme selection UI
THEMES_HTML = """
<!DOCTYPE html>
<html>
<head>
    <title>PwnaUI Themes</title>
    <style>
        body { font-family: monospace; background: #1a1a2e; color: #eee; padding: 20px; }
        .theme-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(200px, 1fr)); gap: 20px; }
        .theme-card { background: #16213e; border-radius: 8px; padding: 15px; cursor: pointer; transition: all 0.2s; }
        .theme-card:hover { transform: scale(1.02); background: #1f4068; }
        .theme-card.active { border: 2px solid #4ecca3; }
        .theme-name { font-size: 1.2em; margin-bottom: 10px; color: #4ecca3; }
        .theme-info { font-size: 0.9em; color: #888; }
        h1 { color: #4ecca3; }
        .status { padding: 10px; margin: 10px 0; border-radius: 4px; }
        .status.success { background: #1b4332; }
        .status.error { background: #4a1515; }
    </style>
</head>
<body>
    <h1>🎨 PwnaUI Themes</h1>
    <div id="status"></div>
    <div id="themes" class="theme-grid">Loading...</div>
    <script>
        async function loadThemes() {
            const resp = await fetch('/plugins/pwnaui_themes/api/themes');
            const data = await resp.json();
            const container = document.getElementById('themes');
            container.innerHTML = '';
            data.themes.forEach(theme => {
                const card = document.createElement('div');
                card.className = 'theme-card' + (theme.name === data.active ? ' active' : '');
                card.innerHTML = `
                    <div class="theme-name">${theme.name}</div>
                    <div class="theme-info">${theme.faces_count || 0} faces</div>
                    <div class="theme-info">${theme.has_voice ? '🔊 Voice' : '🔇 No voice'}</div>
                `;
                card.onclick = () => setTheme(theme.name);
                container.appendChild(card);
            });
        }
        async function setTheme(name) {
            const resp = await fetch('/plugins/pwnaui_themes/api/themes/set', {
                method: 'POST',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify({theme: name})
            });
            const data = await resp.json();
            const status = document.getElementById('status');
            status.className = 'status ' + (data.success ? 'success' : 'error');
            status.textContent = data.message;
            if (data.success) loadThemes();
        }
        loadThemes();
    </script>
</body>
</html>
"""


class PwnaUIThemes(plugins.Plugin):
    __author__ = 'PwnaUI'
    __version__ = '2.0.0'
    __license__ = 'MIT'
    __description__ = 'Theme system with PROPER voice support'
    __name__ = 'pwnaui_themes'
    __help__ = 'Custom faces and voices - hot-swappable!'
    __dependencies__ = {}
    __defaults__ = {
        'enabled': False,
        'theme': None,
    }

    def __init__(self):
        self.ready = False
        self.theme_mgr = None
        self._original_voice = None
        self._agent = None
        self._view = None
        logging.debug("[pwnaui_themes] Plugin initialized")

    def on_loaded(self):
        """Called when plugin is loaded"""
        if not HAS_THEME_MANAGER:
            logging.error("[pwnaui_themes] Theme system not available")
            return

        try:
            self.theme_mgr = get_theme_manager()
            themes = self.theme_mgr.list_themes()
            logging.info(f"[pwnaui_themes] Found {len(themes)} themes: {', '.join(themes[:5])}...")
        except Exception as e:
            logging.error(f"[pwnaui_themes] Failed to init: {e}")
            return

        # Set default theme if configured
        default_theme = self.options.get('theme')
        if default_theme:
            if self.theme_mgr.set_active_theme(default_theme):
                logging.info(f"[pwnaui_themes] Default theme: '{default_theme}'")
            else:
                logging.warning(f"[pwnaui_themes] Theme '{default_theme}' not found")

        self.ready = True
        logging.info("[pwnaui_themes] Plugin loaded - v2.0 with voice patching!")

    def on_ready(self, agent):
        """Called when agent is ready - THIS IS WHERE WE PATCH THE VOICE"""
        self._agent = agent
        
        try:
            self._view = agent.view()
        except:
            # Try alternate access
            if hasattr(agent, '_view'):
                self._view = agent._view
            else:
                logging.error("[pwnaui_themes] Cannot access view!")
                return

        # Store original voice for restore
        if hasattr(self._view, '_voice'):
            self._original_voice = self._view._voice
            logging.debug("[pwnaui_themes] Stored original voice")

        # Apply theme voice
        if self.ready and self.theme_mgr and self.theme_mgr.active_theme:
            self._hook_voice()

    def _hook_voice(self):
        """
        THE FIX: Replace pwnagotchi's Voice with theme's voice.
        No file copying. No restart. Just monkey-patching.
        """
        if not self._view:
            logging.warning("[pwnaui_themes] No view to patch")
            return False

        theme = self.theme_mgr.active_theme
        if not theme:
            logging.info("[pwnaui_themes] No active theme")
            return False

        if not theme.voice:
            logging.warning(f"[pwnaui_themes] Theme '{theme.name}' has no voice.py")
            return False

        try:
            # Create adapter that wraps theme voice
            adapter = VoiceAdapter(theme.voice, theme.name)
            
            # MONKEY-PATCH: Replace the view's voice instance
            self._view._voice = adapter
            
            logging.info(f"[pwnaui_themes] ✓ Voice patched to '{theme.name}'")
            
            # Update status to show it's working
            try:
                self._view.set('status', adapter.on_starting())
            except:
                pass
            
            return True
            
        except Exception as e:
            logging.error(f"[pwnaui_themes] Failed to hook voice: {e}")
            import traceback
            traceback.print_exc()
            return False

    def _restore_voice(self):
        """Restore original voice"""
        if self._original_voice and self._view:
            self._view._voice = self._original_voice
            logging.info("[pwnaui_themes] Voice restored to default")

    def on_webhook(self, path, request):
        """Handle web requests"""
        if not self.ready:
            return {"success": False, "message": "Plugin not ready"}, 503

        # Serve main UI
        if path == "/" or path == "":
            return render_template_string(THEMES_HTML)

        # API routes
        if path.startswith("/api/"):
            api_path = path[4:]

            if api_path == "/themes" or api_path == "/themes/":
                themes = []
                for name in self.theme_mgr.list_themes():
                    theme = self.theme_mgr.get_theme(name)
                    if theme:
                        themes.append({
                            'name': theme.name,
                            'faces_count': len(theme.faces) if hasattr(theme, 'faces') else 0,
                            'has_voice': theme.voice is not None
                        })
                return {
                    "success": True,
                    "themes": themes,
                    "active": self.theme_mgr.active_theme.name if self.theme_mgr.active_theme else None
                }

            elif api_path == "/themes/current":
                theme = self.theme_mgr.active_theme
                return {
                    "success": True,
                    "theme": theme.name if theme else None,
                    "has_voice": theme.voice is not None if theme else False
                }

            elif api_path == "/themes/set" and request.method == "POST":
                data = request.get_json() or {}
                theme_name = data.get('theme')

                if self.theme_mgr.set_active_theme(theme_name):
                    self.options['theme'] = theme_name
                    
                    # HOT-SWAP: Re-apply voice patch
                    voice_ok = self._hook_voice()
                    
                    msg = f"Theme set to '{theme_name}'"
                    if voice_ok:
                        msg += " (voice updated!)"
                    else:
                        msg += " (no voice.py)"
                    
                    return {"success": True, "message": msg}
                else:
                    return {"success": False, "message": f"Theme '{theme_name}' not found"}, 400

        return {"success": False, "message": "Not found"}, 404

    def on_ui_setup(self, ui):
        """Called to setup UI elements"""
        pass

    def on_ui_update(self, ui):
        """Called on UI update"""
        pass

    def on_unload(self, ui):
        """Called when plugin is unloaded"""
        self._restore_voice()
