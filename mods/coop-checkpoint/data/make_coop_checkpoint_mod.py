"""
Coop Checkpoint - mod generator for Call to Arms: Gates of Hell
===============================================================

Builds the data mod  coop_checkpoint  which adds "Save checkpoint" and "Reload checkpoint" buttons to the
reinforcement panel of every campaign and skirmish mission (map/single/**/0.mi), read from the game's own
resource packs (so the output always matches the installed game version).

The button runs the vanilla mission-script command {"autosave"} inside the deterministic mission
script, i.e. in the same simulation step on every player's PC.  In coop the command does nothing on
its own: the version.dll of this project (binaries/x64) turns it into an immediate save of the
current game state, written on every PC that has the DLL goh_mods/coop_checkpoint.dll (file "coop_ckpt.sav"
in the profile save folder; the local server writes "coop_ckpt_srv.sav").

Every event used by the mission script is declared in {Helpers}: without that the dedicated server
crashes (eHelperReinforcement).

Usage
    python make_coop_checkpoint_mod.py "<game_dir>" ["<output_mods_dir>"]
The mod is written to <output_mods_dir>/coop_checkpoint (default: <game_dir>/mods/coop_checkpoint).
Requirements: Python 3.8+, Pillow (button icons; without it the vanilla "tactical" icon is used).
"""
import glob
import io
import os
import re
import shutil
import sys
import zipfile

MOD_NAME = "coop_checkpoint"
INIT_DELAY = 1          # seconds after mission start before the button is shown
BUTTON_COOLDOWN = 10    # seconds between two checkpoints from the button
CRLF = "\r\n"
HERE = os.path.dirname(os.path.abspath(__file__))
EV_CLICK = "[coop_ckpt]/click_save"
EV_RELOAD = "[coop_ckpt]/click_reload"
# set only inside a checkpoint: set, save, reset within one action list, so the running game never
# sees it, but a reloaded checkpoint starts with it set -> the after_reload trigger runs
EV_RESTORED = "[coop_ckpt]/restored"


def block(lines, depth):
    return "".join("\t" * depth + ln + CRLF for ln in lines)


def event_action(name, mode):
    return ['{"event"', '\t{event "%s"}' % name, '\t{mode %s}' % mode, '\t{delay 0}', '}']


def event_declarations():
    return block(['{event "%s"}' % e for e in (EV_CLICK, EV_RELOAD, EV_RESTORED)], 2)


TAG_RESTORED = "_coop_ckpt_restored"
LAST_SLOT = 17          # the game allows 18 scene players (ids 0..17)


def tag_players(first_coop_player, op):
    lines = []
    for slot in range(first_coop_player, LAST_SLOT + 1):
        lines += ['{"entity_state"', '\t{selector', '\t\t{source standart}', '\t\t{ignore_captured_by_user 0}',
                  '\t\t{player "%d"}' % slot, '\t\t{state "not dead"}', '\t}', '\t{%s %s}' % (op, TAG_RESTORED), '}']
    return lines


def autosave_actions(first_coop_player):
    """{"autosave"} wrapped so that only the checkpoint carries the "restored" marks (event + unit tag)."""
    return (event_action(EV_RESTORED, "set") + tag_players(first_coop_player, "tag_add") + ['{"autosave"}'] +
            tag_players(first_coop_player, "tag_remove") + event_action(EV_RESTORED, "reset"))


def reinforcement_entries():
    lines = []
    for key, ev in (("save", EV_CLICK), ("reload", EV_RELOAD)):
        lines += [
            '{"coop_ckpt_%s"' % key,
            '\t{state hidden}',
            '\t{text "mod/%s/%s"}' % (MOD_NAME, key),
            '\t{image coop_ckpt_%s}' % key,
            '\t{hint "mod/%s/%s_hint"}' % (MOD_NAME, key),
            '\t{count -1}',
            '\t{score 0}',
            '\t{delay %d}' % BUTTON_COOLDOWN,
            '\t{eventStart "%s"}' % ev,
            '}',
        ]
    return block(lines, 3)


