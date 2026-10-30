# Real drum multitracks for training: StemGMD (Politecnico di Milano, CC BY 4.0), the Groove MIDI
# Dataset's drummers played on ten acoustic kits from Logic Pro's libraries, every drum on its own
# track. Where the generator builds a mix from one-shots, these are whole performances with a real
# sampler's round robins, velocities and hi-hat play, so the model hears drums as they sound in songs.
#
# StemGMD is 1.13 TB, split over seven archive files of 31 GB. Nobody needs all of it: this reads the
# archive's table of contents (16 MB) and fetches just the recordings it picks, straight from Zenodo.
# Each recording's nine drum tracks become our five parts (toms together, closed and open hi-hat
# together, crash and ride together), as FLAC in data/multitrack/.
#
# It picks performances and kits spread evenly, up to the hours of drums asked for. Groove MIDI's test
# performances are kept apart (half an hour of them), for checking the model on drums it never trained
# on. Stop it any time with Ctrl+C; run it again to carry on, or with more hours to add more.
#
# Usage: python multitrack.py [--data data] [--hours 20]   (or: fetch_data.py --multitrack 20)
import argparse, csv, io, json, os, random, shutil, struct, threading, time, urllib.error, urllib.request, zlib
from concurrent.futures import ThreadPoolExecutor, as_completed
import numpy as np, soundfile as sf

RATE = 44100
CLASSES = ['kick', 'snare', 'toms', 'hihat', 'cymbals']
PIECE_CLASS = {'kick': 'kick', 'snare': 'snare', 'hi_tom': 'toms', 'mid_tom': 'toms', 'low_tom': 'toms',
               'hihat_closed': 'hihat', 'hihat_open': 'hihat', 'crash_left': 'cymbals', 'ride': 'cymbals'}
# The split archive's pieces in order: .z01 ... .z06 in part 1 of the Zenodo record, .zip in part 2.
PIECES = [f'https://zenodo.org/api/records/7860223/files/StemGMD.z{i:02d}/content' for i in range(1, 7)] + \
         ['https://zenodo.org/api/records/7882857/files/StemGMD.zip/content']
PIECE_SIZE = 31138512896
CONTENTS = (6, 24691683826, 16216552)         # where the table of contents is: piece, offset, length
HELD_OUT_HOURS = 0.5


def fetch_range(piece, start, length, tries=6):
    """Bytes [start, start + length) of one archive piece, carrying on into the next piece if needed."""
    out = []
    while length > 0:
        end = start + length if piece == len(PIECES) - 1 else min(start + length, PIECE_SIZE)
        for attempt in range(tries):
            try:
                request = urllib.request.Request(PIECES[piece], headers={'Range': f'bytes={start}-{end - 1}'})
                with urllib.request.urlopen(request, timeout=120) as response:
                    if response.status != 206:
                        raise IOError(f'the server sent the whole file (status {response.status})')
                    data = response.read()
                if len(data) != end - start:
                    raise IOError(f'got {len(data)} of {end - start} bytes')
                break
            except (urllib.error.URLError, OSError) as e:
                if attempt == tries - 1:
                    raise IOError(f'Zenodo download failed: {e}') from e
                time.sleep(2 ** attempt * 2)
        out.append(data)
        length -= end - start
        piece, start = piece + 1, 0
    return b''.join(out)


def read_contents(blob):
    """The archive's table of contents: {name: (method, packed size, unpacked size, piece, offset)}."""
    entries, i = {}, 0
    while blob[i:i + 4] == b'PK\x01\x02':
        _, _, _, method, _, _, _, csize, usize, nlen, xlen, clen, piece, _, _, offset = \
            struct.unpack('<HHHHHHIIIHHHHHII', blob[i + 4:i + 46])
        name = blob[i + 46:i + 46 + nlen].decode('utf-8', 'replace')
        extra, j = blob[i + 46 + nlen:i + 46 + nlen + xlen], 0
        while j + 4 <= len(extra):                    # sizes and offsets past 4 GB are in the zip64 field
            tag, size = struct.unpack('<HH', extra[j:j + 4])
            if tag == 1:
                values, k = extra[j + 4:j + 4 + size], 0
                if usize == 0xFFFFFFFF:
                    usize = struct.unpack('<Q', values[k:k + 8])[0]; k += 8
                if csize == 0xFFFFFFFF:
                    csize = struct.unpack('<Q', values[k:k + 8])[0]; k += 8
                if offset == 0xFFFFFFFF:
                    offset = struct.unpack('<Q', values[k:k + 8])[0]; k += 8
                if piece == 0xFFFF:
                    piece = struct.unpack('<I', values[k:k + 4])[0]
            j += 4 + size
        entries[name] = (method, csize, usize, piece, offset)
        i += 46 + nlen + xlen + clen
    return entries


