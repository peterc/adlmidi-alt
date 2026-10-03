# adlmidi
ADLMIDI is a MIDI player that uses OPL3 emulation.

![https://bisqwit.iki.fi/jutut/kuvat/programming_examples/midiplay.gif](https://bisqwit.iki.fi/jutut/kuvat/programming_examples/midiplay.gif)

## Key features

* OPL3 emulation with four-operator mode support
* FM patches from a number of known PC games, copied from files typical to AIL = Miles Sound System / DMX / HMI = Human Machine Interfaces / Creative IBK.
* Stereo sound
* Reverb filter based on code from SoX, based on code from Freeverb. A copy of either project is not needed.
* Number of simulated soundcards can be specified as 1-100 (maximum channels 1800!)
* xterm-256color support
* WIN32 console support (also tested with HXRT / MS-DOS)
* Pan (binary panning, i.e. left/right side on/off)
* Pitch-bender with adjustable range
* Vibrato that responds to RPN/NRPN parameters
* Sustain enable/disable
* MIDI and RMI file support
* loopStart / loopEnd tag support (Final Fantasy VII)
* Use automatic arpeggio with chords to relieve channel pressure
* Support for multiple concurrent MIDI synthesizers (per-track device/port select FF 09 message), can be used to overcome 16 channel limit
* Support for ScummVM, Doom, IMF and CMF files
* This project is developed in C++, but a GW-BASIC version is also provided (MIDIPLAY.BAS). This player was first implemented in GW-BASIC for a Youtube demonstration video! With alterations that take 15 seconds to implement, it can be run in QBasic and QuickBASIC too. Note: The GW-BASIC version does not contain all of the same features that the C++ version does.

## Building on macOS

Install the Xcode Command Line Tools (`xcode-select --install`) if needed, then
install the dependencies with Homebrew and build:

```sh
brew install pkg-config sdl2
make -j4
./adlmidi /path/to/song.mid -nl
```

The puzzle game is disabled by default. To enable it:

```sh
./adlmidi /path/to/song.mid -nl -game
```

Press `q`, Escape, or Ctrl-C to stop playback and restore the terminal.

Output gain defaults to +12 dB for both playback and WAV exports. Use `-gain`
to adjust it, for example `./adlmidi song.mid -gain 6`, or `-gain 0` for no
output boost. The range is -60 to +30 dB. Soft peak limiting protects loud
passages, and reverb adds ambience while preserving the direct instrument sound.
Reverb gain defaults to +24 dB; use `-reverb gain=18` to reduce its level,
or `-nr` to disable reverb.

Playback uses the terminal's alternate screen with a black background and fixed
RGB colours. Metadata, the single puzzle board, and notes have separate areas.
Each MIDI channel has an instrument label beside its notes. Track/instrument
names from the MIDI file are used when available, with General MIDI names as
the fallback.
The layout adjusts when you resize the terminal; resizing restarts the puzzle.
The puzzle is hidden automatically in terminals smaller than 50 columns or
29 rows. Font choice remains controlled by your terminal settings.

OpenMP is enabled automatically when the compiler supports it; Apple's Clang
builds without it. The macOS compatibility changes come from
[upstream issue #20](https://github.com/bisqwit/adlmidi/issues/20).

## Usage

```
Usage: adlmidi <midifilename> [ <options> ] [ <banknumber> [ <numcards> [ <numfourops>] ] ]
       adlmidi <midifilename> -1   To enter instrument tester
 -p Enables adlib percussion instrument mode (use with CMF files)
 -t Enables tremolo amplification mode
 -v Enables vibrato amplification mode
 -s Enables scaling of modulator volumes
 -nl Quit without looping
 -game Show the puzzle game (disabled by default)
 -no-game Hide the puzzle game
 -gain <dB> Output gain in dB (-60..30, default: 12)
 -w Write WAV file rather than playing
    Banks: 0 = AIL (Star Control 3, Albion, Empire 2, Sensible Soccer, Settlers 2, many others)
           1 = Bisqwit (selection of 4op and 2op)
           2 = HMI (Descent, Asterix)
           3 = HMI (Descent:: Int)
           4 = HMI (Descent:: Ham)
           5 = HMI (Descent:: Rick)
           6 = HMI (Descent 2)
           7 = HMI (Normality)
           8 = HMI (Shattered Steel)
           9 = HMI (Theme Park)
          10 = HMI (3d Table Sports, Battle Arena Toshinden)
          11 = HMI (Aces of the Deep)
          12 = HMI (Earthsiege)
          13 = HMI (Anvil of Dawn)
          14 = DMX (Doom           :: partially pseudo 4op)
          15 = DMX (Hexen, Heretic :: partially pseudo 4op)
          16 = DMX (MUS Play       :: partially pseudo 4op)
          17 = AIL (Discworld, Grandest Fleet, Pocahontas, Slob Zone 3d, Ultima 4, Zorro)
          18 = AIL (Warcraft 2)
          19 = AIL (Syndicate)
          20 = AIL (Guilty, Orion Conspiracy, Terra Nova Strike Force Centauri :: 4op)
          21 = AIL (Magic Carpet 2)
          22 = AIL (Nemesis)
          23 = AIL (Jagged Alliance)
          24 = AIL (When Two Worlds War :: 4op, MISSING INSTRUMENTS)
          25 = AIL (Bards Tale Construction :: MISSING INSTRUMENTS)
          26 = AIL (Return to Zork)
          27 = AIL (Theme Hospital)
          28 = AIL (National Hockey League PA)
          29 = AIL (Inherit The Earth)
          30 = AIL (Inherit The Earth, file two)
          31 = AIL (Little Big Adventure :: 4op)
          32 = AIL (Wreckin Crew)
          33 = AIL (Death Gate)
          34 = AIL (FIFA International Soccer)
          35 = AIL (Starship Invasion)
          36 = AIL (Super Street Fighter 2 :: 4op)
          37 = AIL (Lords of the Realm :: MISSING INSTRUMENTS)
          38 = AIL (SimFarm, SimHealth :: 4op)
          39 = AIL (SimFarm, Settlers, Serf City)
          40 = AIL (Caesar 2 :: partially 4op, MISSING INSTRUMENTS)
          41 = AIL (Syndicate Wars)
          42 = AIL (Bubble Bobble Feat. Rainbow Islands, Z)
          43 = AIL (Warcraft)
          44 = AIL (Terra Nova Strike Force Centuri :: partially 4op)
          45 = AIL (System Shock :: partially 4op)
          46 = AIL (Advanced Civilization)
          47 = AIL (Battle Chess 4000 :: partially 4op, melodic only)
          48 = AIL (Ultimate Soccer Manager :: partially 4op)
          49 = AIL (Air Bucks, Blue And The Gray, America Invades, Terminator 2029)
          50 = AIL (Ultima Underworld 2)
          51 = AIL (Kasparov's Gambit)
          52 = AIL (High Seas Trader :: MISSING INSTRUMENTS)
          53 = AIL (Master of Magic, Master of Orion 2 :: 4op, std percussion)
          54 = AIL (Master of Magic, Master of Orion 2 :: 4op, orchestral percussion)
          55 = SB (Action Soccer)
          56 = SB (3d Cyberpuck :: melodic only)
          57 = SB (Simon the Sorcerer :: melodic only)
          58 = OP3 (The Fat Man 2op set)
          59 = OP3 (The Fat Man 4op set)
          60 = OP3 (JungleVision 2op set :: melodic only)
          61 = OP3 (Wallace 2op set, Nitemare 3D :: melodic only)
          62 = TMB (Duke Nukem 3D)
          63 = TMB (Shadow Warrior)
          64 = DMX (Raptor)
          65 = TMB (Blood)
     Use banks 2-5 to play Descent "q" soundtracks.
     Look up the relevant bank number from descent.sng.

     <numfourops> can be used to specify the number
     of four-op channels to use. Each four-op channel eats
     the room of two regular channels. Use as many as required.
     The Doom & Hexen sets require one or two, while
     Miles four-op set requires the maximum of numcards*6.

     When playing Creative Music Files (CMF), try the
     -p and -v options if it sounds wrong otherwise.```
```

## Copying and contributing

ADLMIDI is distributed under the terms of the General Public License version 3 (GPL3). 
The OPL3 emulator within is from DOSBox is licensed under GPL2+. 
The FM soundfonts (patches) used in the program are imported from various PC games without permission from their respective authors. The question of copyright, when it comes to sets of 11 numeric bytes, is somewhat vague, especially considering that most of those sets are simply descendants of the patch sets originally published by AdLib Inc. for everyone's free use.
Patches (as in source code modifications) and other related material can be submitted to the primary author by e-mail at:Joel Yliluoma <bisqwit@iki.fi>

The author also wishes to hear if you use adlmidi, and for what you use it and what you think of it.
