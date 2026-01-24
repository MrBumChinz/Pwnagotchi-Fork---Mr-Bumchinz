"""
PwnaUI Themes Plugin for Pwnagotchi v3.0.0

Clean architecture rewrite with proper voice patching for both MANU and AUTO modes.

The key insight: Pwnagotchi's hooks fire in different orders depending on mode:
    - AUTO mode: on_loaded -> on_ui_setup -> on_ready (bettercap connects)
    - MANU mode: on_loaded -> on_ui_setup -> (on_ready NEVER fires!)

Solution: Patch voice as soon as BOTH conditions are met:
    1. Theme manager is ready (self._theme_ready = True, set in on_loaded)
    2. View is available (self._view_ready = True, from on_ui_setup or on_ready)

Installation:
    1. Copy to /root/custom_plugins/pwnaui_themes.py
    2. Enable in config.toml:
       [main.plugins.pwnaui_themes]
       enabled = true
       theme = "rick-sanchez"  # optional default theme
"""

import os
import sys
import logging
import pwnagotchi.plugins as plugins
from flask import render_template_string

__author__ = 'PwnaUI'
__version__ = '3.0.0'
__license__ = 'MIT'
__description__ = 'Theme system with voice support - works in MANU and AUTO modes'

# Add pwnaui python path
sys.path.insert(0, '/home/pi/pwnaui/python')
sys.path.insert(0, '/home/pi/pwnaui')

try:
    from themes import ThemeManager, get_theme_manager, ThemeVoice
    HAS_THEME_MANAGER = True
except ImportError as e:
    logging.error(f"[pwnaui_themes] Import failed: {e}")
    HAS_THEME_MANAGER = False


# =============================================================================
# VOICE ADAPTER - Bridges PwnaUI themes to Pwnagotchi's Voice interface
# =============================================================================

