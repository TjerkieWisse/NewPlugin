# Downloads the training material and prepares it in data/:
#   data/groove/  the Groove MIDI Dataset's MIDI files (Google Magenta, CC BY 4.0), 3 MB
#   data/bank/    recorded drum sounds as one-shots per instrument: the single hits of StemGMD's ten
#                 kits (Politecnico di Milano, CC BY 4.0; 200 MB), three DrumGizmo kits (CC BY 4.0),
#                 and CC0 one-shots from Freesound (needs a free API key; see freesound.py)
#
# The DrumGizmo kits are recorded on up to 16 microphones. For every hit this keeps two stereo
# mixdowns, the close microphones and the room (overheads and ambience), so the generator can blend
# them differently every time. Downloads resume if interrupted.
#
# Usage: python fetch_data.py [--data data] [--groove-only] [--kits stemgmd,freesound,drs,muldjord,crocell]
#                             [--delete-downloads]
import argparse, io, json, os, re, sys, urllib.error, urllib.request, zipfile
import xml.etree.ElementTree as ET
import numpy as np, soundfile as sf
from scipy import signal

RATE = 44100
GROOVE_URL = 'https://storage.googleapis.com/magentadata/datasets/groove/groove-v1.0.0-midionly.zip'
KITS = {
    'stemgmd': 'https://zenodo.org/records/7882857/files/StemGMD_single_hits.zip?download=1',
    'drs': 'https://drumgizmo.org/kits/DRSKit/DRSKit2_1.zip',
    'muldjord': 'https://drumgizmo.org/kits/MuldjordKit/MuldjordKit3.zip',
    'crocell': 'https://drumgizmo.org/kits/CrocellKit/CrocellKit1_1.zip',
}
MAX_LAYERS = 20          # velocity layers kept per instrument, spread from soft to hard


def download(url, path):
    """Downloads url to path, resuming a partial file."""
    done = os.path.getsize(path) if os.path.exists(path) else 0
    request = urllib.request.Request(url, headers={'Range': f'bytes={done}-'} if done else {})
    try:
        response = urllib.request.urlopen(request)
    except urllib.error.HTTPError as e:
        if e.code == 416:                             # nothing left to fetch: already complete
            return
        raise
    with response:
        if done and response.status != 206:
            done = 0                                  # the server ignored the range: start over
        total = done + int(response.headers.get('Content-Length', 0))
        if total and done >= total:
            return
        with open(path, 'ab' if done else 'wb') as f:
            shown = -1
            while chunk := response.read(1 << 20):
                f.write(chunk)
                done += len(chunk)
                if total and int(done * 20 / total) != shown:
                    shown = int(done * 20 / total)
                    print(f'  {os.path.basename(path)}: {done / 1e6:.0f} of {total / 1e6:.0f} MB', flush=True)


def fetch_groove(data, downloads):
    target = os.path.join(data, 'groove')
    if os.path.isdir(target) and any(f.endswith('.mid') for _, _, fs in os.walk(target) for f in fs):
        print('Grooves already there.')
        return
    path = os.path.join(downloads, 'groove-midionly.zip')
    print('Downloading the Groove MIDI Dataset...')
    download(GROOVE_URL, path)
    with zipfile.ZipFile(path) as z:
        for name in z.namelist():
            if name.endswith('.mid'):
                out = os.path.join(target, os.path.relpath(name, 'groove'))
                os.makedirs(os.path.dirname(out), exist_ok=True)
                open(out, 'wb').write(z.read(name))
    print('Grooves ready.')


# --- StemGMD ---------------------------------------------------------------------------------------

# StemGMD's names for its nine pieces; each comes at ten velocities, 0 the hardest and 9 the softest.
STEMGMD_PIECES = {
    'kick': 'kick', 'snare': 'snare', 'hihat_closed': 'hihat_closed', 'hihat_open': 'hihat_open',
    'hi_tom': 'tom0', 'mid_tom': 'tom1', 'low_tom': 'tom2', 'crash_left': 'crash', 'ride': 'ride',
}