def triggers(first_coop_player):
    lines = ['{"[coop_ckpt]/init"', '\t{condition', '\t}', '\t{actions',
             '\t\t{"delay"', '\t\t\t{time %d}' % INIT_DELAY, '\t\t}',
             '\t\t{"reinforcement"', '\t\t\t{operation show}', '\t\t\t{name coop_ckpt_save}', '\t\t}',
             '\t\t{"reinforcement"', '\t\t\t{operation show}', '\t\t\t{name coop_ckpt_reload}', '\t\t}',
             '\t}', '}']
    lines += ['{"[coop_ckpt]/click_save"', '\t{condition', '\t\t{terms', '\t\t\t{"1.event"',
              '\t\t\t\t{id "%s"}' % EV_CLICK, '\t\t\t}', '\t\t}', '\t}', '\t{actions']
    lines += ["\t\t" + ln for ln in event_action(EV_CLICK, "reset")]
    lines += ['\t\t{"trigger"', '\t\t\t{name "[coop_ckpt]/click_save"}', '\t\t}',
              '\t\t{"sound"', '\t\t\t{name "interface/message"}', '\t\t}']
    lines += ["\t\t" + ln for ln in autosave_actions(first_coop_player)]
    lines += ['\t}', '}']
    # reload request: an achievement command, which the game already forwards to every client.
    # "name" must be a real achievement (it is converted to an id, unknown names make the command invalid);
    # "nation" is a free string (std::string at +0x18 of the command): the DLL recognises coop_ckpt_reload.
    lines += ['{"[coop_ckpt]/click_reload"', '\t{condition', '\t\t{terms', '\t\t\t{"1.event"',
              '\t\t\t\t{id "%s"}' % EV_RELOAD, '\t\t\t}', '\t\t}', '\t}', '\t{actions',
              '\t\t{"achievement"', '\t\t\t{name rus_skirmish1_easy}', '\t\t\t{nation coop_ckpt_reload}', '\t\t}',
              '\t\t{"sound"', '\t\t\t{name "interface/message"}', '\t\t}']
    lines += ["\t\t" + ln for ln in event_action(EV_RELOAD, "reset")]
    lines += ['\t\t{"delay"', '\t\t\t{time 0.5}', '\t\t}',
              '\t\t{"trigger"', '\t\t\t{name "[coop_ckpt]/click_reload"}', '\t\t}', '\t}', '}']
    # after a reload: the multiplayer unit assignment ("Units assignment ... -> P4") is not part of the
    # saved scene, so the units of the coop player slots are autoassigned again (the server sends
    # cmMP_Autoassign for each of them).  Coop slots = players after the mission's aliased players.
    lines += ['{"[coop_ckpt]/after_reload"', '\t{condition', '\t\t{expression "1 | 2"}', '\t\t{terms',
              '\t\t\t{"1.event"', '\t\t\t\t{id "%s"}' % EV_RESTORED, '\t\t\t}',
              '\t\t\t{"2.entities"', '\t\t\t\t{selector', '\t\t\t\t\t{tag %s}' % TAG_RESTORED, '\t\t\t\t}',
              '\t\t\t\t{count', '\t\t\t\t\t{op ">"}', '\t\t\t\t\t{value 0}', '\t\t\t\t}', '\t\t\t}',
              '\t\t}', '\t}', '\t{actions']
    lines += ["\t\t" + ln for ln in event_action(EV_RESTORED, "reset")]
    lines += ['\t\t{"entity_state"', '\t\t\t{selector', '\t\t\t\t{source standart}', '\t\t\t\t{tag %s}' % TAG_RESTORED,
              '\t\t\t}', '\t\t\t{tag_remove %s}' % TAG_RESTORED, '\t\t}']
    lines += ['\t\t{"delay"', '\t\t\t{time 1}', '\t\t}']
    for slot in range(first_coop_player, LAST_SLOT + 1):
        lines += ['\t\t{"autoassign"', '\t\t\t{selector', '\t\t\t\t{ignore_captured_by_user 0}',
                  '\t\t\t\t{player "%d"}' % slot, '\t\t\t\t{state "not dead"}', '\t\t\t}', '\t\t}',
                  '\t\t{"delay"', '\t\t\t{time 0.1}', '\t\t}']
    lines += ['\t\t{"trigger"', '\t\t\t{name "[coop_ckpt]/after_reload"}', '\t\t}', '\t}', '}']
    return block(lines, 3)


def first_coop_player(text):
    """First scene player id of the humans, computed as the game does (mp_game.cpp, server 0xa52820): the
    multiplayer players get the ids after the highest owner of any actor in the scene ({Player N} of the
    top-level Human / Entity blocks).  The old rule (after the highest {alias}) was wrong for some missions."""
    owners = [int(o) for o in re.findall(r"\n\t\t\{Player (\d+)\}", text)]
    if owners:
        return max(owners) + 1
    m = re.search(r"\n\t\{Players\r\n(.*?)\r\n\t\}", text, re.S)
    aliases = [int(a) for a in re.findall(r"\{alias (\d+) ", m.group(1))] if m else []
    return max(aliases) + 1 if aliases else 4


