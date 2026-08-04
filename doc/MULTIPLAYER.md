# Multiplayer

This document covers the first version of networked multiplayer: a dedicated
server binary that streams its text screen to one thin client over TCP, and the
client that connects to it. This is a technology preview -- see
[Limitations](#limitations-of-this-first-version) before you plan a session.

## Overview

The server is a special build of the game (`MP_SERVER=1`). It links an
in-memory curses backend and renders everything -- menus, the map, IMGUI
screens -- into a text grid. One client connects over TCP, sends its terminal
size, and the server starts streaming the grid to it. The client is the normal
game binary in a special mode: it forwards your keystrokes to the server and
paints whatever screen the server sends. The server runs the game logic; the
client is a thin remote viewport.

Because the server is a full game build, everything you can do in single-player
(create a character, move, fight, craft, read, sleep) is available -- but only
one client connects, and only the server's own keybindings are in effect.

## Building

### Server

```sh
make -j$(nproc) CCACHE=1 MP_SERVER=1 BUILD_PREFIX=mpserver-
```

This produces `mpserver-cataclysm-server`. `MP_SERVER` implies `MULTIPLAYER` and
sets a server-specific target name (Makefile, `TARGET_NAME = cataclysm-server`),
so the build never collides with a normal client build. The server links the
in-memory curses backend and IMGUI's text backend; it does not link SDL or
ncurses.

To also build the server test binary (the `[mp]` tests, used by CI), add
`TESTS=1`:

```sh
make -j$(nproc) CCACHE=1 TESTS=1 MP_SERVER=1 BUILD_PREFIX=mpserver-
# -> tests/mpserver-cata_test
```

### Client

The client is the normal game built with `MULTIPLAYER=1` (the curses build is
sufficient):

```sh
make -j$(nproc) CCACHE=1 MULTIPLAYER=1
# -> cataclysm
```

`MULTIPLAYER` is **not** on by default, and it is what compiles `--connect` in.
A plain `make` produces a binary that still accepts `--connect` on the command
line but refuses to run with it, because the client code is not in the build.
If you already have a plain build sitting at `./cataclysm`, this rebuilds it in
place -- quit any running copy first, or the link fails with "Text file busy".

## Running a session

### 1. Start the server

```sh
./mpserver-cataclysm-server --server <port> --world <name> --userdir <path>
```

- `--server <port>` -- run this instance as a multiplayer server that streams
  its screen to one client (main.cpp, `--server` handler).
- `--world <name>` -- load this world. If omitted, the server opens the
  world-selection menu, which is streamed to the client, so you can pick a
  world from the client side.
- `--userdir <path>` -- keep the server's config, saves, and log under this
  directory instead of your normal single-player profile. Recommended, so the
  server does not share options or savegames with your client.

The server binds a TCP listener on `<port>`, waits for one client, and performs
a connection handshake (protocol version, terminal size, 256-colour support).
Only after the client is accepted does the game start; you can take as long as
you like to connect.

The server has no screen of its own -- its curses backend draws into a buffer
that goes out over the wire -- so the console it was launched from carries a
session log on stderr instead:

```
[21:44:03] mp: listening on port 4321 (world: "Hanston", protocol 1)
[21:44:03] mp: waiting for a client
[21:44:31] mp: connection from 127.0.0.1:55226; shaking hands
[21:44:31] mp: client "sunknown" connected from 127.0.0.1:55226 (terminal 180x50, 256 colours, protocol 1)
[21:45:12] mp: game started in world "Hanston" as "Norman Blair"
[21:52:03] mp: saving "Norman Blair"
[21:52:05] mp: saved
[21:52:10] mp: client disconnected after 7m39s
[21:52:10] mp: server stopped
```

Every failure that stops the server says so on the same stream: a port already
in use, a failed accept, and a rejected handshake with the reason the client was
turned away.

### 2. Connect the client

```sh
./cataclysm --connect <host:port>
```

- `--connect <host:port>` -- connect to a multiplayer server and play as its
  client (main.cpp, `--connect` handler).

The client needs a terminal of at least 80x24. On connect it announces its
terminal size; the server streams at that size. Resizing the client's terminal
renegotiates the grid.

When the client disconnects, the server shuts down cleanly (it does not keep
running and it does not keep the world open for a second player).

## Limitations of this first version

- **One player.** The server accepts exactly one client (`mp_listener::accept`)
  and ends the session when that client disconnects. There is no rejoin, no
  reconnect, and no spectator mode.
- **The server's keyboard layout is the one that counts.** The client sends
  raw input events (key presses, mouse position, modifiers); the server feeds
  them through its own keybinding configuration. If you want a different layout,
  change the server's keybindings. The client's keymap is not consulted.
- **No sound.** The server renders to a text grid and streams that grid; audio is
  not transmitted and the client does not synthesise it.
- **No tileset map.** The streamed picture is the server's text grid. Tile
  graphics are never sent; the client renders text cells and colours.
- **Text only.** The picture is a grid of characters with per-cell colour
  indices. Mouse events are forwarded, but there is no pixel-perfect or
  hardware-accelerated rendering.

## Debugging

The wire traffic is useful for debugging a session.

- **Control messages are JSON.** The hello, resize, input, cursor, and bye
  messages are serialised as readable JSON (`mp_control_to_json`, mp_protocol).
  Screen frames are the compact binary of `mp_screen_codec`, because that is
  the hot path.
- **Peer JSON is parsed leniently.** Incoming control messages call
  `obj.allow_omitted_members()` before reading fields (mp_protocol.cpp), so a
  peer that adds a field does not trip the "unvisited member" debug message.
  The whole parse is wrapped in a `try`/`catch( const JsonError & )`, so a
  malformed or unknown message is dropped rather than raised. The `proto` field
  in the client's hello exists so a later protocol version can add fields and
  an older peer can skip what it does not recognise.
- **Frames.** Every payload is framed with one kind byte (`mp_msg_kind`: 1 for
  control, 2 for screen) followed by the body (`mp_wrap`/`mp_unwrap`). The TCP
  transport sets `TCP_NODELAY` so key presses are not batched behind screen
  traffic.
- **The console.** The server's session log (above) is the first place to look;
  it is written to stderr, so `./mpserver-cataclysm-server ... 2>server.log`
  keeps it, and a service manager captures it with no extra configuration.
- **The debug log.** The game writes `debug.log` under the user directory's
  config dir (`<userdir>/config/debug.log`). Every console line is mirrored
  there through the `D_MAIN` class, but at `D_INFO` level, which the default log
  filter drops -- so an unconfigured run leaves no `debug.log` at all and the
  console is what you have. Errors (`mp client: cannot connect ...`, the
  oversized-terminal refusal) are logged at `D_ERROR`/`D_WARNING` and do reach
  the file. Run both sides with the same `--userdir` or with none at all; the
  `[mp]` server tests use a fresh `--userdir` so a session never writes into a
  profile you care about.
