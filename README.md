# vibeshine-server

A fork of [Vibeshine](https://github.com/Nonary/Vibeshine), the Sunshine-based game streaming host, that adds what [vibeshine-client](https://github.com/oomfslop/vibeshine-client) needs to stream single Windows apps as Mac windows.

It includes everything upstream Vibeshine offers, such as the virtual display driver, Windows Graphics Capture as a service, PyroWave, the virtual gamepad driver and Playnite sync. See the [upstream README](https://github.com/Nonary/Vibeshine#readme) for details. Regular Moonlight clients work as usual.

## What's added

### App windows

Clients with the window permission can stream individual windows instead of a whole display.

When the client picks a window, the host moves it onto a desk, a private virtual display the size of the client's screen. New windows from the same apps follow automatically: menus, dialogs, Properties sheets, the "Open with" picker and UAC prompts.

The host composites the desk's windows into a single atlas frame, so one video stream carries all of them. Background windows get a lower frame rate and, if the client asks for it, a coarser encode (a per-block quality map keeps the focused window at full quality). Windows completely hidden behind others are not sent at all, and the Mac shows their last frame until they are visible again. While idle, the stream drops to one frame per second.

The client and the host keep window position, size, stacking order and focus in sync both ways. The host reports each window's minimum and maximum size, its resize steps and whether it can be resized. Closing the desk sends every window back where it was, minimized and maximized windows included. Windows stranded by a crash or service restart are recovered on the next start.

### Input

- A virtual precision touchpad for native trackpad scrolling and gestures in Windows apps.
- Key repeats paced by the client instead of a timer on the host.
- The keyboard layout follows the client's input source, chosen from the layouts the signed-in user has installed.

### Apollo-style extras

- Clipboard sync.
- App launcher: lists the user's Start menu apps (with icons) and programs on `PATH`, and launches them as the signed-in user.

### Other

- The `capture_immediate` capture mode.
- Per-session host cursor hiding.

## Installation

Ask Claude.

## Permissions

Window streaming and clipboard sync are off by default for every paired device. Turn them on in the web UI under **Devices** → the device → **Allow app-window streaming** and **Allow clipboard sync**.

## API

All on the HTTPS port, authenticated with the client certificate like the rest of the GameStream API.

| Endpoint | Purpose |
| --- | --- |
| `GET /actions/windows`, `/actions/window-icon` | List windows and fetch their icons |
| `POST /actions/window?op=` | Adopt, release, park, move, resize, activate, minimize, maximize, restore, lower, dismiss, close |
| `POST /actions/desk?op=` | Open, close or read the desk, or set its stacking order |
| `GET /actions/desk-layout` | Long-poll for the desk layout (windows, frames, sizes, focus) |
| `GET /actions/window-hittest` | What is under a point |
| `GET /actions/launchables`, `/actions/launchable-icons`, `POST /actions/launch` | App launcher |
| `POST /actions/keyboard-language?lang=` | Switch the host keyboard layout |
| `GET`/`POST /actions/clipboard` | Clipboard |

## Building

CI builds an unsigned Windows x64 ZIP and MSI, and a Linux `.deb`, on every push. `v*` tags attach them to a GitHub release.

Local builds follow the [upstream build docs](docs/building.md). On Windows that means MSYS2 UCRT64 with CMake and Ninja.

## Debugging

Desk behaviour is traced in the host log (`config\logs\sunshine-<date>.log`):

- `Desk trace:` lines cover foreground changes, new windows and what happened to them, layout changes, client operations and activation results.
- `Input trace:` lines log mouse presses with the window under the pointer.

Keystrokes are never logged.

## License

GPL-3.0, same as Sunshine and Vibeshine.