class VoiceAdapter:
    """
    Adapts PwnaUI ThemeVoice to pwnagotchi's Voice interface.
    
    Implements ALL pwnagotchi Voice methods by delegating to theme's voice.py.
    Adds theme tag prefix [THEMENAME] to messages.
    Falls back gracefully for missing methods.
    """
    
    def __init__(self, theme_voice, theme_name, lang='en'):
        self.theme_voice = theme_voice
        self.theme_name = theme_name
        self.lang = lang
        self.tag = f"[{theme_name.upper().replace('-', '')}]"
        
        # Get the actual voice instance (handles ThemeVoice wrapper)
        if hasattr(theme_voice, 'voice_instance') and theme_voice.voice_instance:
            self._voice = theme_voice.voice_instance
        else:
            self._voice = theme_voice
    
    def _call(self, method_name, *args, **kwargs):
        """Call a voice method with fallback handling"""
        try:
            if hasattr(self._voice, method_name):
                method = getattr(self._voice, method_name)
                try:
                    if args:
                        result = method(*args)
                    elif kwargs:
                        result = method(**kwargs)
                    else:
                        result = method()
                    return self._tag(result)
                except TypeError:
                    # Method signature mismatch - try no args
                    try:
                        return self._tag(method())
                    except:
                        pass
            
            # Fallback to ThemeVoice.get_message() style
            if hasattr(self.theme_voice, 'get_message'):
                return self._tag(self.theme_voice.get_message(method_name, **kwargs))
                
        except Exception as e:
            logging.debug(f"[pwnaui_themes] Voice.{method_name} failed: {e}")
        
        return f"{self.tag} ..."
    
    def _tag(self, msg):
        """Add theme tag prefix if not already present"""
        if msg and not msg.startswith('['):
            return f"{self.tag} {msg}"
        return msg or f"{self.tag} ..."
    
    # =========================================================================
    # PWNAGOTCHI VOICE INTERFACE - All methods pwnagotchi may call
    # =========================================================================
    
    def custom(self, s):
        return self._tag(s)
    
    def default(self):
        return self._call('default')
    
    def on_starting(self):
        return self._call('on_starting')
    
    def on_keys_generation(self):
        return self._call('on_keys_generation')
    
    def on_normal(self):
        return self._call('on_normal')
    
    def on_free_channel(self, channel):
        return self._call('on_free_channel', channel)
    
    def on_reading_logs(self, lines_so_far=0):
        return self._call('on_reading_logs', lines_so_far)
    
    def on_bored(self):
        return self._call('on_bored')
    
    def on_motivated(self, reward):
        return self._call('on_motivated', reward)
    
    def on_demotivated(self, reward):
        return self._call('on_demotivated', reward)
    
    def on_sad(self):
        return self._call('on_sad')
    
    def on_angry(self):
        return self._call('on_angry')
    
    def on_excited(self):
        return self._call('on_excited')
    
    def on_new_peer(self, peer):
        name = peer.name() if hasattr(peer, 'name') else str(peer)
        return self._call('on_new_peer', name)
    
    def on_lost_peer(self, peer):
        name = peer.name() if hasattr(peer, 'name') else str(peer)
        return self._call('on_lost_peer', name)
    
    def on_miss(self, who):
        return self._call('on_miss', who)
    
    def on_grateful(self):
        return self._call('on_grateful')
    
    def on_lonely(self):
        return self._call('on_lonely')
    
    def on_napping(self, secs):
        return self._call('on_napping', secs)
    
    def on_shutdown(self):
        return self._call('on_shutdown')
    
    def on_awakening(self):
        return self._call('on_awakening')
    
    def on_waiting(self, secs):
        return self._call('on_waiting', secs)
    
    def on_assoc(self, ap):
        return self._call('on_assoc', ap)
    
    def on_deauth(self, sta):
        return self._call('on_deauth', sta)
    
    def on_handshakes(self, new_shakes):
        return self._call('on_handshakes', new_shakes)
    
    def on_unread_messages(self, count, total):
        return self._call('on_unread_messages', count, total)
    
    def on_rebooting(self):
        return self._call('on_rebooting')
    
    def on_uploading(self, to):
        return self._call('on_uploading', to)
    
    def on_downloading(self, name):
        return self._call('on_downloading', name)
    
    def on_last_session_data(self, last_session):
        return self._call('on_last_session_data', last_session)
    
    def on_last_session_tweet(self, last_session):
        return self._call('on_last_session_tweet', last_session)
    
    def on_ai_ready(self):
        return self._call('on_ai_ready')
    
    def on_epoch(self, epoch, epoch_data):
        return self._call('on_epoch', epoch, epoch_data)
    
    def on_wait(self, secs):
        return self._call('on_wait', secs)
    
    def on_sleep(self, secs):
        return self._call('on_sleep', secs)
    
    def on_wake(self):
        return self._call('on_wake')
    
    def on_manual_mode(self, last_session):
        return self._call('on_manual_mode', last_session)
    
    def hhmmss(self, count, fmt):
        """Time unit formatting - not themed"""
        if fmt == 'h':
            return 'hour' if count == 1 else 'hours'
        elif fmt == 'm':
            return 'min' if count == 1 else 'mins'
        elif fmt == 's':
            return 'sec' if count == 1 else 'secs'
        return fmt


# =============================================================================
# WEB UI - Theme selection interface
# =============================================================================

