# Multiplayer

This document covers networked multiplayer: a dedicated server binary that
streams a text screen to each of its thin clients over TCP, and the client that
connects to it. This is a technology preview -- see
[Limitations](#limitations-of-this-first-version) before you plan a session.

## Overview

The server is a special build of the game (`MP_SERVER=1`). It links an
in-memory curses backend and renders everything -- menus, the map, IMGUI
screens -- into a text grid per player. Each client connects over TCP, sends its
terminal size, and the server streams that player's grid to it. The client is the normal
game binary in a special mode: it forwards your keystrokes to the server and
paints whatever screen the server sends. The server runs the game logic; the
client is a thin remote viewport.

Because the server is a full game build, everything you can do in single-player
(move, fight, craft, read, sleep) is available, with every player in their own
menus at the same time -- but only the server's own keybindings are in effect.

The server also owns the world and the party. It opens the world named on its
command line, waits for its players, and gives each of them their own join menu,
in which the player creates a survivor or continues the one they played last
time. Nothing has to be prepared in the world beforehand.

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
./mpserver-cataclysm-server --server <port> --world <name> --players <count> --userdir <path>
```

- `--server <port>` -- run this instance as a multiplayer server that streams a
  screen to each connected client (main.cpp, `--server` handler).
- `--world <name>` -- the world to open, and it is required: a server started
  without one says `no world given: start the server with --world <name>` on its
  console and stops. A name that is not in the save folder yet is created with
  the default mods, so `--world coop` on a machine with no such world gives you
  a fresh empty one.
- `--players <count>` -- how many clients the server waits for before the game
  can start (default 1). It waits at the listener, then opens the world and
  waits for every one of those clients to choose a survivor (see
  [Joining a session](#joining-a-session)). The roster is fixed once the party
  starts: nobody joins or leaves after that.
- `--userdir <path>` -- keep the server's config, saves, and log under this
  directory instead of your normal single-player profile. Recommended, so the
  server does not share options or savegames with your client.

The server binds a TCP listener on `<port>`, waits for `<count>` clients, and
performs a connection handshake with each (protocol version, terminal size,
256-colour support, the client's name). Only after every client is accepted does
the world open and the join phase begin; you can take as long as you like to
connect.

The server has no screen of its own -- its curses backend draws into a buffer
that goes out over the wire -- so the console it was launched from carries a
session log on stderr instead:

```
[21:44:03] mp: listening on port 4321 (world: "Hanston", protocol 1)
[21:44:03] mp: waiting for 2 clients
[21:44:31] mp: connection from 127.0.0.1:55226; shaking hands
[21:44:31] mp: client "Bo" connected from 127.0.0.1:55226 (terminal 180x50, 256 colours, protocol 1)
[21:44:31] mp: 1 of 2 connected
[21:44:47] mp: connection from 127.0.0.1:55230; shaking hands
[21:44:47] mp: client "Ada" connected from 127.0.0.1:55230 (terminal 80x24, 256 colours, protocol 1)
[21:44:47] mp: 2 of 2 connected
[21:44:47] mp: world "Hanston" is open; waiting for 2 players to choose a survivor
[21:45:12] mp: loading "Norman Blair"
[21:45:12] mp: "Bo" plays "Norman Blair"
[21:45:19] mp: "Ada" plays "Ada King" (new)
[21:45:19] mp: game started in world "Hanston"
[21:52:03] mp: saving "Norman Blair"
[21:52:03] mp: saving "Ada King"
[21:52:05] mp: saved
[21:52:10] mp: "Bo" disconnected after 7m39s; ending the session
[21:52:10] mp: game ended
[21:52:10] mp: server stopped
```

Every failure that stops the server says so on the same stream: a port already
in use, a failed accept, a rejected handshake with the reason the client was
turned away, and a named world whose creation failed.

### 2. Connect the client

```sh
./cataclysm --connect <host:port> [--username <name>]
```

- `--connect <host:port>` -- connect to a multiplayer server and play as its
  client (main.cpp, `--connect` handler).
- `--username <name>` -- optional, and it does not choose your character. It is
  the label this client announces in its handshake: the server puts it in its
  console log and on the other players' waiting screen while they wait for you
  to choose. Without it the client announces `client`. Which survivor you play
  is decided inside the join menu, not on the command line.

The client needs a terminal of at least 80x24. On connect it announces its
terminal size; the server streams at that size. Resizing the client's terminal
renegotiates the grid.

The client also opens whichever join menu the server puts in front of it, and
remembers the survivor the server tells it it is playing (see [The character
card](#the-character-card)). Nothing else about the session lives on the client.

When any player leaves after the party has started -- disconnects, quits or dies
-- the session ends for everyone: the server tells every client why ("Bo
disconnected"), stops the player threads, and exits. It does not keep the world
open for a replacement, and it does not start a second session: run the server
again to play again. The join phase is the exception -- a client that quits or
drops there costs only its own seat, and the server keeps listening for a
replacement (see [Joining a session](#joining-a-session)).

## Two or more players

### Preparing a world

A co-op world is an ordinary world with more than one character in it. The
server opens the world, and the characters are made in the join menu, one save
file each -- so there is nothing to prepare beyond picking the world and its
mods:

1. Decide on a world name. If you want specific mods or a particular
   world-generation preset, create the world first in the normal game
   (World -> New World) and quit without making a character.
2. Start the server on it. A name that does not exist yet is created with the
   default mods, which is fine for a first session.

```sh
./mpserver-cataclysm-server --server 4321 --world "coop" --players 2 --userdir mp-coop
```

The world need not contain any characters when the server opens it. If it has
been played before, its map, clock and NPCs are there and the party starts from
them; if it is new, the first party's characters are placed next to each other
in a starting location of their own.

### Joining a session

Each connected client gets its own join menu in its own window, streamed like
any other screen. It offers:

- **Continue as <name>** -- present only when this client's card names a
  character that is still in this world and nobody else in this session has
  taken it. Loads that character where it was left.
- **New character: custom / preset / random** -- the ordinary character
  creator. `preset` picks from the server's character templates. A new
  character's name must be free: not a save in this world and not a name
  another player in this session has just confirmed -- the game asks for
  another name otherwise.
- **Settings** -- the server's options. Anyone in the party may change them;
  co-op is a trusted circle.
- **Help**, **Quit** -- the usual. Quit leaves before the party has started:
  your client exits, your seat is offered to the next connection, and everyone
  else stays where they were.

A new character is placed on a free tile next to the rest of the party: the
bubble is shared, and the tether has to hold from the first turn. All co-op
characters also start under one scenario, **Co-op Survivors**
(`data/json/scenarios_coop.json`, id `coop_survivors`); on a server the
creator's scenario list is filtered down to it, so the party gets one safe,
roomy place to wake up in.

Whoever chooses first sees a waiting screen that lists the players who have not
chosen yet, by the name their client announced (`--username`, or `client` if
they gave none -- the menu does not know their survivor yet). When the last one
confirms, the party starts.

### The character card

The server tells each client which survivor it plays, and the client writes a
small card under its own user directory:

```
<userdir>/config/mp/<base64 of host:port>.mpcard.json
```

For example `config/mp/#MTI3LjAuMC4xOjQzMjE=.mpcard.json` after connecting to
`127.0.0.1:4321`. It records the server address, the world name, the character's
`save_id` and its name -- enough to say "this is me" next time, and nothing
more. It is not a save: a character without its world is unplayable, so the card
cannot be played and cannot restore anything. The server stays the only source
of truth; a card naming a character that no longer exists simply means the
Continue entry is absent, and a client that has never been here sees the same
menu minus that entry.

There is one card per server address, in that client's user directory: connect
with the same `host:port` to be offered the same survivor. Two clients sharing
one user directory and one address would share the card, so give each its own
`--userdir` if you run more than one client on one machine.

### Starting the next session

Stop the server, start it again on the same `--world`, and have each client
connect with the same `--connect` address. Everyone who has played there before
finds "Continue as <name>" at the top of their menu and picks up where they
left off; anyone new creates a character instead. The world's map, clock and
NPCs are shared -- each character's own save file holds that character.

### How turns work

Everyone acts at once: each player has their own screen, their own menus and
their own input, and opening a menu never blocks anyone else's screen. The world
-- monsters, fire, the clock -- only moves on once every player has acted. A
player who still has time to spend this turn **holds** the world; a player who
has spent it, or who is running a long activity (crafting, reading, a long walk
to a destination), has **acted** and holds nothing.

Leafing through a menu costs no time. So a player reading the crafting list
holds the world until they pick something -- and their partner, who has already
acted, waits.

### Hurrying a partner

Press **`` ` ``** (backquote, the `coop_hurry` action) to start a countdown for
every player who is still holding the world -- you too, if you press it while
you still have time to spend. When it runs out, those players stop holding the
world for as long as they stay at the screen they are on: the world moves on,
and their characters stand still and lose those turns. Nothing is closed -- the
menu stays open, their keys still work, and they finish what they were doing
when they are ready. Once they act, the world waits for them again like anyone
else. If what they chose has gone stale meanwhile (somebody took the item), the
game refuses it with its usual message.

When a hostile creature can see any player, the countdown starts by itself and
is shorter (4 seconds rather than 15).

If nobody is left playing -- everyone has been dropped this way or is busy with
a long activity -- the world does not race ahead: it moves one turn per
countdown length until somebody acts.

## Limitations of this first version

- **A fixed roster, and no rejoin.** The server waits for exactly `--players`
  clients before it opens the world. A client that leaves the join menu frees
  its seat for a new connection, but once the party has started a disconnect,
  a quit or a death ends the session for everyone and the server process exits.
  There is no reconnect and no spectator mode.
- **Some world state is still shared.** The game itself warns that many features
  "will not work correctly with multiple characters in the same world", and
  co-op now lives on exactly that path. Missions and vehicle seats are per
  character as of this phase; base camps and zones, NPC followers, faction
  reputation, and achievements are not, and are deferred to Phase 5 with
  estimates in `docs/superpowers/reports/2026-08-08-multi-character-world-audit.md`.
- **Hurry is pressed by the wrong player.** A player who has already acted is
  parked until the next turn and reads no keys, so they cannot press hurry; only
  a player who is still holding the world can. In practice the waiting player
  asks their partner to press it, or waits; danger starts the countdown on its
  own. Planned for the next phase, together with a "carry on without me".
- **The world cannot ask anyone anything.** An NPC who walks up to talk, a
  monster that asks a question, a popup from an effect: these come from the
  world's half of the turn, which has no player to answer, so they get the
  answer of Escape -- the conversation does not open, the question is answered
  "no" -- and the server's console says one was skipped.
- **A death ends the session.** There are no death screens and no deathcam in a
  session: the world is saved with the dead character in it, and the death is
  dealt with the next time the world is loaded for single-player play.
- **One terminal size for everyone.** All players draw into one shared root
  window, which is sound only while every client runs the same terminal size.
  Use the same size on every machine.
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
