# Your own drum one-shots, for training: put them in my_sounds/ next to this script, in folders as you
# like, for example
#   my_sounds/realistic/Kicks/Kick 01.wav
#   my_sounds/trap/808s/808 Long.wav
#   my_sounds/house/Hats/Open Hat 3.wav
# Each folder directly in my_sounds/ becomes a collection of its own (realistic, trap, house). Which
# instrument a sound is, is read from its file name, or else the nearest folder name that says (kick,
# 808, snare, clap, rim, tom, hat, open hat, crash, ride, ...); sounds that name nothing recognisable
# (perc, shaker, ...) or aren't single hits (reverse cymbals, rolls, fills, loops) are skipped and listed. Every sound gets the same checks as Freesound's: one clear hit, sounding like
# what it's called.
#
# Only use sounds you may use to train a model you sell: ones you made or recorded yourself, or with
# the maker's permission. Many sample packs forbid this in their licence.
import os, re
import numpy as np, soundfile as sf
import freesound

AUDIO = ('.wav', '.aif', '.aiff', '.flac', '.ogg', '.mp3')

# Checked in this order, so "808 snare" is a snare and "open hat" is an open hi-hat. Words match as a
# whole, except those in PART_OF_WORD, which may also be part of one ("kick" in "808kick").
KINDS = [
    ('hihat_open', {'open', 'ohh', 'ohat', 'openhat', 'openhh'}, {'hat', 'hh', 'hihat', 'hihats', 'hats'}),
    ('hihat_closed', {'hat', 'hats', 'hh', 'hihat', 'hihats', 'chh', 'chat', 'closedhat', 'closedhh'}, None),
    ('crash', {'crash', 'crashes', 'splash', 'china', 'cymbal', 'cymbals', 'cym'}, None),
    ('ride', {'ride', 'rides'}, None),
    ('rim', {'rim', 'rims', 'rimshot', 'rimshots', 'sidestick', 'stick', 'sticks'}, None),
    ('snare', {'snare', 'snares', 'snr', 'sd', 'clap', 'claps', 'snap', 'snaps'}, None),
    ('toms', {'tom', 'toms', 'floortom'}, None),
    ('kick', {'kick', 'kicks', 'kik', 'bd', 'bassdrum', '808', '808s'}, None),
]
PART_OF_WORD = {'kick', 'kicks', 'snare', 'snares', 'hihat', 'hihats', 'crash', 'clap', 'claps', 'rimshot',
                'sidestick', 'bassdrum', 'openhat', 'closedhat', 'floortom', 'cymbal', 'cymbals'}
# Not single hits, whatever else the name says: reverse cymbals and swells, rolls, fills, loops.
NOT_A_HIT = {'reverse', 'reversed', 'rev', 'riser', 'risers', 'swell', 'swells', 'sweep', 'roll', 'rolls',
             'fill', 'fills', 'loop', 'loops', 'fx', 'buildup', 'uplifter', 'downlifter'}
SEARCH_NAME = {'snare': 'snare', 'rim': 'rim', 'toms': 'toms', 'kick': 'kick', 'hihat_open': 'hihat_open',
               'hihat_closed': 'hihat_closed', 'crash': 'crash', 'ride': 'ride'}


def _kind_in(name):
    words = re.findall(r'[a-z]+|\d+[a-z]*', name.lower())
    text = ''.join(words)
    def has(tokens):
        return any(w in tokens for w in words) or any(t in text for t in tokens & PART_OF_WORD)
    for kind, tokens, also in KINDS:
        if has(tokens) and (also is None or has(also)):
            return kind
    return None


def kind_of(path):
    """The instrument a file is, or None: from its own name if that says, otherwise from the folder
    it's in, then the one above, and so on (so 'Kicks & Snares/Kick 1.wav' is a kick)."""
    parts = path.replace('\\', '/').split('/')
    parts[-1] = os.path.splitext(parts[-1])[0]
    if any(w in NOT_A_HIT for part in parts for w in re.findall(r'[a-z]+', part.lower())):
        return None
    for name in reversed(parts):
        kind = _kind_in(name)
        if kind:
            return kind
    return None


def harvest(bank, folder):
    """Adds every collection in `folder` to the bank as a kit 'mine_<name>'; returns their entries."""
    index = {}
    for collection in sorted(os.listdir(folder)):
        root = os.path.join(folder, collection)
        if not os.path.isdir(root) or collection.startswith('.'):
            continue
        name = 'mine_' + re.sub(r'\W+', '_', collection.lower()).strip('_')
        kept, skipped, rejected, origins = {}, [], [], {}
        for here, _, files in os.walk(root):
            for f in sorted(files):
                if not f.lower().endswith(AUDIO) or f.startswith('.'):
                    continue
                path = os.path.join(here, f)
                rel_in = os.path.relpath(path, root)
                kind = kind_of(rel_in)
                if kind is None:
                    skipped.append(rel_in)
                    continue
                try:
                    audio, rate = sf.read(path, dtype='float32')
                except Exception:
                    rejected.append(rel_in)
                    continue
                result = freesound.clean(audio, rate, SEARCH_NAME[kind], longer=2.0)
                if result is None:
                    rejected.append(rel_in)
                    continue
                audio, centroid = result
                n = sum(len(v) for v in kept.values())
                rel = os.path.join(name, kind, f'{n:05d}.flac')
                os.makedirs(os.path.join(bank, name, kind), exist_ok=True)
                sf.write(os.path.join(bank, rel), audio, freesound.RATE, subtype='PCM_24')
                kept.setdefault(kind, []).append((centroid, rel))
                origin = os.path.dirname(rel_in) or '.'
                origins.setdefault(kind, {})
                origins[kind][origin] = origins[kind].get(origin, 0) + 1
        entries = {}
        for kind, sounds in kept.items():
            if kind == 'toms':                        # high, mid and low toms by brightness
                sounds.sort(reverse=True)
                for k, part in enumerate(np.array_split(np.arange(len(sounds)), 3)):
                    if len(part):
                        entries[f'tom{k}'] = [[[sounds[i][1], None]] for i in part]
            else:
                entries[kind] = [[[rel, None]] for _, rel in sounds]
        index[name] = entries
        print(f'  {name}: ' + ', '.join(f'{k} {len(v)}' for k, v in sorted(kept.items())) +
              f'; {len(skipped)} skipped (name not recognised), {len(rejected)} rejected by the checks', flush=True)
        for kind, folders in sorted(origins.items()):     # where each instrument's sounds came from
            top = sorted(folders.items(), key=lambda f: -f[1])[:4]
            print(f'    {kind} from: ' + '; '.join(f'{folder} ({n})' for folder, n in top), flush=True)
        for label, items in (('not recognised', skipped), ('rejected', rejected)):
            if items:
                print(f'    {label}, for example: ' + '; '.join(items[:8]), flush=True)
    return index