def load_missions(game_dir):
    paks = sorted(glob.glob(os.path.join(game_dir, "resource", "*.pak")))
    paks.append(os.path.join(game_dir, "resource", "map", "single.pak"))
    missions = {}
    for pak in paks:
        if not os.path.isfile(pak):
            continue
        with zipfile.ZipFile(pak) as z:
            for n in z.namelist():
                if n.startswith("map/single/") and n.lower().endswith(".mi") and n not in missions:
                    missions[n] = z.read(n)
    return missions


def patch(data):
    text = data.decode("utf-8")
    if "[coop_ckpt]/init" in text:
        raise RuntimeError("already patched")
    first = first_coop_player(text)
    # the mission's own checkpoints carry the "restored" event too
    def wrap(mm):
        return CRLF.join(mm.group(1) + ln for ln in autosave_actions(first)) + CRLF
    text, autosaves = re.subn(r'(\t+)\{"autosave"\}\r\n', wrap, text)
    text, n = re.subn(r"(\n\t\{Helpers\r\n)", lambda mm: mm.group(1) + event_declarations(), text, count=1)
    if n != 1:
        raise RuntimeError("{Helpers} block not found")
    m = re.search(r"\n\t\t\{reinforcements\r\n(?:\t\t\t\{scoreTotal [^\r\n]*\}\r\n)?(?:\t\t\t\{scoreCurrent [^\r\n]*\}\r\n)?", text)
    if not m:
        raise RuntimeError("{reinforcements} block not found")
    text = text[:m.end()] + reinforcement_entries() + text[m.end():]
    text, n = re.subn(r"(\n\t\t\{triggers\r\n)", lambda mm: mm.group(1) + triggers(first), text, count=1)
    if n != 1:
        raise RuntimeError("{triggers} block not found")
    return text.encode("utf-8")


SAVE_HINT = 'msgid "Save the current state of the battle for every player (coop: every player needs this mod)."'


def write_localization(out):
    lines = ['msgid ""', 'msgstr ""', '"Project-Id-Version: Coop Checkpoint\\n"', '"Language: en\\n"',
             '"MIME-Version: 1.0\\n"', '"Content-Type: text/plain; charset=UTF-8\\n"',
             '"Content-Transfer-Encoding: 8bit\\n"', '',
             'msgctxt "mod/%s/save"' % MOD_NAME, 'msgid "Save checkpoint"', 'msgstr ""', '',
             'msgctxt "mod/%s/save_hint"' % MOD_NAME,
             SAVE_HINT,
             'msgstr ""', '',
             'msgctxt "mod/%s/reload"' % MOD_NAME, 'msgid "Reload checkpoint"', 'msgstr ""', '',
             'msgctxt "mod/%s/reload_hint"' % MOD_NAME,
             'msgid "Arm the reload: when the host restarts this mission, every player starts from the last checkpoint."',
             'msgstr ""', '']
    path = os.path.join(out, "localizations", "default", "interface", "text", MOD_NAME + ".pot")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(CRLF.join(lines).encode("utf-8"))
    it = ['msgid ""', 'msgstr ""', '"Project-Id-Version: Coop Checkpoint\\n"', '"Language: it\\n"',
          '"MIME-Version: 1.0\\n"', '"Content-Type: text/plain; charset=UTF-8\\n"',
          '"Content-Transfer-Encoding: 8bit\\n"', '',
          'msgctxt "mod/%s/save"' % MOD_NAME, 'msgid "Save checkpoint"', 'msgstr "Salva checkpoint"', '',
          'msgctxt "mod/%s/save_hint"' % MOD_NAME,
          SAVE_HINT,
          'msgstr "Salva lo stato attuale della battaglia per tutti i giocatori (coop: ogni giocatore deve avere la mod)."',
          '',
          'msgctxt "mod/%s/reload"' % MOD_NAME, 'msgid "Reload checkpoint"', 'msgstr "Ricarica checkpoint"', '',
          'msgctxt "mod/%s/reload_hint"' % MOD_NAME,
          'msgid "Arm the reload: when the host restarts this mission, every player starts from the last checkpoint."',
          'msgstr "Arma la ricarica: quando l\'host riavvia questa missione, tutti ripartono dall\'ultimo checkpoint."',
          '']
    path = os.path.join(out, "localizations", "it", "interface", "text", MOD_NAME + ".po")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(CRLF.join(it).encode("utf-8"))


