# Trains the drum split model. It saves its progress every 10 minutes and when you press Ctrl+C; run
# the same command again to carry on where it stopped. Every 1000 updates it checks itself on a fixed
# set of examples and keeps the best version so far (best.pt), which export.py turns into the model
# KITNAP runs.
#
# Usage: python train.py [--config configs/large.yaml] [--run runs/large] [--batch 2] [--accum 2]
#                        [--examples 300000] [--workers 3]
import argparse, copy, csv, math, os, signal, time
os.environ.setdefault('PYTORCH_ENABLE_MPS_FALLBACK', '1')
# Keep the GPU to 70% of the memory macOS recommends (see bench.py), so the Mac never has to swap.
os.environ.setdefault('PYTORCH_MPS_HIGH_WATERMARK_RATIO', '0.7')
os.environ.setdefault('PYTORCH_MPS_LOW_WATERMARK_RATIO', '0.5')
import numpy as np, torch
import model as M
import generator as G

PARTS = G.CLASSES


def quiet_worker(_):
    """Ctrl+C is for the main process, which saves before stopping; the example makers ignore it."""
    signal.signal(signal.SIGINT, signal.SIG_IGN)


class Stream(torch.utils.data.IterableDataset):
    """Training examples from the generator; every worker draws its own."""

    def __init__(self, data, seconds, seed):
        self.data, self.seconds, self.seed = data, seconds, seed

    def __iter__(self):
        info = torch.utils.data.get_worker_info()
        for mix, parts in G.Examples(self.data, self.seconds, seed=self.seed, stream=info.id if info else 0):
            yield torch.from_numpy(mix), torch.from_numpy(parts)


