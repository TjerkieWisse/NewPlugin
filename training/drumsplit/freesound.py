# Drum one-shots from Freesound (freesound.org) that their makers released under CC0, so they can be
# used for anything without conditions: 808s, kicks, snares, claps, rims, toms, hi-hats, crashes and
# rides, many of them electronic. Uses Freesound's API with your API key (free: freesound.org/apiv2/apply)
# and downloads each sound's high-quality preview.
#
# Every sound is checked before it goes into the bank: one hit (not a loop or a few hits), starting at
# the start, and sounding like what it's labelled as (a "kick" that's mostly treble is left out).
# bank/freesound/credits.csv lists every sound used, with its maker.
import csv, io, json, os, time, urllib.error, urllib.parse, urllib.request
import numpy as np, soundfile as sf
from scipy import signal

API = 'https://freesound.org/apiv2/search/text/'
RATE = 44100
PER_INSTRUMENT = 300     # sounds kept per instrument at most

# What to search for, per instrument: search words, the length range in seconds, and the range of
# its "brightness" (spectral centroid, Hz) that a sound of that kind falls in.
SEARCHES = {
    'kick':         (['808 kick', 'kick drum one shot', 'kick'], (0.08, 2.5), (30, 1500)),
    'snare':        (['snare one shot', 'snare', 'clap', 'handclap'], (0.05, 1.5), (400, 7000)),
    'rim':          (['rimshot', 'rim shot', 'sidestick'], (0.03, 0.8), (500, 8000)),
    'toms':         (['tom drum', 'floor tom', '808 tom'], (0.1, 2.0), (40, 2500)),
    'hihat_closed': (['closed hihat', 'closed hi-hat', 'hihat closed'], (0.02, 0.6), (3000, 16000)),
    'hihat_open':   (['open hihat', 'open hi-hat'], (0.15, 3.0), (3000, 16000)),
    'crash':        (['crash cymbal'], (0.5, 6.0), (2000, 16000)),
    'ride':         (['ride cymbal'], (0.3, 5.0), (1500, 16000)),
}
# Words in a sound's name or tags that mean it's more than one hit.
NOT_ONE_SHOT = {'loop', 'loops', 'beat', 'beats', 'groove', 'bpm', 'pattern', 'break', 'breakbeat',
                'fill', 'roll', 'song', 'music', 'ambience', 'ambient', 'soundscape', 'sequence', 'kit'}


def _api(key, query, filt, page):
    params = urllib.parse.urlencode({
        'query': query, 'filter': filt, 'page': page, 'page_size': 150, 'sort': 'downloads_desc',
        'fields': 'id,name,tags,duration,username,license,previews,num_downloads'})
    request = urllib.request.Request(f'{API}?{params}', headers={'Authorization': f'Token {key}'})
    for attempt in range(6):
        try:
            with urllib.request.urlopen(request, timeout=60) as r:
                return json.load(r)
        except urllib.error.HTTPError as e:
            if e.code == 401:
                raise SystemExit('Freesound refused the API key: check it (freesound.org/home/app_permissions).')
            if e.code == 404:                        # no more pages
                return {'results': [], 'next': None}
            if e.code != 429 and attempt == 5:
                raise
        except OSError:
            if attempt == 5:
                raise
        time.sleep(2 ** attempt)                     # rate limit (60 searches a minute) or a hiccup


def candidates(key, inst):
    """Freesound's CC0 sounds for an instrument, most downloaded first, without obvious loops."""
    words, (lo, hi), _ = SEARCHES[inst]
    filt = f'license:"Creative Commons 0" duration:[{lo} TO {hi}]'
    found = {}
    for query in words:
        for page in (1, 2, 3):
            data = _api(key, query, filt, page)
            for s in data.get('results', []):
                text = set(t.lower() for t in s.get('tags', [])) | set(s['name'].lower().replace('_', ' ').replace('-', ' ').split())
                if not text & NOT_ONE_SHOT and s.get('previews'):
                    found.setdefault(s['id'], s)
            if not data.get('next') or len(found) >= PER_INSTRUMENT * 2:
                break
            time.sleep(1.1)
        time.sleep(1.1)
    return list(found.values())


