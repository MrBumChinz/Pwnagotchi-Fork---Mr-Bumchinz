"""
PwnaUI Theme Web API

Provides REST endpoints for theme management:
- GET /api/themes - List available themes
- GET /api/themes/current - Get current theme
- POST /api/themes/set - Set active theme
- GET /api/themes/<name>/preview - Get theme preview image

Can be integrated as a Pwnagotchi plugin or standalone Flask app.
"""

import os
import json
import logging
from flask import Blueprint, jsonify, request, send_file
from typing import Optional

# Try to import our theme manager
try:
    from pwnaui.themes import ThemeManager, get_theme_manager
except ImportError:
    from themes import ThemeManager, get_theme_manager

log = logging.getLogger("pwnaui.web.themes")

# Flask Blueprint for theme routes
themes_bp = Blueprint('themes', __name__, url_prefix='/api/themes')

# Global theme manager reference
_theme_mgr: Optional[ThemeManager] = None


def get_mgr() -> ThemeManager:
    """Get theme manager instance"""
    global _theme_mgr
    if _theme_mgr is None:
        _theme_mgr = get_theme_manager()
    return _theme_mgr


@themes_bp.route('/', methods=['GET'])
@themes_bp.route('', methods=['GET'])
def list_themes():
    """
    List available themes.
    
    Returns:
        JSON with list of themes and their metadata
    """
    mgr = get_mgr()
    mgr.refresh()  # Refresh to catch new themes
    
    themes = []
    for name in mgr.list_themes():
        theme = mgr.get_theme(name)
        if theme:
            themes.append(theme.to_dict())
    
    return jsonify({
        "success": True,
        "themes": themes,
        "active": mgr.active_theme.name if mgr.active_theme else None
    })


@themes_bp.route('/current', methods=['GET'])
def get_current_theme():
    """
    Get current active theme.
    
    Returns:
        JSON with current theme info
    """
    mgr = get_mgr()
    theme = mgr.get_active_theme()
    
    if theme:
        return jsonify({
            "success": True,
            "theme": theme.to_dict()
        })
    else:
        return jsonify({
            "success": True,
            "theme": None
        })


@themes_bp.route('/set', methods=['POST'])
def set_theme():
    """
    Set active theme.
    
    Request body:
        {"theme": "theme_name"} or {"theme": null} to disable
        
    Returns:
        JSON with success status
    """
    data = request.get_json() or {}
    theme_name = data.get('theme')
    
    mgr = get_mgr()
    
    if mgr.set_active_theme(theme_name):
        return jsonify({
            "success": True,
            "message": f"Theme set to '{theme_name}'" if theme_name else "Themes disabled"
        })
    else:
        return jsonify({
            "success": False,
            "message": f"Failed to set theme '{theme_name}'"
        }), 400


@themes_bp.route('/<name>/preview', methods=['GET'])
def get_theme_preview(name: str):
    """
    Get preview image for a theme.
    Returns the HAPPY face as a preview.
    
    Args:
        name: Theme name
        
    Returns:
        PNG image file
    """
    mgr = get_mgr()
    theme = mgr.get_theme(name)
    
    if not theme:
        return jsonify({"success": False, "message": "Theme not found"}), 404
    
    # Try to get HAPPY face as preview
    preview_path = theme.get_face_path("HAPPY")
    if not preview_path or not os.path.exists(preview_path):
        # Try any available face
        for state in ["COOL", "AWAKE", "EXCITED", "BORED"]:
            preview_path = theme.get_face_path(state)
            if preview_path and os.path.exists(preview_path):
                break
    
    if preview_path and os.path.exists(preview_path):
        return send_file(preview_path, mimetype='image/png')
    
    return jsonify({"success": False, "message": "No preview available"}), 404


