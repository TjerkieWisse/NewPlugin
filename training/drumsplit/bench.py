# Speed test: how fast this computer can train the drum split model, so we know whether a full run
# takes days or a week. Times real training steps for the three model sizes at a few batch sizes,
# checks memory (and whether macOS had to swap), and how fast the CPU makes training examples.
# Takes about 10 minutes; writes everything to bench_results.txt as well.
#
# Usage: python bench.py [--data data] [--steps 15]
import argparse, os, platform, subprocess, sys, time
os.environ.setdefault('PYTORCH_ENABLE_MPS_FALLBACK', '1')
# On a Mac the GPU shares memory with everything else. Cap it at 70% of what macOS recommends, so a
# setting that doesn't fit fails cleanly here instead of making the Mac swap to its SSD.
os.environ.setdefault('PYTORCH_MPS_HIGH_WATERMARK_RATIO', '0.7')
import numpy as np, torch
import model as M

# A full training run, as examples seen: 300,000 segments of 3 s, about 250 hours of drums.
FULL_RUN_EXAMPLES = 300_000

lines = []


def say(text=''):
    print(text, flush=True)
    lines.append(text)


def swap_used_mb():
    if platform.system() != 'Darwin':
        return None
    out = subprocess.run(['sysctl', 'vm.swapusage'], capture_output=True, text=True).stdout
    try:
        return float(out.split('used = ')[1].split('M')[0])
    except (IndexError, ValueError):
        return None


def sync(device):
    if device.type == 'mps':
        torch.mps.synchronize()
    elif device.type == 'cuda':
        torch.cuda.synchronize()


def memory_gb(device):
    if device.type == 'mps':
        return torch.mps.driver_allocated_memory() / 1e9
    if device.type == 'cuda':
        return torch.cuda.max_memory_allocated() / 1e9
    return float('nan')


def free_memory(device):
    if device.type == 'mps':
        torch.mps.empty_cache()
    elif device.type == 'cuda':
        torch.cuda.empty_cache()
        torch.cuda.reset_peak_memory_stats()


def time_training(device, cfg, batch, steps, half):
    dim_f, dim_t = cfg['audio']['dim_f'], cfg['audio']['dim_t']
    net = M.Net(cfg).to(device)
    opt = torch.optim.AdamW(net.parameters(), lr=1e-4)
    n = M.frames_to_samples(dim_t)
    mix = torch.randn(batch, 2, n, device=device) * 0.1
    parts = torch.randn(batch, 5, 2, n, device=device) * 0.05
    def step():
        with torch.autocast(device.type, dtype=torch.float16, enabled=half):
            spec = M.spectrogram(mix, dim_f)
            loss = M.loss_fn(net(spec).float(), spec, parts, dim_f)
        opt.zero_grad(set_to_none=True)
        loss.backward()
        opt.step()
    for _ in range(3):                            # warm-up: the first steps compile kernels
        step()
    sync(device)
    start = time.time()
    for _ in range(steps):
        step()
    sync(device)
    seconds = (time.time() - start) / steps
    mem = memory_gb(device)
    del net, opt, mix, parts
    free_memory(device)
    return seconds, mem


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--data', default='data')
    ap.add_argument('--steps', type=int, default=15)
    ap.add_argument('--sizes', default='small,medium,large')
    ap.add_argument('--batches', default='2,4,8')
    args = ap.parse_args()

    device = torch.device('mps' if torch.backends.mps.is_available() else 'cuda' if torch.cuda.is_available() else 'cpu')
    say(f'KITNAP drum split speed test - {platform.platform()}, {platform.processor() or platform.machine()}')
    say(f'torch {torch.__version__}, device {device}, {os.cpu_count()} CPU cores')
    if device.type == 'cpu':
        say('No GPU found: training on this computer would be far too slow. The numbers below are for the CPU.')
    swap_before = swap_used_mb()

    # 1. How fast the CPU makes examples (one process; training uses several).
    import generator
    examples = iter(generator.Examples(args.data, M.frames_to_samples(256) / 44100, seed=0))
    next(examples)
    start = time.time()
    for _ in range(10):
        next(examples)
    per_example = (time.time() - start) / 10
    workers = max(1, (os.cpu_count() or 4) - 2)
    say(f'\nExamples: {per_example * 1000:.0f} ms each on one core, about {workers / per_example:.0f} per second with {workers} workers')

    # 2. Training speed per model size and batch size.
    say('\nsize    params  batch  precision  s/step  examples/s  GPU memory  full run (300k examples)')
    results = []
    for size in args.sizes.split(','):
        cfg = M.load_config(os.path.join('configs', size + '.yaml'))
        params = sum(p.numel() for p in M.Net(cfg).parameters()) / 1e6
        for batch in [int(b) for b in args.batches.split(',')]:
            for half in ((False, True) if device.type != 'cpu' else (False,)):   # fp16 is only fast on a GPU
                try:
                    seconds, mem = time_training(device, cfg, batch, args.steps, half)
                except RuntimeError as e:          # out of memory, or an op this device can't do
                    say(f'{size:7} {params:5.1f}M  {batch:5}  {"fp16" if half else "fp32":9}  failed: {str(e).splitlines()[0][:60]}')
                    free_memory(device)
                    continue
                rate = batch / seconds
                hours = FULL_RUN_EXAMPLES / rate / 3600
                results.append((hours, size, batch, half, mem))
                memory = f'{mem:8.1f} GB' if mem == mem else '       -   '
                say(f'{size:7} {params:5.1f}M  {batch:5}  {"fp16" if half else "fp32":9}  {seconds:6.2f}  {rate:10.1f}  {memory}  {hours:6.1f} h ({hours / 24:.1f} days)')
    swap_after = swap_used_mb()

    say('')
    if swap_before is not None and swap_after is not None:
        grew = swap_after - swap_before
        say(f'Swap: {swap_before:.0f} MB before, {swap_after:.0f} MB after' +
            (' - macOS ran short of memory during the test.' if grew > 500 else ' - fine, memory was enough.'))
    if results:
        best = {}
        for hours, size, batch, half, mem in results:
            if size not in best or hours < best[size][0]:
                best[size] = (hours, batch, half, mem)
        say('Fastest setting per size:')
        for size, (hours, batch, half, mem) in best.items():
            say(f'  {size}: batch {batch}, {"fp16" if half else "fp32"} - a full run in about {hours:.0f} hours ({hours / 24:.1f} days)')
    open('bench_results.txt', 'w').write('\n'.join(lines) + '\n')
    say('\nSaved to bench_results.txt - send that file back.')


if __name__ == '__main__':
    main()
