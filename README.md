# FF13-2-SpeedrunPractice

[![Build](https://github.com/a-ki-yoshi/FF13-2-SpeedrunPractice/actions/workflows/build.yml/badge.svg)](https://github.com/a-ki-yoshi/FF13-2-SpeedrunPractice/actions/workflows/build.yml)
[![Twitch Status](https://img.shields.io/twitch/status/a_ki_yoshi)](https://www.twitch.tv/a_ki_yoshi)
[![X (formerly Twitter) URL](https://img.shields.io/twitter/url?url=https%3A%2F%2Fx.com%2FOribeAkiyoshi)](https://x.com/OribeAkiyoshi)

[**日本語のREADMEはこちら/Read in Japanese**](README.ja.md)

A mod for the PC (Steam) version of FINAL FANTASY XIII-2, with features that make speedrun practice easier.

## Requirements

- Windows
- FINAL FANTASY XIII-2 on Steam (PC)
  - Any region should work, though only the Japan region has been verified.  
    The mod speaks Japanese when the game's language in Steam is Japanese, English for every other language.  
    Developed and tested on the Japan-region install; reports from other regions are welcome too.

## How to use

1. Download the zip file

[**Click `FF13-2-SpeedrunPractice-vx.x.x.zip` on this page**](https://github.com/a-ki-yoshi/FF13-2-SpeedrunPractice/releases).

2. Extract the zip file and place the three files other than `LICENSE` and `README` in the same folder as `ffxiii2img.exe`

Example folder path: `C:\Program Files (x86)\Steam\steamapps\common\FINAL FANTASY XIII-2\alba_data\prog\win\bin\`

| File | Description |
| ---- | ---- |
| `dinput8.dll` | The mod itself |
| `ff13-2-practice.bat` | A batch file that starts the game with the mod. See "Notes for timed runs" |
| `ff13-2-run.bat` | A batch file that starts the game without the mod. See "Notes for timed runs" |

3. Start the game

4. Use the features with their hotkeys (**F1 shows every hotkey and setting with a description, and lets you change them on the spot**)

Note: no settings file (`ff13-2-srpractice.ini`) is included. It is created in the same folder as `ffxiii2img.exe` the first time you change a setting on the hotkeys-and-settings screen.  
Note: if F1 does not work for some reason, type the two lines below in Notepad or similar, save them as `ff13-2-srpractice.ini`, and place it next to the dll (write the key you want to use instead of F1 in place of `Ctrl+1`).

```ini
[hotkeys]
help = Ctrl+1
```

Note: to update (install another version), place the contents of the new zip the same way, overwriting the old files. The ini is not included in the zip, so your settings stay as they are.

## Notes for timed runs

On Speedrun.com, using a mod that is not allowed for the game or category is against the rules.  
Do not use this mod when timing a run to submit to Speedrun.com or elsewhere.  
**Just having this mod installed changes some behaviour** (the "Skip the Historia Crux on load" and "Skip the recap after a load" settings, which are on by default, and the location line it adds to the save list).  
For timed runs, start the game without the mod, for example with the batch files below.

### Keeping the mod out of the game entirely, e.g. for timed runs

If you want to keep the mod out of the game entirely, e.g. for timed runs, starting the game from one of the two included bat files (batch files) makes this easy.  
`ff13-2-run.bat`: starts FF13-2 without the mod  
`ff13-2-practice.bat`: starts FF13-2 with the mod
- They turn the mod on and off by renaming `dinput8.dll` to a name the game does not load (`dinput8.dll.off`) and back
- With the "Save slot limit" set to 41 or more, the game without the mod quits at the save list if save 41 or later exists, so `ff13-2-run.bat` takes them out of the game's reach before starting it by renaming `ff13_2-NN.dat` in the save folder to `ff13_2-NN.dat.parked`
  - Starting the game with `ff13-2-practice.bat`, or renaming the file back to `dinput8.dll` by hand and starting the game, makes the mod take the `.parked` off again
- With a category picked under "Category for starts without the mod" on the hotkeys-and-settings screen, `ff13-2-run.bat` lists that category's saves 1 to 40 in the save list
- Saves made while the game runs without the mod are kept as that category's saves after the next start with the mod
- Running either one twice in a row changes nothing
- They cannot be used while the game is running (quit it first)
- You may also move or rename the dll by hand instead of using the bat files. In that case, if save 41 or later exists, move those saves out too
- The bat files also work through a shortcut you create and click
- The bat files can be renamed, as long as the extension stays

## Using it with other mods

- **It cannot be used together with the [FPS Fix](https://github.com/MrTyton/Final-Fantasy-Speedruns/tree/master/Final%20Fantasy%20XIII/FPS%20Fix) (the one whose files are named `dinput8.dll` and `FF13Fix.ini`).** Both use the file name `dinput8.dll`, so they cannot sit in the same folder
  - The included bat files stop without renaming anything when the `dinput8.dll` in place is not this mod's (the FPS Fix's, for example)
- It can be used together with the Drop Mod (Nekton and Gahongas Drop Rate Mod). Neither the mod nor its bat files interfere with the Drop Mod.

## When publishing videos or images

When publishing videos, images or the like that show things normal play does not, please make it clear that a mod was used.  
This is to keep them from being mistaken for bugs in normal play.

## Hotkeys in short

- Each feature has a hotkey
- F1 shows the hotkeys-and-settings screen on the game screen, listing every feature with its current hotkey and every setting. You can change them there too

## Features

Features that are switched ON/OFF keep their ON/OFF state after the game is restarted.

### Features used with a hotkey

(The keys in \[ \] are the default hotkeys.)

- **\[F1\] Hotkeys and settings**
  - Shows or hides the hotkeys-and-settings screen
  - Pick a row with the ↑ ↓ keys
  - To change a hotkey, confirm on its row and then press the new key (combinations with Ctrl, Shift and Alt work too). Pressing the same key again unbinds it
  - A key already assigned to another feature cannot be used
  - A changed value takes effect at once
  - `ff13-2-srpractice.ini` is created to hold the settings if it does not exist yet, and only the line concerned is rewritten
  - Settings you can change
    - Hotkeys while another window is active (default: OFF)
      - When OFF, hotkeys are accepted only while the game window is active
      - When ON, they work while it is not active too
    - Save slot limit (default: 40)
      - Raises the number of save slots up to 99
      - **Turn Steam Cloud sync off for this game when using this feature**
    - Fixed autosave slot (default: **OFF**)
      - Makes autosaves go to the last-numbered slot, so the save you loaded is not overwritten
      - Note that the autosave slot's contents get overwritten
    - Keep a save's note when saving over it (default: **OFF**)
      - When you save over a slot after giving it a note with the "Tidy the game's save list" feature, the note stays when ON and is erased when OFF
    - Category for starts without the mod (default: none)
      - **Turn Steam Cloud sync off for this game when using this feature**
      - Picks the category listed in the save list when the game starts without the mod through `ff13-2-run.bat` (details under "Notes for timed runs")
      - None: the category in use as it is (only saves 41 and later are hidden)
    - Skip the Historia Crux on load (default: ON)
      - A save made inside an area loads straight into that area at the place it was saved
      - You can go to the Historia Crux at any time with "Historia Crux" on the pause menu
      - A save made at the Historia Crux, one made before this mod was installed, or a file brought in from elsewhere loads into the Historia Crux as before
    - Skip the recap after a load (default: ON)
      - Skips the recap movie that plays right after a load
    - GAME SPEED factors (default: 3.0)
      - Sets the factors that "GAME SPEED" below steps through
      - On the hotkeys-and-settings screen you can register up to 8 of 0.5/1.5/2/3/4/5/6/8/10 (any value from 0.1 to 10 when written directly in the ini)
      - For example, with 3 and 8 registered, each press steps through 3x → 8x → OFF in turn
    - Text size (default: 100%)
      - Sets the size of the text this mod draws on the screen (the lines of the features that are on, the battle overlay and the menus)
      - On the hotkeys-and-settings screen you can pick auto/75%/100%/125%/150%/175%/200%/250%/300% (any size from 0.5 to 4 when written directly in the ini)
      - 100% is the size the text has always had. Auto grows it with the screen's resolution (100% at 720p, 150% at 1080p, 300% at 4K)
      - A change takes effect at once. When the hotkeys-and-settings screen does not fit on the screen, it scrolls to keep the selected row in view
    - Log level (default: normal)
      - Sets how much detail the log records
      - Note that the log starts anew at every launch, and only the previous launch's log is kept, as `prev.log`

- **\[F2\] GAME SPEED**
  - Changes the game's speed. Field movement, battles, ATB speed and everything else change together
  - While on, the screen shows something like `(F2) GAME SPEED 3x: ON`

- **\[F3\] KILL MODE**
  - The hotkey steps through "instant win at battle start + one-hit kill" → "one-hit kill" → OFF in turn
    - **Instant win at battle start**
      - Turning it on during a battle does nothing to the battle already under way. It takes effect from the next battle start
      - A battle ends in a win the moment it starts
      - Items and CP other than guaranteed drops are not obtained
      - A battle emptied by the instant win is not restarted by "Battle auto-restart"
    - **One-hit kill**
      - An enemy's HP drops to 0 the moment you damage it
      - Item drops work as in a normal battle
  - While on, the screen shows something like `(F3) INSTANT WIN AT BATTLE START + ONE-HIT KILL: ON`

- **\[F5\] Suppress enemy spawns**
  - Enemies stop appearing while you move around the field
  - While on, the screen shows `(F5) SUPPRESS ENEMY SPAWNS: ON`

- **\[F6\] Battle auto-restart**
  - When on, wiping out the enemies does not win the battle; a restart runs instead
    - In a boss battle where a restart begins the battle again, the same battle starts once more
    - The opening boss battle is two battles in a row. The game's own restart goes back to the first even from the second, but the auto-restart starts from the second
    - **It does not work in battles where a movie starts once the enemy's HP is gone.** Restart those by hand
  - While on, the screen shows `(F6) BATTLE AUTO-RESTART: ON`

- **\[F7\] Save anywhere**
  - **Turn Steam Cloud sync off for this game when using this feature**
  - In the field, opens the same slot-selection screen the Historia Crux save uses
  - It works properly only in the field. At the Historia Crux, use the game's own save screen
  - Nothing stops it from being pressed during an event or the like, so if you press the hotkey by mistake, cancel on the slot-selection screen (pressing the hotkey again does not cancel)
  - "Save" from the pause menu in the field works as before (it silently overwrites the slot you last saved to or loaded from)

- **\[F9\] Tidy the game's save list**
  - **Turn Steam Cloud sync off for this game when using this feature**
  - Controls
    - Confirm the selected item: Enter
      - While this menu is shown, keyboard input does not reach the game, so it is safe even if the game's confirm key is the same
    - Pick an item: ↑ ↓ keys
    - Change a value: ← → keys
    - Pick the save to work on in the save list: A D keys
    - Typing text: the usual keys
    - Deleting a character: Backspace
  - The menu appears only on the title screen and the save list
  - Each screen has its own menu
  - Features on the title screen
    - Which saves the list shows
      - An area filter for the save list used to load
      - Narrows the list to one area + era, the same as the list's third line (e.g. "Sunleth Waterscape AF300")
      - A save made at the Historia Crux counts as the area that was selected there
      - The area choices are in the order of each area's earliest save
      - Works on saves made before this mod was installed and on saves made without the mod too
      - The first save after NEW GAME counts as `Valhalla`
      - The setting is saved, so the same area is still selected after the game is restarted
    - Manage categories
      - Switches whole sets of saves, one folder each
        - The category in use stays directly in the save folder as before; the others are stashed in `{save folder}\categories\{category name}\`
      - Before you use it, a category named "main" counts as the one selected
      - One category holds as many saves as the save slot limit (40 normally)
      - The rows in detail
        - "switch to {target} (using {category name})"
          - Pick the target with the ←→ keys. The switch happens when you press the confirm key, select another row, or close the menu with the hotkey
          - The first choice, `----`, means nothing is picked and nothing switches
        - New category (with a copy of the latest save / with a copy of every save)
          - Type a category name and switch to the new category
          - "the latest save": copies only the latest save (because a category with no saves loses "Continue" on the title screen)
          - "every save": copies every save of the category in use as it is, save numbers included
          - About the input field
            - Half-width (ASCII) characters can be typed directly
            - You can also paste text copied elsewhere with Ctrl+V
              - That way multibyte characters such as full-width letters and kana work too
            - Up to 24 characters
            - `\ / : * ? " < > | ,` cannot be used
            - Backspace deletes one character at a time
            - A name that an existing category already has cannot be confirmed (an error appears in red in the input field)
            - Enter confirms, the hotkey cancels
        - Rename the current category
        - "Delete "{category name}""
          - Pick the category to delete with the ←→ keys and confirm to delete it
          - A category holding one save or more needs one more confirm, as a check
          - The category in use cannot be picked. To delete it, switch to another category first
          - It goes to the Windows Recycle Bin with its contents, so a mistake can be restored from there
            - If the save folder is on a drive without a Recycle Bin (network or removable), it is deleted outright
      - The record of direct loads moves together with the saves
      - If a switch is cut off by a crash or a power loss, it is finished automatically at the next launch. Until then, nothing that moves or deletes saves is accepted
    - Sort order
      - ← → picks the order of the game's save list: save date / story order (endgame first) / story order (earliest first) / by save number
      - In story order, saves at the same place are ordered by how far the story has gone there, then by play time
      - In every order, the autosave slot comes first (its heading is `(AUTO) ` followed by the game's own heading)
      - The dates shown in the save list stay the real save dates
      - The chosen order is remembered, and every list comes out in that order until you change it
    - Renumber into story order
      - Renumbers the saves in the category in use into story order (saves at the same point in date order, oldest first). Save 01 becomes the earliest in the story
      - The first confirm shows a check message; confirming again runs it
      - If the renaming is cut off by a crash or a power loss, it is finished automatically at the next launch, or the saves go back to their old numbers
  - Features on the save list (each one works on the save the cursor is on)
    - The menu follows the cursor: while the menu is shown, moving the cursor with a gamepad or the A/D keys switches to that save
    - The hotkey closes it. Backspace does not (the game itself takes that key as "cancel")
    - Note
      - Type a text to show in the game's save list
      - It is added to the end of the heading line, as in "{heading} - note"
      - About the input field
        - Half-width (ASCII) characters can be typed directly
        - You can also paste text copied elsewhere with Ctrl+V
          - That way multibyte characters such as full-width letters and kana work too
        - Up to 24 characters
        - Backspace deletes one character at a time
        - Enter confirms, the hotkey cancels
      - The note is written into the save file itself, so it travels with the file (it also survives the copy to the autosave slot)
      - When you save over the save
        - With "Keep a save's note when saving over it" ON on the hotkeys-and-settings screen, the mod writes the note back at every save so it stays
        - When OFF, the note is erased too
        - Whatever the setting, an autosave to the autosave slot erases the note
    - Send to another category (the "copy" and "move" rows)
      - "Copy" or "move" one save, or every save in a selected range at once
      - Pick the target category with the ←→ keys. The picked target is shown as "copy -> {category name}"
      - Picking "<new category>" creates a new one with a name you give it
      - In the target, the saves go into that category's free numbers
      - If the target has fewer free numbers than the saves in the range, nothing is done and a message says so
      - "Move" leaves out the slot in use and the autosave slot (they stay, and a message says so). "Copy" can send them
      - After a "move", the moved saves still appear while the save list stays open
        - Picking one only says the load failed; no harm is done
        - They disappear when the save list is opened again
      - Note that if a "move" leaves the source with no saves, "Continue" disappears when you go back to the title screen
    - Select a range
      - Running "select a range (save N is one end)" makes that save one end of the range
      - Put the game's cursor on the other end with a gamepad or the A/D keys to select the range
      - While the list is narrowed to an area, only the saves shown count
      - Running the same row again, or leaving the save list, clears the selection
    - Delete
      - Deletes every save in the selected range at once
      - It cannot run with no range selected
      - The first confirm turns the row into a check message; confirming again runs it
      - Leaving the row, moving the cursor to another save, closing the menu or clearing the range cancels the check
      - The saves go to the Windows Recycle Bin, so a mistake can be restored from there
        - If the save folder is on a drive without a Recycle Bin (network or removable), they are deleted outright
      - As with "move", the slot in use and the autosave slot stay
      - The deleted saves still appear while the save list stays open. They disappear when the list is opened again
      - Note that when a category has no saves left, "Continue" disappears from the title screen

- **\[F10\] Battle overlay**
  - Shows the following in battle
    - TIME
      - GAME: in-game time
      - REAL: real time
      - ATB \*\*s: seconds left until a paradigm shift fully refills every party member's ATB (READY = the next shift refills it)
    - TARGET
      - HP: the target's remaining HP
      - ATB: the target's ATB gauge
      - CHAIN: how long the target's chain gauge lasts; the seconds left until it drops back to 100%
  - Used together with "GAME SPEED", GAME (in-game time) runs at that speed too. REAL does not change

### Always-on features

- **The area at the time of saving, in the save list**
  - The last line of each save in the save list shows the area it was saved in
    - A save made inside an area: the area name and era, such as `Academia AF500`
    - A save made at the Historia Crux: the area the cursor was on, such as `Historia Crux (Academia AF500)`
    - The first save after NEW GAME: `Valhalla`
  - Saves made before this mod was installed show nothing until they are saved again

## Files the mod creates

- The `ff13-2-mods\` folder and the files in it
  - Created at the first launch (the log, the state, the record of direct loads, category names and so on)
- `ff13-2-srpractice.ini`
  - Created in the same folder as `ffxiii2img.exe` when you change a setting on the hotkeys-and-settings screen
- `{save folder}\categories\`
  - Created when you make a category. It holds the saves of the categories not in use
- `{save folder}\ff13_2-NN.dat.parked`
  - Saves 41 and later (all of them while a "Category for starts without the mod" is picked), renamed by `ff13-2-run.bat`. They are put back when the mod starts
- `ff13-2-srpractice.nomod`
  - Placed inside the category picked under "Category for starts without the mod" (in `ff13-2-mods\` while it is the category in use)

## When something goes wrong, or the game crashes

If you can, please do the following.

### 1. Copying the log somewhere safe

First, copy `ff13-2-mods\ff13-2-srpractice.log` to another place, such as the desktop.

> **Note: when the game is started again, this log moves to `ff13-2-mods\prev.log` and a new log begins; the launch after that overwrites `prev.log` too.**
> Copy it before the game has been started twice (the log from right after the crash is the only clue). When the game crashes, the mod also tries to write where it crashed into this log.

### 2. Reproducing with `verbose`

If you can reproduce it, set "Log level" on the hotkeys-and-settings screen to `verbose`, then do the same thing again to reproduce the problem.
That makes the cause much easier to find. Once reproduced, copy that log somewhere safe in the same way.

### 3. Sending the log

Getting in touch with the symptom (what you did and what happened) and the copied log helps a great deal.

## Uninstall

- To uninstall, just delete `dinput8.dll` from the install folder
- The files and folders the mod created can be deleted too
  - **Exception: the save folder's `categories\` folder holds the saves of the categories not in use.** Before deleting it, move the saves you need out of `categories\` (directly into the save folder), or back up the whole folder
- **If you set the "Save slot limit" to 41 or more, first move saves 41 and later to free numbers 1 to 40, or out of the save folder.** Left in place, they make the game without the mod quit the moment the save list opens
- After starting the game with `ff13-2-run.bat`, start it once with the mod before deleting. The saves `ff13-2-run.bat` hid (`.parked`) come back when the mod starts

## Safety

- This mod does no communication of any kind and makes no network connection
- Of the game's own files, the only ones it rewrites are save files (writing the location line and notes, renaming, moving and deleting with the tidy feature, and the copy to the autosave slot)

## Disclaimer

This mod is unofficial, and is in no way affiliated with or endorsed by Square Enix.

Use it only with a copy of FINAL FANTASY XIII-2 that you legitimately own.
Use at your own risk.

The author accepts no responsibility for lost save data or any other damage.

## Credits

[WhiteCryptTool](https://github.com/Surihix/WhiteCryptTool) by Surihix (GNU GPL v3.0)  
The save-data decryption in this mod is derived from WhiteCryptTool.

## License

FF13-2-SpeedrunPractice is free software, licensed under the **GNU General Public License v3.0 or
later** (see the `LICENSE` file). Copyright (C) 2026 Akiyoshi.