@themes_bp.route('/<name>/face/<state>', methods=['GET'])
def get_theme_face(name: str, state: str):
    """
    Get specific face image from a theme.
    
    Args:
        name: Theme name
        state: Face state (HAPPY, SAD, etc.)
        
    Returns:
        PNG image file
    """
    mgr = get_mgr()
    theme = mgr.get_theme(name)
    
    if not theme:
        return jsonify({"success": False, "message": "Theme not found"}), 404
    
    face_path = theme.get_face_path(state.upper())
    if face_path and os.path.exists(face_path):
        return send_file(face_path, mimetype='image/png')
    
    return jsonify({"success": False, "message": f"Face '{state}' not found"}), 404


# HTML page for theme selection (can be embedded in Pwnagotchi web UI)
THEMES_HTML = '''
<!DOCTYPE html>
<html>
<head>
    <title>PwnaUI Themes</title>
    <style>
        body {
            font-family: monospace;
            background: #1a1a2e;
            color: #eee;
            margin: 0;
            padding: 20px;
        }
        h1 {
            color: #00ff41;
            border-bottom: 2px solid #00ff41;
            padding-bottom: 10px;
        }
        .theme-grid {
            display: grid;
            grid-template-columns: repeat(auto-fill, minmax(200px, 1fr));
            gap: 20px;
            margin-top: 20px;
        }
        .theme-card {
            background: #16213e;
            border: 2px solid #0f3460;
            border-radius: 8px;
            padding: 15px;
            text-align: center;
            cursor: pointer;
            transition: all 0.3s;
        }
        .theme-card:hover {
            border-color: #00ff41;
            transform: scale(1.02);
        }
        .theme-card.active {
            border-color: #00ff41;
            background: #0f3460;
        }
        .theme-card img {
            width: 100%;
            max-width: 128px;
            height: auto;
            image-rendering: pixelated;
            background: #fff;
            border-radius: 4px;
        }
        .theme-card h3 {
            margin: 10px 0 5px;
            color: #00ff41;
        }
        .theme-card p {
            font-size: 12px;
            color: #888;
            margin: 5px 0;
        }
        .theme-card .face-count {
            font-size: 11px;
            color: #666;
        }
        .btn {
            background: #00ff41;
            color: #1a1a2e;
            border: none;
            padding: 8px 16px;
            border-radius: 4px;
            cursor: pointer;
            font-family: monospace;
            font-weight: bold;
            margin-top: 10px;
        }
        .btn:hover {
            background: #00cc33;
        }
        .btn.disabled {
            background: #333;
            color: #666;
            cursor: not-allowed;
        }
        .status {
            margin-top: 20px;
            padding: 10px;
            border-radius: 4px;
            display: none;
        }
        .status.success {
            display: block;
            background: #1a472a;
            border: 1px solid #00ff41;
        }
        .status.error {
            display: block;
            background: #4a1a1a;
            border: 1px solid #ff4141;
        }
        .current-theme {
            margin-bottom: 20px;
            padding: 10px;
            background: #16213e;
            border-radius: 4px;
        }
        .no-theme {
            background: #333;
            color: #888;
        }
    </style>
</head>
<body>
    <h1>🎨 PwnaUI Themes</h1>
    
    <div class="current-theme" id="currentTheme">
        Loading...
    </div>
    
    <div id="status" class="status"></div>
    
    <div class="theme-grid" id="themeGrid">
        Loading themes...
    </div>

    <script>
        let currentTheme = null;
        
        async function loadThemes() {
            try {
                const resp = await fetch('/api/themes/');
                const data = await resp.json();
                
                currentTheme = data.active;
                updateCurrentTheme();
                renderThemes(data.themes);
            } catch (e) {
                document.getElementById('themeGrid').innerHTML = 
                    '<p style="color: #ff4141;">Error loading themes: ' + e.message + '</p>';
            }
        }
        
        function updateCurrentTheme() {
            const el = document.getElementById('currentTheme');
            if (currentTheme) {
                el.innerHTML = '✅ Current theme: <strong>' + currentTheme + '</strong>';
                el.classList.remove('no-theme');
            } else {
                el.innerHTML = '❌ No theme active (using text rendering)';
                el.classList.add('no-theme');
            }
        }
        
        function renderThemes(themes) {
            const grid = document.getElementById('themeGrid');
            
            // Add "None" option
            let html = `
                <div class="theme-card ${!currentTheme ? 'active' : ''}" onclick="setTheme(null)">
                    <div style="font-size: 48px; padding: 20px;">📝</div>
                    <h3>Text Mode</h3>
                    <p>Original text-based faces</p>
                    <button class="btn ${!currentTheme ? 'disabled' : ''}">
                        ${!currentTheme ? 'Active' : 'Select'}
                    </button>
                </div>
            `;
            
            for (const theme of themes) {
                const isActive = theme.name === currentTheme;
                html += `
                    <div class="theme-card ${isActive ? 'active' : ''}" onclick="setTheme('${theme.name}')">
                        <img src="/api/themes/${theme.name}/preview" 
                             alt="${theme.name}" 
                             onerror="this.src='data:image/svg+xml,<svg xmlns=\\'http://www.w3.org/2000/svg\\' width=\\'128\\' height=\\'64\\'><rect fill=\\'%23333\\' width=\\'100%\\' height=\\'100%\\'/><text x=\\'50%\\' y=\\'50%\\' text-anchor=\\'middle\\' fill=\\'%23666\\' font-size=\\'12\\'>No Preview</text></svg>'">
                        <h3>${theme.metadata?.name || theme.name}</h3>
                        <p>${theme.metadata?.description || ''}</p>
                        <div class="face-count">${theme.face_count} faces</div>
                        <button class="btn ${isActive ? 'disabled' : ''}">
                            ${isActive ? 'Active' : 'Select'}
                        </button>
                    </div>
                `;
            }
            
            grid.innerHTML = html;
        }
        
        async function setTheme(name) {
            const statusEl = document.getElementById('status');
            statusEl.className = 'status';
            statusEl.style.display = 'none';
            
            try {
                const resp = await fetch('/api/themes/set', {
                    method: 'POST',
                    headers: {'Content-Type': 'application/json'},
                    body: JSON.stringify({theme: name})
                });
                
                const data = await resp.json();
                
                if (data.success) {
                    currentTheme = name;
                    statusEl.className = 'status success';
                    statusEl.textContent = '✅ ' + data.message;
                    updateCurrentTheme();
                    loadThemes();  // Refresh grid
                } else {
                    statusEl.className = 'status error';
                    statusEl.textContent = '❌ ' + data.message;
                }
            } catch (e) {
                statusEl.className = 'status error';
                statusEl.textContent = '❌ Error: ' + e.message;
            }
            
            statusEl.style.display = 'block';
            setTimeout(() => { statusEl.style.display = 'none'; }, 3000);
        }
        
        // Load themes on page load
        loadThemes();
    </script>
</body>
</html>
'''


@themes_bp.route('/ui', methods=['GET'])
def themes_ui():
    """Serve the theme selection UI page"""
    from flask import Response
    return Response(THEMES_HTML, mimetype='text/html')


def register_with_pwnagotchi(app):
    """
    Register theme routes with Pwnagotchi's Flask app.
    
    Call this from a Pwnagotchi plugin's on_loaded() method:
    
        def on_loaded(self):
            from pwnaui.web_themes import register_with_pwnagotchi
            register_with_pwnagotchi(self._agent._view._web._app)
    """
    app.register_blueprint(themes_bp)
    log.info("Registered PwnaUI themes API")


# Standalone Flask app for testing
def create_app():
    """Create standalone Flask app"""
    from flask import Flask
    app = Flask(__name__)
    app.register_blueprint(themes_bp)
    return app


if __name__ == '__main__':
    # Run standalone server for testing
    app = create_app()
    print("Starting PwnaUI Themes API server...")
    print("Open http://localhost:5000/api/themes/ui in your browser")
    app.run(host='0.0.0.0', port=5000, debug=True)