def fetch_file(entry):
    """One file out of the archive, unpacked."""
    method, csize, _, piece, offset = entry
    blob = fetch_range(piece, offset, 30 + 512 + csize)       # its header (name and extras), then the data
    if blob[:4] != b'PK\x03\x04':
        raise IOError('no file where the table of contents points')
    nlen, xlen = struct.unpack('<HH', blob[26:30])
    data = blob[30 + nlen + xlen:30 + nlen + xlen + csize]
    if len(data) < csize:                                     # the header had more extras than allowed for
        data += fetch_range(piece, offset + len(blob), csize - len(data))
    return zlib.decompressobj(-15).decompress(data) if method == 8 else data


def load_contents(downloads):
    """The table of contents (downloaded once) and Groove MIDI's split of the performances."""
    path = os.path.join(downloads, 'stemgmd_contents.bin')
    if not os.path.exists(path):
        print("Reading StemGMD's table of contents (16 MB)...", flush=True)
        open(path + '.tmp', 'wb').write(fetch_range(*CONTENTS))
        os.replace(path + '.tmp', path)
    entries = read_contents(open(path, 'rb').read())
    info = csv.DictReader(io.StringIO(fetch_file(entries['StemGMD/info.csv']).decode()))
    return entries, {row['audio_filename'][:-4]: row['split'] for row in info}


def recordings(entries):
    """{'kit/drummer/session/take': ({piece: archive entry}, seconds)}, for recordings with all nine
    drum tracks."""
    found = {}
    for name, entry in entries.items():
        p = name.split('/')
        if len(p) == 7 and p[1] == 'audio' and p[5] in PIECE_CLASS and name.endswith('.wav'):
            found.setdefault(f'{p[4]}/{p[2]}/{p[3]}/{p[6][:-4]}', {})[p[5]] = entry
    return {k: (v, (v['kick'][2] - 44) / (RATE * 4)) for k, v in found.items() if len(v) == len(PIECE_CLASS)}


def choose(available, split, hours, have):
    """Which recordings to fetch, in a fixed order, so asking for more hours later only adds: every
    performance in turn on a kit it doesn't have yet, so performances and kits spread evenly. Groove
    MIDI's test performances fill HELD_OUT_HOURS, apart from the rest."""
    rng = random.Random(1)
    kits_of = {}
    for key in sorted(available):
        kit, performance = key.split('/', 1)
        kits_of.setdefault(performance, []).append(kit)
    performances = sorted(kits_of)
    rng.shuffle(performances)
    for kits in kits_of.values():
        rng.shuffle(kits)
    picked = []
    for held_out, budget in ((True, HELD_OUT_HOURS * 3600), (False, hours * 3600)):
        group = [p for p in performances if (split.get(p) == 'test') == held_out]
        total = sum(have[k]['seconds'] for k in have if have[k]['held_out'] == held_out)
        for round_ in range(max(len(k) for k in kits_of.values())):
            for p in group:
                if round_ < len(kits_of[p]) and total < budget:
                    key = f'{kits_of[p][round_]}/{p}'
                    if key not in have:
                        picked.append((key, held_out))
                        total += available[key][1]
    return picked


def convert(key, pieces, folder):
    """Fetches one recording's nine tracks and writes our five parts. Returns the bytes fetched."""
    parts, fetched = {}, 0
    for piece, entry in pieces.items():
        audio, rate = sf.read(io.BytesIO(fetch_file(entry)), dtype='float32', always_2d=True)
        fetched += entry[1]
        if rate != RATE:
            raise IOError(f'{key}: {rate} Hz')
        c = PIECE_CLASS[piece]
        if c in parts:
            n = max(len(parts[c]), len(audio))
            parts[c] = np.pad(parts[c], ((0, n - len(parts[c])), (0, 0))) + np.pad(audio, ((0, n - len(audio)), (0, 0)))
        else:
            parts[c] = audio
    n = max(len(a) for a in parts.values())
    parts = {c: np.pad(a, ((0, n - len(a)), (0, 0))) for c, a in parts.items()}
    peak = max(np.max(np.abs(a)) for a in parts.values())
    scale = 0.99 / peak if peak > 0.99 else 1.0               # summed tracks can go over full scale
    out = os.path.join(folder, key)
    os.makedirs(out + '.tmp', exist_ok=True)
    for c, a in parts.items():
        sf.write(os.path.join(out + '.tmp', c + '.flac'), a * scale, RATE, subtype='PCM_16')
    if os.path.exists(out):
        shutil.rmtree(out)
    os.replace(out + '.tmp', out)
    return fetched, n / RATE