def clean(audio, rate, inst):
    """The sound as a mono one-shot at RATE, starting at its hit and cut where it has died away, or
    None if it isn't one clear hit of this kind."""
    if audio.ndim > 1:
        audio = audio.mean(axis=1)
    if rate != RATE:
        audio = signal.resample_poly(audio, RATE, rate)
    peak = np.max(np.abs(audio))
    if peak < 1e-3:
        return None
    level = np.abs(audio) / peak
    start = int(np.argmax(level > 0.03))
    if start > 0.3 * RATE:                            # a long silence before the hit: probably not a one-shot
        return None
    audio = audio[max(0, start - int(0.002 * RATE)):]
    # One hit: after the loudest moment, the level never jumps back up to near the peak.
    frame = int(0.01 * RATE)
    env = np.sqrt(np.convolve(audio ** 2, np.ones(frame) / frame, mode='valid')[::frame] + 1e-12)
    top = int(np.argmax(env))
    after = env[top:]
    if len(after) > 6:
        low = np.minimum.accumulate(after)
        if np.any((after[3:] > 0.5 * env[top]) & (after[3:] > 3 * low[:-3])):
            return None
    # Cut where it has died away (-60 dB), with a short fade.
    loud = np.nonzero(np.abs(audio) > np.max(np.abs(audio)) * 1e-3)[0]
    audio = audio[:loud[-1] + int(0.01 * RATE)].copy()
    lo, hi = SEARCHES[inst][1]
    if not lo <= len(audio) / RATE <= hi * 1.2:
        return None
    fade = min(len(audio) // 4, int(0.01 * RATE))
    audio[-fade:] *= np.linspace(1, 0, fade)
    # It has to sound like what it says it is.
    spectrum = np.abs(np.fft.rfft(audio[:int(0.5 * RATE)], 8192)) ** 2
    freqs = np.fft.rfftfreq(8192, 1 / RATE)
    centroid = float(np.sum(freqs * spectrum) / (np.sum(spectrum) + 1e-12))
    c_lo, c_hi = SEARCHES[inst][2]
    if not c_lo <= centroid <= c_hi:
        return None
    return (audio / np.max(np.abs(audio)) * 0.9).astype(np.float32), centroid


def harvest(bank, key):
    """Adds the Freesound one-shots to the bank as one kit, 'freesound'; returns its index entry."""
    folder = os.path.join(bank, 'freesound')
    index, credits = {}, []
    for inst in SEARCHES:
        found = candidates(key, inst)
        print(f'  {inst}: {len(found)} candidates', flush=True)
        kept = []
        for s in found:
            if len(kept) >= PER_INSTRUMENT:
                break
            url = s['previews'].get('preview-hq-ogg') or s['previews'].get('preview-hq-mp3')
            try:
                with urllib.request.urlopen(url, timeout=60) as r:
                    audio, rate = sf.read(io.BytesIO(r.read()), dtype='float32')
            except Exception:                         # an odd file or a failed download: skip it
                continue
            result = clean(audio, rate, inst)
            if result is None:
                continue
            audio, centroid = result
            rel = os.path.join('freesound', inst, f'{s["id"]}.flac')
            os.makedirs(os.path.join(bank, 'freesound', inst), exist_ok=True)
            sf.write(os.path.join(bank, rel), audio, RATE, subtype='PCM_16')
            kept.append((centroid, rel))
            credits.append([s['id'], s['name'], s['username'], s['license'], f'https://freesound.org/s/{s["id"]}/'])
        print(f'  {inst}: {len(kept)} kept', flush=True)
        if inst == 'toms':                            # high, mid and low toms by brightness
            kept.sort(reverse=True)
            for k, part in enumerate(np.array_split(np.arange(len(kept)), 3)):
                index[f'tom{k}'] = [[[kept[i][1], None]] for i in part]
        else:
            index[inst] = [[[rel, None]] for _, rel in kept]
    os.makedirs(folder, exist_ok=True)
    with open(os.path.join(folder, 'credits.csv'), 'w', newline='') as f:
        writer = csv.writer(f)
        writer.writerow(['id', 'name', 'maker', 'licence', 'url'])
        writer.writerows(credits)
    return index