THEMES_HTML = """
<!DOCTYPE html>
<html>
<head>
    <title>PwnaUI Themes</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        * { box-sizing: border-box; }
        body { 
            font-family: 'Courier New', monospace; 
            background: linear-gradient(135deg, #1a1a2e 0%, #16213e 100%);
            color: #eee; 
            padding: 20px; 
            min-height: 100vh;
            margin: 0;
        }
        h1 { 
            color: #4ecca3; 
            text-align: center;
            margin-bottom: 10px;
        }
        .subtitle {
            text-align: center;
            color: #888;
            margin-bottom: 30px;
        }
        .theme-grid { 
            display: grid; 
            grid-template-columns: repeat(auto-fill, minmax(180px, 1fr)); 
            gap: 15px;
            max-width: 1200px;
            margin: 0 auto;
        }
        .theme-card { 
            background: rgba(255,255,255,0.05);
            border: 1px solid rgba(255,255,255,0.1);
            border-radius: 12px; 
            padding: 20px; 
            cursor: pointer; 
            transition: all 0.3s ease;
            text-align: center;
        }
        .theme-card:hover { 
            transform: translateY(-5px); 
            background: rgba(78, 204, 163, 0.1);
            border-color: #4ecca3;
            box-shadow: 0 10px 30px rgba(0,0,0,0.3);
        }
        .theme-card.active { 
            border: 2px solid #4ecca3;
            background: rgba(78, 204, 163, 0.15);
        }
        .theme-name { 
            font-size: 1.1em; 
            margin-bottom: 8px; 
            color: #4ecca3;
            font-weight: bold;
        }
        .theme-info { 
            font-size: 0.85em; 
            color: #888;
            margin: 4px 0;
        }
        .voice-badge {
            display: inline-block;
            padding: 2px 8px;
            border-radius: 10px;
            font-size: 0.75em;
            margin-top: 8px;
        }
        .voice-badge.has-voice { background: #1b4332; color: #4ecca3; }
        .voice-badge.no-voice { background: #4a1515; color: #ff6b6b; }
        .status { 
            padding: 15px; 
            margin: 20px auto;
            border-radius: 8px;
            text-align: center;
            max-width: 600px;
            display: none;
        }
        .status.show { display: block; }
        .status.success { background: rgba(27, 67, 50, 0.8); border: 1px solid #4ecca3; }
        .status.error { background: rgba(74, 21, 21, 0.8); border: 1px solid #ff6b6b; }
        .loading { text-align: center; padding: 40px; color: #888; }
    </style>
</head>
<body>
    <h1>🎨 PwnaUI Themes</h1>
    <p class="subtitle">v3.0.0 - Select a theme to change face and voice</p>
    <div id="status" class="status"></div>
    <div id="themes" class="theme-grid">
        <div class="loading">Loading themes...</div>
    </div>
    <script>
        const API = '/plugins/pwnaui_themes/api';
        
        function showStatus(msg, isError) {
            const el = document.getElementById('status');
            el.textContent = msg;
            el.className = 'status show ' + (isError ? 'error' : 'success');
            setTimeout(() => el.classList.remove('show'), 3000);
        }
        
        async function loadThemes() {
            try {
                const resp = await fetch(API + '/themes');
                const data = await resp.json();
                
                if (!data.success) {
                    showStatus('Failed to load themes', true);
                    return;
                }
                
                const container = document.getElementById('themes');
                container.innerHTML = '';
                
                data.themes.sort((a, b) => a.name.localeCompare(b.name));
                
                data.themes.forEach(theme => {
                    const card = document.createElement('div');
                    card.className = 'theme-card' + (theme.name === data.active ? ' active' : '');
                    card.innerHTML = `
                        <div class="theme-name">${theme.name}</div>
                        <div class="voice-badge ${theme.has_voice ? 'has-voice' : 'no-voice'}">
                            ${theme.has_voice ? '🔊 Voice' : '🔇 No voice'}
                        </div>
                    `;
                    card.onclick = () => setTheme(theme.name);
                    container.appendChild(card);
                });
            } catch (e) {
                showStatus('Error: ' + e.message, true);
            }
        }
        
        async function setTheme(name) {
            try {
                const resp = await fetch(API + '/themes/set', {
                    method: 'POST',
                    headers: {'Content-Type': 'application/json'},
                    body: JSON.stringify({theme: name})
                });
                const data = await resp.json();
                showStatus(data.message, !data.success);
                if (data.success) loadThemes();
            } catch (e) {
                showStatus('Error: ' + e.message, true);
            }
        }
        
        loadThemes();
    </script>
</body>
</html>
"""


# =============================================================================
# PLUGIN CLASS - Clean state machine architecture
# =============================================================================