def fetch(data, hours, workers=6):
    folder = os.path.join(data, 'multitrack')
    downloads = os.path.join(data, 'downloads')
    os.makedirs(downloads, exist_ok=True)
    os.makedirs(folder, exist_ok=True)
    index_path = os.path.join(folder, 'index.json')
    have = json.load(open(index_path)) if os.path.exists(index_path) else {}
    entries, split = load_contents(downloads)
    available = recordings(entries)
    todo = choose(available, split, hours, have)
    if not todo:
        print(f'Multitracks ready: {sum(v["seconds"] for v in have.values()) / 3600:.1f} hours.')
        return
    packed = sum(available[k][0][p][1] for k, _ in todo for p in available[k][0])
    free = shutil.disk_usage(folder).free
    print(f'Fetching {len(todo)} recordings, {sum(available[k][1] for k, _ in todo) / 3600:.1f} hours of drums: '
          f'{packed / 1e9:.1f} GB to download, about {packed * 0.4 / 1e9:.1f} GB on disk ({free / 1e9:.0f} GB free).', flush=True)
    if packed * 0.4 > free * 0.9:
        raise SystemExit('Not enough free disk space: ask for fewer --hours, or free some space first.')

    lock, done, started = threading.Lock(), [0, 0], time.time()

    def save():
        json.dump(have, open(index_path + '.tmp', 'w'), indent=0)
        os.replace(index_path + '.tmp', index_path)

    pool = ThreadPoolExecutor(workers)
    jobs = {pool.submit(convert, key, available[key][0], folder): (key, held_out) for key, held_out in todo}
    try:
        for job in as_completed(jobs):
            key, held_out = jobs[job]
            try:
                fetched, seconds = job.result()
            except Exception as e:
                print(f'  {key}: skipped ({e})', flush=True)
                continue
            with lock:
                have[key] = {'seconds': round(seconds, 3), 'held_out': held_out}
                done[0] += 1
                done[1] += fetched
                if done[0] % 20 == 0 or done[0] == len(todo):
                    save()
                    rate = done[1] / (time.time() - started)
                    print(f'  {done[0]}/{len(todo)} recordings, {done[1] / 1e9:.1f} of {packed / 1e9:.1f} GB, '
                          f'{rate / 1e6:.1f} MB/s, about {(packed - done[1]) / rate / 3600:.1f} h to go', flush=True)
    except KeyboardInterrupt:
        print('Stopping; what is fetched so far is kept. Run the same command again to carry on.', flush=True)
        pool.shutdown(wait=False, cancel_futures=True)
        save()
        raise SystemExit(1)
    pool.shutdown()
    save()
    print(f'Multitracks ready: {sum(v["seconds"] for v in have.values()) / 3600:.1f} hours.')


class Multitracks:
    """The fetched recordings, for the generator: excerpts of the five parts."""

    def __init__(self, data):
        self.folder = os.path.join(data, 'multitrack')
        path = os.path.join(self.folder, 'index.json')
        index = json.load(open(path)) if os.path.exists(path) else {}
        self.sets = {}
        for held_out in (False, True):
            keys = sorted(k for k, v in index.items() if v['held_out'] == held_out)
            seconds = np.array([index[k]['seconds'] for k in keys])
            self.sets[held_out] = (keys, seconds / seconds.sum()) if keys else None

    def available(self, held_out=False):
        return self.sets[held_out] is not None

    def read(self, rng, n, held_out=False):
        """n samples of one recording's parts [5, 2, n], from a random point: a recording shorter
        than that lands somewhere in the excerpt, with silence around it."""
        keys, weights = self.sets[held_out]
        key = keys[rng.choice(len(keys), p=weights)]
        folder = os.path.join(self.folder, key)
        length = sf.info(os.path.join(folder, 'kick.flac')).frames
        start = int(rng.integers(0, max(1, length - n + 1)))
        at = int(rng.integers(0, n - length + 1)) if length < n else 0
        parts = np.zeros((len(CLASSES), 2, n), np.float32)
        for i, c in enumerate(CLASSES):
            audio, _ = sf.read(os.path.join(folder, c + '.flac'), start=start, frames=min(n, length), dtype='float32', always_2d=True)
            parts[i, :, at:at + len(audio)] = audio.T
        return parts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--data', default='data')
    ap.add_argument('--hours', type=float, default=20, help='hours of drums to train on (on top of the check set)')
    ap.add_argument('--workers', type=int, default=6, help='downloads at the same time')
    args = ap.parse_args()
    fetch(args.data, args.hours, args.workers)


if __name__ == '__main__':
    main()
