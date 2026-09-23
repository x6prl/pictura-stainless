# Window Manager & Compositor Integration Guide

`pictura-stainless` can be integrated into any Window Manager or Wayland compositor using the **`-p`** flag (or **`-rp`** to search subdirectories recursively):

```sh
# Top-level directory only:
pictura-stainless -p /path/to/wallpapers

# Recursive search through all subfolders:
pictura-stainless -rp /path/to/wallpapers
```

This prints **only** the path of the randomly chosen image directly to `stdout` with **no trailing newline**, making it ideal for command substitution in shell scripts, configs, and hotkey daemons.

---

## Table of Contents

- [Wayland Compositors](#wayland-compositors)
  - [Sway](#sway)
  - [Hyprland](#hyprland)
  - [River](#river)
  - [Niri](#niri)
- [X11 Window Managers](#x11-window-managers)
  - [i3wm](#i3wm)
  - [bspwm & sxhkd](#bspwm--sxhkd)
  - [dwm](#dwm)
  - [AwesomeWM](#awesomewm)
  - [Qtile](#qtile)
- [Automated Wallpaper Rotation](#automated-wallpaper-rotation)
  - [Systemd User Timer (Recommended)](#systemd-user-timer-recommended)
  - [Simple Shell Loop](#simple-shell-loop)

---

## Wayland Compositors

### Sway

Sway has built-in wallpaper configuration through IPC (`swaymsg`).

In `~/.config/sway/config`:

```i3config
# Set random wallpaper on startup and config reload
exec_always swaymsg output "*" bg "$(pictura-stainless -rp ~/Pictures/Wallpapers)" fill

# Keybinding to shuffle wallpaper (Super + Shift + W)
bindsym $mod+Shift+w exec swaymsg output "*" bg "$(pictura-stainless -rp ~/Pictures/Wallpapers)" fill
```

---

### Hyprland

Hyprland relies on external wallpaper daemons such as **`swww`** (recommended for smooth transitions) or **`hyprpaper`**.

#### Option A: With `swww` (Animated Transitions)

In `~/.config/hypr/hyprland.conf`:

```ini
# Start the daemon
exec-once = swww-daemon

# Set initial wallpaper on login
exec-once = swww img "$(pictura-stainless -rp ~/Pictures/Wallpapers)" --transition-type wipe

# Keybinding to change wallpaper (SUPER + SHIFT + W)
bind = $mainMod SHIFT, W, exec, swww img "$(pictura-stainless -rp ~/Pictures/Wallpapers)" --transition-type wipe --transition-fps 60
```

#### Option B: With `hyprpaper`

In `~/.config/hypr/hyprland.conf`:

```ini
exec-once = hyprpaper
bind = $mainMod SHIFT, W, exec, bash -c 'WP="$(pictura-stainless -rp ~/Pictures/Wallpapers)" && hyprctl hyprpaper unload all && hyprctl hyprpaper preload "$WP" && hyprctl hyprpaper wallpaper ",$WP"'
```

---

### River

In `~/.config/river/init`:

```sh
# Set initial wallpaper using swaybg
pkill swaybg
swaybg -i "$(pictura-stainless -rp ~/Pictures/Wallpapers)" -m fill &

# Keybinding (Super + Shift + W)
riverctl map normal Super+Shift W spawn 'pkill swaybg; swaybg -i "$(pictura-stainless -rp ~/Pictures/Wallpapers)" -m fill &'
```

*(Alternatively, use `swww img "$(pictura-stainless -rp ~/Pictures/Wallpapers)"` if running `swww`).*

---

### Niri

In `~/.config/niri/config.kdl`:

```kdl
spawn-at-startup "swww-daemon"
spawn-at-startup "sh" "-c" "swww img \"$(pictura-stainless -rp ~/Pictures/Wallpapers)\""

binds {
    Mod+Shift+W { spawn "sh" "-c" "swww img \"$(pictura-stainless -rp ~/Pictures/Wallpapers)\" --transition-type fade"; }
}
```

---

## X11 Window Managers

For X11 WMs, pair `pictura-stainless` with **`feh`** or **`xwallpaper`**:

```sh
sudo pacman -S feh      # Arch Linux
sudo apt install feh    # Debian / Ubuntu
sudo dnf install feh    # Fedora
```

---

### i3wm

In `~/.config/i3/config`:

```i3config
# Set random wallpaper on startup and reload
exec_always --no-startup-id feh --bg-fill "$(pictura-stainless -rp ~/Pictures/Wallpapers)"

# Keybinding to shuffle (Mod + Shift + W)
bindsym $mod+Shift+w exec --no-startup-id feh --bg-fill "$(pictura-stainless -rp ~/Pictures/Wallpapers)"
```

---

### bspwm & sxhkd

In `~/.config/bspwm/bspwmrc` (startup):
```sh
feh --bg-fill "$(pictura-stainless -rp ~/Pictures/Wallpapers)" &
```

In `~/.config/sxhkd/sxhkdrc` (keybinding):
```sh
# Super + Shift + W
super + shift + w
    feh --bg-fill "$(pictura-stainless -rp ~/Pictures/Wallpapers)"
```

---

### dwm

In `~/.xinitrc` or `~/.dwm/autostart.sh`:

```sh
feh --bg-fill "$(pictura-stainless -rp ~/Pictures/Wallpapers)" &
# or with xwallpaper:
# xwallpaper --zoom "$(pictura-stainless -rp ~/Pictures/Wallpapers)" &
```

To bind a shortcut in `config.h`:
```c
static const char *wallcmd[] = { "sh", "-c", "feh --bg-fill \"$(pictura-stainless -rp ~/Pictures/Wallpapers)\"", NULL };

static const Key keys[] = {
    { MODKEY|ShiftMask, XK_w, spawn, {.v = wallcmd } },
    /* ... */
};
```

---

### AwesomeWM

In `~/.config/awesome/rc.lua`:

```lua
local awful = require("awful")

-- Set on startup
awful.spawn.with_shell('feh --bg-fill "$(pictura-stainless -rp ~/Pictures/Wallpapers)"')

-- Keybinding: Mod4 + Shift + W
awful.keyboard.append_global_keybindings({
    awful.key({ modkey, "Shift" }, "w", function ()
        awful.spawn.with_shell('feh --bg-fill "$(pictura-stainless -rp ~/Pictures/Wallpapers)"')
    end, {description = "shuffle wallpaper", group = "screen"}),
})
```

---

### Qtile

In `~/.config/qtile/config.py`:

```python
import subprocess
from libqtile.config import Key
from libqtile.lazy import lazy
from libqtile import hook

WALLPAPER_DIR = "~/Pictures/Wallpapers"

@hook.subscribe.startup_once
def autostart():
    cmd = f'feh --bg-fill "$(pictura-stainless -rp {WALLPAPER_DIR})"'
    subprocess.Popen(cmd, shell=True)

# Add to keys list:
keys.extend([
    Key(["mod4", "shift"], "w", lazy.spawn(f'sh -c \'feh --bg-fill "$(pictura-stainless -rp {WALLPAPER_DIR})"\'')),
])
```

---

## Automated Wallpaper Rotation

### Systemd User Timer (Recommended)

To automatically shuffle wallpapers in the background at regular intervals without persistent sleeping shell scripts:

1. Create service unit `~/.config/systemd/user/wallpaper-rotate.service`:

```ini
[Unit]
Description=Rotate desktop wallpaper with pictura-stainless

[Service]
Type=oneshot
# Select the command corresponding to your desktop environment:

# For Sway:
ExecStart=/bin/sh -c 'swaymsg output "*" bg "$(/usr/local/bin/pictura-stainless -rp %h/Pictures/Wallpapers)" fill'

# For swww (Hyprland / River / Niri):
# ExecStart=/bin/sh -c 'swww img "$(/usr/local/bin/pictura-stainless -rp %h/Pictures/Wallpapers)" --transition-type wipe'

# For X11 (feh):
# ExecStart=/bin/sh -c 'feh --bg-fill "$(/usr/local/bin/pictura-stainless -rp %h/Pictures/Wallpapers)"'
```

2. Create timer unit `~/.config/systemd/user/wallpaper-rotate.timer`:

```ini
[Unit]
Description=Rotate wallpaper every 15 minutes

[Timer]
OnUnitActiveSec=15min
OnBootSec=1min

[Install]
WantedBy=timers.target
```

3. Enable and start the timer:

```sh
systemctl --user daemon-reload
systemctl --user enable --now wallpaper-rotate.timer
```

---

### Simple Shell Loop

If you prefer a lightweight background loop without systemd, add this snippet to your compositor/WM startup script:

```sh
# Rotates wallpaper every 10 minutes (600 seconds)
while true; do
    # Replace with your wallpaper tool of choice (swaymsg, swww, feh, swaybg, etc.)
    swww img "$(pictura-stainless -rp ~/Pictures/Wallpapers)" --transition-type wipe
    sleep 600
done &
```