def part_snr(predicted, target_spec):
    """How close each predicted part is to the true one, in dB (higher is better), summed over the
    batch: the energy of the true part over the energy of the error."""
    err = ((predicted - target_spec) ** 2).sum(dim=(0, 2, 3, 4))
    ref = (target_spec ** 2).sum(dim=(0, 2, 3, 4))
    return 10 * torch.log10((ref + 1e-8) / (err + 1e-8))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--config', default='configs/large.yaml')
    ap.add_argument('--data', default='data')
    ap.add_argument('--run', default=None, help='folder for checkpoints and logs (default: runs/<config name>)')
    ap.add_argument('--batch', type=int, default=2)
    ap.add_argument('--accum', type=int, default=2, help='batches added up per update')
    ap.add_argument('--examples', type=int, default=300_000, help='examples to train on in total')
    ap.add_argument('--lr', type=float, default=3e-4)
    ap.add_argument('--workers', type=int, default=3)
    ap.add_argument('--fp32', action='store_true', help='train in full precision (slower)')
    ap.add_argument('--seed', type=int, default=0)
    args = ap.parse_args()

    device = torch.device('mps' if torch.backends.mps.is_available() else 'cuda' if torch.cuda.is_available() else 'cpu')
    run = args.run or os.path.join('runs', os.path.splitext(os.path.basename(args.config))[0])
    os.makedirs(run, exist_ok=True)
    cfg = M.load_config(args.config)
    dim_f, dim_t = cfg['audio']['dim_f'], cfg['audio']['dim_t']
    seconds = M.frames_to_samples(dim_t) / G.RATE

    net = M.Net(cfg).to(device)
    ema = copy.deepcopy(net).eval()
    for p in ema.parameters():
        p.requires_grad_(False)
    opt = torch.optim.AdamW(net.parameters(), lr=args.lr, weight_decay=0.01)
    half = device.type != 'cpu' and not args.fp32
    scaler = torch.amp.GradScaler(device.type, enabled=half)
    updates = math.ceil(args.examples / (args.batch * args.accum))
    step, best = 0, -1e9

    last = os.path.join(run, 'last.pt')
    if os.path.exists(last):
        state = torch.load(last, map_location='cpu', weights_only=True)
        net.load_state_dict(state['model']); ema.load_state_dict(state['ema'])
        opt.load_state_dict(state['opt']); scaler.load_state_dict(state['scaler'])
        step, best = state['step'], state['best']
        print(f'Carrying on from update {step} of {updates}.')
    else:
        print(f'Starting: {sum(p.numel() for p in net.parameters()) / 1e6:.1f}M parameters, {updates} updates '
              f'of {args.batch * args.accum} examples, on {device}{" in fp16" if half else ""}.')

    def save(path, extra=None):
        state = {'model': net.state_dict(), 'ema': ema.state_dict(), 'opt': opt.state_dict(),
                 'scaler': scaler.state_dict(), 'step': step, 'best': best, 'config': args.config}
        torch.save(state | (extra or {}), path + '.tmp')
        os.replace(path + '.tmp', path)               # never a half-written checkpoint

    # A fixed set of examples to check progress on, the same every time.
    validation = [next(e) for e in [iter(G.Examples(args.data, seconds, seed=987654))] for _ in range(48)]

    def validate():
        totals, loss = torch.zeros(len(PARTS)), 0.0
        with torch.no_grad():
            for i in range(0, len(validation), 4):
                mix = torch.from_numpy(np.stack([v[0] for v in validation[i:i + 4]])).to(device)
                parts = torch.from_numpy(np.stack([v[1] for v in validation[i:i + 4]])).to(device)
                spec = M.spectrogram(mix, dim_f)
                with torch.autocast(device.type, dtype=torch.float16, enabled=half):
                    out = ema(spec)
                out = out.float()
                loss += float(M.loss_fn(out, spec, parts, dim_f))
                b, p, c, n = parts.shape
                target = M.spectrogram(parts.reshape(b * p, c, n), dim_f).reshape(out.shape)
                totals += part_snr(out, target).cpu()
        return loss / (len(validation) / 4), totals / (len(validation) / 4)

    # Every start draws new examples, so a resumed run doesn't repeat the ones it already saw.
    loader = torch.utils.data.DataLoader(Stream(args.data, seconds, seed=args.seed * 1_000_003 + step),
                                         batch_size=args.batch, num_workers=args.workers, worker_init_fn=quiet_worker,
                                         persistent_workers=args.workers > 0, prefetch_factor=4 if args.workers else None)
    stop = []
    signal.signal(signal.SIGINT, lambda *_: stop.append(True))
    log = open(os.path.join(run, 'log.csv'), 'a', newline='')
    writer = csv.writer(log)
    if log.tell() == 0:
        writer.writerow(['update', 'examples', 'loss', 'val_loss'] + [f'{p}_db' for p in PARTS])

    def lr_at(u):
        warm = min(1.0, (u + 1) / 500)
        return args.lr * warm * (0.05 + 0.95 * 0.5 * (1 + math.cos(math.pi * min(1.0, u / updates))))

    batches = iter(loader)
    started, saved_at, running = time.time(), time.time(), []
    while step < updates and not stop:
        for g in opt.param_groups:
            g['lr'] = lr_at(step)
        for _ in range(args.accum):
            mix, parts = next(batches)
            mix, parts = mix.to(device), parts.to(device)
            spec = M.spectrogram(mix, dim_f)
            with torch.autocast(device.type, dtype=torch.float16, enabled=half):
                out = net(spec)
            loss = M.loss_fn(out.float(), spec, parts, dim_f) / args.accum
            scaler.scale(loss).backward()
            running.append(loss.item() * args.accum)
        scaler.unscale_(opt)
        torch.nn.utils.clip_grad_norm_(net.parameters(), 5.0)
        scaler.step(opt)
        scaler.update()
        opt.zero_grad(set_to_none=True)
        with torch.no_grad():                         # the averaged weights, which give the cleaner model
            for e, p in zip(ema.parameters(), net.parameters()):
                e.lerp_(p, 0.001)
            for e, b in zip(ema.buffers(), net.buffers()):
                e.copy_(b)
        step += 1

        if step % 50 == 0:
            rate = 50 * args.batch * args.accum / (time.time() - started)
            left = (updates - step) * args.batch * args.accum / rate / 3600
            print(f'update {step}/{updates}  loss {np.mean(running):.4f}  {rate:.1f} examples/s  '
                  f'about {left:.1f} h to go', flush=True)
            started, running = time.time(), []
        if step % 1000 == 0 or step == updates:
            val_loss, part_db = validate()
            print('  check: loss %.4f, ' % val_loss + ', '.join(f'{p} {d:.1f} dB' for p, d in zip(PARTS, part_db)), flush=True)
            writer.writerow([step, step * args.batch * args.accum, '', f'{val_loss:.5f}'] + [f'{d:.2f}' for d in part_db])
            log.flush()
            score = float(part_db.mean())
            if score > best:
                best = score
                torch.save({'ema': ema.state_dict(), 'step': step, 'config': args.config}, os.path.join(run, 'best.pt'))
                print('  best so far, saved.', flush=True)
        if time.time() - saved_at > 600:
            save(last)
            saved_at = time.time()

    save(last)
    print(('Stopped' if stop else 'Finished') + f' at update {step} of {updates}; progress saved in {run}.')


if __name__ == '__main__':
    main()