def convert_stemgmd(zip_path, bank):
    """StemGMD's single hits: ten kits, each a kit of its own in the bank (already stereo mixes, so
    there's no separate room part)."""
    z = zipfile.ZipFile(zip_path)
    hits = {}
    for name in z.namelist():
        parts = name.split('/')
        if name.endswith('.wav') and len(parts) == 3:
            piece, velocity = parts[2][:-4].rsplit('-', 1)
            if piece in STEMGMD_PIECES:
                hits.setdefault(parts[1], {}).setdefault(piece, {})[int(velocity)] = name
    index = {}
    for kit, pieces in sorted(hits.items()):
        name = 'stemgmd_' + kit
        index[name] = {}
        for piece, files in sorted(pieces.items()):
            entries = []
            for j, velocity in enumerate(sorted(files, reverse=True)):            # soft to hard
                audio, rate = sf.read(io.BytesIO(z.read(files[velocity])), dtype='float32', always_2d=True)
                if rate != RATE:
                    audio = signal.resample_poly(audio, RATE, rate, axis=0)
                level = np.max(np.abs(audio), axis=1)
                start = max(0, int(np.argmax(level > level.max() * 0.01)) - int(0.002 * RATE))  # the hit starts on time
                audio, _ = trim(audio[start:].T, np.zeros((2, len(audio) - start)))
                rel = os.path.join(name, piece, f'{j:02d}.flac')
                os.makedirs(os.path.join(bank, name, piece), exist_ok=True)
                sf.write(os.path.join(bank, rel), audio.T, RATE, subtype='PCM_16')
                entries.append([rel, None])
            index[name].setdefault(STEMGMD_PIECES[piece], []).append(entries)
        print(f'  {name}: {len(pieces)} pieces', flush=True)
    return index


# --- DrumGizmo kits -------------------------------------------------------------------------------

def instrument_for(name, note):
    """Which of the generator's instruments a DrumGizmo instrument is, from its name (and MIDI note)."""
    n = name.lower()
    if 'trigger' in n:
        return None
    if any(k in n for k in ('kdrum', 'kick', 'bassdrum', 'bdrum')):
        return 'kick'
    if any(k in n for k in ('rest', 'side', 'stick', 'rim')) and 'snare' in n or n in ('sidestick', 'xstick'):
        return 'rim'
    if 'snare' in n:
        return 'snare'
    if 'hat' in n or n.startswith('hh'):
        if 'open' in n or note == 46:
            return 'hihat_open'
        return 'hihat_closed'
    if 'tom' in n or 'floor' in n:
        return 'tom'
    if 'ride' in n:
        return 'ride'
    if any(k in n for k in ('crash', 'china', 'splash', 'cymbal', 'stack')):
        return 'crash'
    return None


def is_room(channel):
    c = channel.lower()
    return any(k in c for k in ('amb', 'room', 'oh', 'overhead'))


def channel_pan(channel, tom_channels):
    """Where a microphone sits in the stereo mixdown."""
    c = channel.lower()
    if is_room(channel):
        return -0.8 if c.endswith('l') or 'left' in c else 0.8 if c.endswith('r') or 'right' in c else 0.0
    if channel in tom_channels:
        i = tom_channels.index(channel)
        return -0.3 + 0.6 * i / max(1, len(tom_channels) - 1)
    if 'hat' in c or c.startswith('hh'):
        return -0.3
    if 'ride' in c:
        return 0.3
    return 0.0


def mixdown(audio_by_channel, tom_channels):
    """Close and room stereo mixdowns of one hit's microphone channels."""
    length = max(len(a) for a in audio_by_channel.values())
    close, room = np.zeros((2, length)), np.zeros((2, length))
    for channel, audio in audio_by_channel.items():
        angle = (channel_pan(channel, tom_channels) + 1) * np.pi / 4
        stereo = np.outer([np.cos(angle), np.sin(angle)], audio) * np.sqrt(2)
        (room if is_room(channel) else close)[:, :len(audio)] += stereo
    return close, room


def trim(close, room):
    """Cuts the silence after the hit has died away (-70 dB), with a short fade."""
    level = np.max(np.abs(close + room), axis=0)
    loud = np.nonzero(level > level.max() * 10 ** (-70 / 20))[0]
    end = min(len(level), (loud[-1] if len(loud) else 0) + int(0.02 * RATE))
    fade = np.linspace(1, 0, min(end, int(0.01 * RATE)))
    close, room = close[:, :end].copy(), room[:, :end].copy()
    close[:, end - len(fade):] *= fade
    room[:, end - len(fade):] *= fade
    return close, room