def write_icons(game_dir, out):
    try:
        from PIL import Image, ImageDraw, ImageFont
    except ImportError:
        print("  Pillow not installed: falling back to the vanilla 'tactical' icon")
        return "tactical"
    scene = zipfile.ZipFile(os.path.join(game_dir, "resource", "interface", "scene.pak"))
    dst = os.path.join(out, "resource", "interface", "scene", "unit_icon")
    os.makedirs(dst, exist_ok=True)
    font = None
    for cand in ("C:/Windows/Fonts/impact.ttf", "C:/Windows/Fonts/arialbd.ttf"):
        if os.path.isfile(cand):
            font = ImageFont.truetype(cand, 21)
            break
    if font is None:
        font = ImageFont.load_default(size=18)
    for key, label in (("save", "SAVE\nCHECKPT"), ("reload", "RELOAD\nCHECKPT")):
        for state in range(4):
            raw = scene.read("scene/unit_icon/skip_preparation_eng_0%d.tga" % state)
            src = Image.open(io.BytesIO(raw))
            orientation = src.info.get("orientation", -1)
            im = src.convert("RGBA")
            draw = ImageDraw.Draw(im)
            cx, cy = 43, 36
            fill = (235, 235, 235, 255) if state != 3 else (140, 140, 140, 255)
            draw.multiline_text((cx, cy), label, font=font, fill=fill, anchor="mm", align="center",
                                stroke_width=2, stroke_fill=(0, 0, 0, 255), spacing=-2)
            im.save(os.path.join(dst, "coop_ckpt_%s_0%d.tga" % (key, state)), orientation=orientation)
    ic = Image.open(os.path.join(dst, "coop_ckpt_save_00.tga")).convert("RGBA")
    prev = Image.new("RGBA", (300, 170), (40, 40, 40, 255))
    prev.paste(ic, ((300 - ic.width) // 2, (170 - ic.height) // 2), ic)
    prev.convert("RGB").save(os.path.join(out, "preview.jpg"), quality=90)
    return None


MOD_INFO = """{mod
\t{name "%(name)s"}
\t{tags "Gameplay"}
\t{desc "Coop checkpoints: Save / Reload checkpoint buttons in the reinforcement panel of every campaign and skirmish mission. Every player of the coop match needs this mod AND binaries/x64/goh_mods/coop_checkpoint.dll (see README)."}
\t{minGameVersion "any"}
\t{maxGameVersion "any"}
}
""" % {"name": MOD_NAME}

README = r"""Coop Checkpoint - data mod
========================

Part of the Coop Checkpoint mod: it only works together with
<game folder>\binaries\x64\version.dll  and  <game folder>\binaries\x64\goh_mods\coop_checkpoint.dll.
EVERY player of the coop match needs both and must enable this mod in Options > Mods.

Save checkpoint: every PC saves the battle state at the same simulation step
  (Documents\My Games\gates of hell\profiles\<id>\save\coop_ckpt.sav; the host's local
  server writes coop_ckpt_srv.sav).  A new checkpoint overwrites the previous one.  The mission's own
  autosaves become checkpoints too.
Reload checkpoint: every PC arms the reload; the host's game leaves the match and starts the same
  mission again, and every PC loads its checkpoint instead of the mission start.

Logs: %TEMP%\goh_coop_checkpoint_<exe name>.log
"""


def build(game_dir, mods_dir=None):
    out = os.path.join(mods_dir or os.path.join(game_dir, "mods"), MOD_NAME)
    if os.path.isdir(out):
        shutil.rmtree(out)
    missions = load_missions(game_dir)
    for path, data in sorted(missions.items()):
        dst = os.path.join(out, "resource", *path.split("/"))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "wb") as f:
            f.write(patch(data))
    write_localization(out)
    fallback = write_icons(game_dir, out)
    if fallback:
        for p in glob.glob(os.path.join(out, "resource", "map", "**", "*.mi"), recursive=True):
            with open(p, "rb") as f:
                t = f.read()
            t = re.sub(rb"\{image coop_ckpt_[a-z]+\}", b"{image " + fallback.encode() + b"}", t)
            with open(p, "wb") as f:
                f.write(t)
    with open(os.path.join(out, "mod.info"), "wb") as f:
        f.write(MOD_INFO.replace("\n", CRLF).encode("utf-8"))
    with open(os.path.join(out, "README.txt"), "wb") as f:
        f.write(README.replace("\n", CRLF).encode("utf-8"))
    print("[%s] missions patched: %d" % (MOD_NAME, len(missions)))
    print("  -> " + out)


if __name__ == "__main__":
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    build(sys.argv[1], sys.argv[2] if len(sys.argv) == 3 else None)