class PwnaUIThemes(plugins.Plugin):
    __author__ = __author__
    __version__ = __version__
    __license__ = __license__
    __description__ = __description__
    __name__ = 'pwnaui_themes'
    __help__ = 'Custom faces and voices - works in MANU and AUTO modes!'
    __dependencies__ = {}
    __defaults__ = {
        'enabled': False,
        'theme': None,
    }

    def __init__(self):
        # State flags
        self._theme_ready = False    # Theme manager initialized
        self._view_ready = False     # View obtained
        self._voice_patched = False  # Voice successfully patched
        
        # References
        self.theme_mgr = None
        self._view = None
        self._original_voice = None
        self._agent = None
        
        logging.debug("[pwnaui_themes] Plugin instance created")

    # =========================================================================
    # LIFECYCLE HOOKS - Called by pwnagotchi in order
    # =========================================================================

    def on_loaded(self):
        """
        Hook 1: Plugin loaded. Initialize theme manager.
        Called early in both MANU and AUTO modes.
        """
        if not HAS_THEME_MANAGER:
            logging.error("[pwnaui_themes] Theme system not available")
            return

        try:
            self.theme_mgr = get_theme_manager()
            themes = self.theme_mgr.list_themes()
            logging.info(f"[pwnaui_themes] Found {len(themes)} themes")
        except Exception as e:
            logging.error(f"[pwnaui_themes] Failed to init theme manager: {e}")
            return

        # Set default theme if configured
        default_theme = self.options.get('theme')
        if default_theme:
            if self.theme_mgr.set_active_theme(default_theme):
                logging.info(f"[pwnaui_themes] Default theme: '{default_theme}'")
            else:
                logging.warning(f"[pwnaui_themes] Theme '{default_theme}' not found")

        self._theme_ready = True
        logging.info(f"[pwnaui_themes] v{__version__} loaded")
        
        # Try to patch voice (may have view from previous hook order)
        self._try_patch_voice("on_loaded")

    def on_ui_setup(self, ui):
        """
        Hook 2: UI is being set up. Get view reference.
        Called in both MANU and AUTO modes.
        """
        if self._view_ready:
            return  # Already have view
        
        # Extract view from ui object
        if hasattr(ui, '_view'):
            self._view = ui._view
        else:
            self._view = ui
        
        if self._view and hasattr(self._view, '_voice'):
            self._original_voice = self._view._voice
            self._view_ready = True
            logging.debug("[pwnaui_themes] Got view from on_ui_setup")
            
            # Try to patch voice
            self._try_patch_voice("on_ui_setup")

    def on_ready(self, agent):
        """
        Hook 3: Agent ready (bettercap connected).
        ONLY called in AUTO mode! Never fires in MANU mode.
        """
        self._agent = agent
        
        if not self._view_ready:
            try:
                self._view = agent.view()
                if self._view and hasattr(self._view, '_voice'):
                    self._original_voice = self._view._voice
                    self._view_ready = True
                    logging.debug("[pwnaui_themes] Got view from on_ready")
            except Exception as e:
                logging.error(f"[pwnaui_themes] Cannot get view: {e}")
                return
        
        # Try to patch voice
        self._try_patch_voice("on_ready")

    # =========================================================================
    # VOICE PATCHING - Single unified method
    # =========================================================================

    def _try_patch_voice(self, caller):
        """
        Attempt to patch voice. Called from multiple hooks.
        Only patches when BOTH theme_ready AND view_ready.
        """
        # Check preconditions
        if self._voice_patched:
            return True  # Already patched
        
        if not self._theme_ready:
            logging.debug(f"[pwnaui_themes] {caller}: theme not ready yet")
            return False
        
        if not self._view_ready or not self._view:
            logging.debug(f"[pwnaui_themes] {caller}: view not ready yet")
            return False
        
        # Get active theme
        theme = self.theme_mgr.active_theme
        if not theme:
            logging.debug(f"[pwnaui_themes] {caller}: no active theme")
            return False
        
        if not theme.voice:
            logging.info(f"[pwnaui_themes] Theme '{theme.name}' has no voice.py")
            return False
        
        # Create adapter and patch
        try:
            adapter = VoiceAdapter(theme.voice, theme.name)
            self._view._voice = adapter
            self._voice_patched = True
            
            logging.info(f"[pwnaui_themes] ✓ Voice patched to '{theme.name}' (from {caller})")
            
            # Show themed starting message
            try:
                self._view.set('status', adapter.on_starting())
            except:
                pass
            
            return True
            
        except Exception as e:
            logging.error(f"[pwnaui_themes] Voice patch failed: {e}")
            return False

    def _patch_voice_for_theme(self, theme_name):
        """
        Re-patch voice when theme changes (called from webhook).
        """
        if not self._view:
            logging.warning("[pwnaui_themes] Cannot patch: no view")
            return False
        
        theme = self.theme_mgr.active_theme
        if not theme or not theme.voice:
            logging.info(f"[pwnaui_themes] Theme '{theme_name}' has no voice.py")
            return False
        
        try:
            adapter = VoiceAdapter(theme.voice, theme.name)
            self._view._voice = adapter
            logging.info(f"[pwnaui_themes] ✓ Voice switched to '{theme.name}'")
            
            try:
                self._view.set('status', adapter.on_starting())
            except:
                pass
            
            return True
        except Exception as e:
            logging.error(f"[pwnaui_themes] Voice switch failed: {e}")
            return False

    def _restore_voice(self):
        """Restore original voice on unload"""
        if self._original_voice and self._view:
            self._view._voice = self._original_voice
            self._voice_patched = False
            logging.info("[pwnaui_themes] Voice restored to default")

    # =========================================================================
    # WEB INTERFACE
    # =========================================================================

    def on_webhook(self, path, request):
        """Handle web requests for theme UI and API"""
        logging.debug(f"[pwnaui_themes] Webhook: path={path!r}")
        
        if not self._theme_ready:
            return {"success": False, "message": "Plugin not ready"}, 503

        # Normalize path - pwnagotchi sends None for root
        if path is None:
            path = ""

        # Main UI (root path)
        if path in ("", "/", "None"):
            try:
                return render_template_string(THEMES_HTML)
            except Exception as e:
                logging.error(f"[pwnaui_themes] Template error: {e}")
                return {"success": False, "message": str(e)}, 500

        # Normalize API path
        api_path = path
        if path.startswith("/api/"):
            api_path = path[4:]
        elif path.startswith("api/"):
            api_path = "/" + path[4:]

        # GET /api/themes - List all themes
        if api_path in ("/themes", "/themes/"):
            themes = []
            for name in self.theme_mgr.list_themes():
                theme = self.theme_mgr.get_theme(name)
                if theme:
                    themes.append({
                        'name': theme.name,
                        'has_voice': theme.voice is not None
                    })
            return {
                "success": True,
                "themes": themes,
                "active": self.theme_mgr.active_theme.name if self.theme_mgr.active_theme else None
            }

        # GET /api/themes/current - Get active theme
        if api_path == "/themes/current":
            theme = self.theme_mgr.active_theme
            return {
                "success": True,
                "theme": theme.name if theme else None,
                "has_voice": theme.voice is not None if theme else False
            }

        # POST /api/themes/set - Set active theme
        if api_path == "/themes/set" and request.method == "POST":
            data = request.get_json() or {}
            theme_name = data.get('theme')

            if not theme_name:
                return {"success": False, "message": "Missing 'theme' parameter"}, 400

            if self.theme_mgr.set_active_theme(theme_name):
                self.options['theme'] = theme_name
                
                # Hot-swap voice
                voice_ok = self._patch_voice_for_theme(theme_name)
                
                msg = f"Theme set to '{theme_name}'"
                if voice_ok:
                    msg += " (voice updated!)"
                else:
                    msg += " (no voice.py)"

                return {"success": True, "message": msg}
            else:
                return {"success": False, "message": f"Theme '{theme_name}' not found"}, 404

        return {"success": False, "message": "Not found"}, 404

    # =========================================================================
    # OTHER HOOKS
    # =========================================================================

    def on_ui_update(self, ui):
        """Called on each UI update - no-op for now"""
        pass

    def on_unload(self, ui):
        """Called when plugin is unloaded"""
        self._restore_voice()