def convert_kit(zip_path, kit, bank):
    z = zipfile.ZipFile(zip_path)
    names = z.namelist()
    # The kit file (some kits come with several variants: take the one with the most instruments).
    kits = [(n, ET.fromstring(z.read(n))) for n in names if n.endswith('.xml') and n.count('/') == 1]
    kit_xml, drumkit = max(((n, x) for n, x in kits if x.tag == 'drumkit'), key=lambda k: len(list(k[1].iter('instrument'))))
    root_dir = kit_xml.split('/')[0]
    midimap = {}
    for n in names:
        if n.lower().endswith('midimap.xml') and n.count('/') == 1:
            for m in ET.fromstring(z.read(n)).iter('map'):
                midimap.setdefault(m.get('instr'), int(m.get('note')))
    channels = [c.get('name') for c in drumkit.iter('channel') if 'trigger' not in c.get('name').lower()]
    tom_channels = [c for c in channels if 'tom' in c.lower() or 'floor' in c.lower()]

    instruments = []
    for inst in drumkit.iter('instrument'):
        kind = instrument_for(inst.get('name'), midimap.get(inst.get('name')))
        if kind:
            instruments.append((inst.get('name'), kind, os.path.join(root_dir, inst.get('file')).replace('\\', '/')))
    toms = sorted((i for i in instruments if i[1] == 'tom'), key=lambda i: -midimap.get(i[0], 0))  # high to low
    index = {}
    for name, kind, xml_path in instruments:
        if kind == 'tom':
            k = toms.index(next(t for t in toms if t[0] == name))
            kind = f'tom{round(k * 2 / max(1, len(toms) - 1))}'
        inst_xml = ET.fromstring(z.read(xml_path))
        samples = sorted(inst_xml.iter('sample'), key=lambda s: float(s.get('power', 0)))
        keep = np.unique(np.linspace(0, len(samples) - 1, min(MAX_LAYERS, len(samples))).round().astype(int))
        layers, peak = [], 1e-9
        files = {}
        for i in keep:
            per_channel = {}
            for af in samples[i].iter('audiofile'):
                if af.get('channel') not in channels:
                    continue
                path = os.path.normpath(os.path.join(os.path.dirname(xml_path), af.get('file'))).replace('\\', '/')
                if path not in files:
                    audio, rate = sf.read(io.BytesIO(z.read(path)), dtype='float32', always_2d=True)
                    if rate != RATE:
                        audio = signal.resample_poly(audio, RATE, rate, axis=0)
                    files = {path: audio}                 # one hit's file at a time
                per_channel[af.get('channel')] = files[path][:, int(af.get('filechannel', 1)) - 1]
            if per_channel:
                close, room = trim(*mixdown(per_channel, tom_channels))
                layers.append((close, room))
                peak = max(peak, np.max(np.abs(close + room)))
        folder = os.path.join(bank, kit, re.sub(r'\W', '_', name))
        os.makedirs(folder, exist_ok=True)
        entries = []
        for j, (close, room) in enumerate(layers):
            paths = []
            for part, audio in (('close', close), ('room', room)):
                rel = os.path.join(kit, re.sub(r'\W', '_', name), f'{j:02d}_{part}.flac')
                sf.write(os.path.join(bank, rel), (audio * 0.9 / peak).T, RATE, subtype='PCM_24')
                paths.append(rel)
            entries.append(paths)
        index.setdefault(kind, []).append(entries)
        print(f'  {name} -> {kind}: {len(entries)} layers', flush=True)
    return index


def freesound_key(data):
    """The Freesound API key: from FREESOUND_KEY, or data/freesound_key.txt, or asked for once and kept
    there (data/ stays out of git)."""
    path = os.path.join(data, 'freesound_key.txt')
    key = os.environ.get('FREESOUND_KEY') or (open(path).read().strip() if os.path.exists(path) else '')
    if not key:
        key = input('Freesound API key (from freesound.org/apiv2/apply): ').strip()
        open(path, 'w').write(key)
    return key


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--data', default='data')
    ap.add_argument('--groove-only', action='store_true')
    ap.add_argument('--kits', default='stemgmd,freesound,drs,muldjord,crocell')
    ap.add_argument('--delete-downloads', action='store_true')
    args = ap.parse_args()

    downloads = os.path.join(args.data, 'downloads')
    os.makedirs(downloads, exist_ok=True)
    fetch_groove(args.data, downloads)
    if args.groove_only:
        return

    bank = os.path.join(args.data, 'bank')
    index_path = os.path.join(bank, 'index.json')
    index = json.load(open(index_path)) if os.path.exists(index_path) else {}
    for kit in args.kits.split(','):
        if any(k == kit or k.startswith(kit + '_') for k in index):
            print(f'{kit} already in the bank.')
            continue
        if kit == 'freesound':
            import freesound
            print('Fetching one-shots from Freesound...')
            index[kit] = freesound.harvest(bank, freesound_key(args.data))
            json.dump(index, open(index_path, 'w'), indent=1)
            continue
        zip_path = os.path.join(downloads, os.path.basename(KITS[kit]).split('?')[0])
        print(f'Downloading {kit}...')
        download(KITS[kit], zip_path)
        print(f'Converting {kit}...')
        if kit == 'stemgmd':
            index.update(convert_stemgmd(zip_path, bank))
        else:
            index[kit] = convert_kit(zip_path, kit, bank)
        json.dump(index, open(index_path, 'w'), indent=1)
        if args.delete_downloads:
            os.remove(zip_path)
    print('Bank ready:', ', '.join(index))


if __name__ == '__main__':
    main()
