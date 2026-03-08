# Game Files Setup

PocketQuake supports the original Quake campaign, both official mission packs, and the X-Men Quake total conversion. Each appears as a separate entry in the Pocket's game browser.

## Downloading Game Files

A complete Quake package with all required files is available at:

**https://archive.org/details/quake-complete**

Download `Quake.zip` (~1 GB) and extract it. The archive contains:

| Directory | Contents |
|-----------|----------|
| `id1/` | Original Quake (`pak0.pak` + soundtrack) |
| `hipnotic/` | Scourge of Armagon mission pack (`pak0.pak` + soundtrack) |
| `rogue/` | Dissolution of Eternity mission pack (`pak0.pak` + soundtrack) |
| `xmen/` | X-Men Quake total conversion (`pak0.pak` - `pak4.pak` + `progs.dat`) |

## SD Card Layout

Copy the game files to your Analogue Pocket SD card under `Assets/pocketquake/common/`:

```
Assets/pocketquake/common/
+-- pak0.pak                    <-- id1/pak0.pak (required for all instances)
+-- hipnotic/
|   +-- pak0.pak                <-- hipnotic/pak0.pak
+-- rogue/
|   +-- pak0.pak                <-- rogue/pak0.pak
+-- XMEN/
|   +-- pak0.pak                <-- xmen/pak0.pak
|   +-- pak1.pak                <-- xmen/pak1.pak
|   +-- pak2.pak                <-- xmen/pak2.pak
|   +-- pak3.pak                <-- xmen/pak3.pak
|   +-- progs.dat               <-- xmen/progs.dat
```

Note: The X-Men folder in the archive is named `xmen/` but must be placed as `XMEN/` on the SD card.

## What Each Instance Needs

### Shareware (free)
- `pak0.pak` -- included with the shareware release, Episode 1 only

### Quake (full game)
- `pak0.pak` -- base game data
- `pak1.pak` -- registered game data (Episodes 2-4). If you own Quake on Steam or GOG, this file is in the `id1/` folder of your installation

### Scourge of Armagon (Hipnotic)
- `pak0.pak` + `pak1.pak` -- base Quake (required)
- `hipnotic/pak0.pak` -- mission pack data

### Dissolution of Eternity (Rogue)
- `pak0.pak` + `pak1.pak` -- base Quake (required)
- `rogue/pak0.pak` -- mission pack data

### X-Men Quake
- `pak0.pak` + `pak1.pak` -- base Quake (required)
- `XMEN/pak0.pak` through `pak3.pak` -- mod data
- `XMEN/progs.dat` -- mod game logic
