#!/usr/bin/env python3
"""Keep the independent MIDI logger / BlackHole meters open for a manual rehearsal."""
import argparse
from datetime import datetime
import os
from pathlib import Path
import signal
import subprocess
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, help='Fresh directory for received MIDI and audio')
    parser.add_argument('--device', default='BlackHole 16ch')
    parser.add_argument('--obs-relay', action='store_true', help='Also feed the five existing OBS stem inputs')
    args = parser.parse_args()
    if sys.platform != 'darwin':
        parser.error('The visible native loopback app requires macOS')
    app = ROOT / 'build/lighthouse_loopback.app'
    if not app.exists():
        parser.error('Build first: cmake -S . -B build -DSAV_BUILD_DEMO_VALIDATION=ON && cmake --build build -j 6')
    output = (args.output or ROOT / 'artifacts/last-lighthouse/manual-loopback' /
              datetime.now().strftime('%Y%m%d-%H%M%S-%f')).resolve()
    output.mkdir(parents=True, exist_ok=False)
    fifo = output / 'native-input.f32'
    os.mkfifo(fifo, 0o600)
    stopping = threading.Event()
    signal.signal(signal.SIGINT, lambda *_: stopping.set())
    signal.signal(signal.SIGTERM, lambda *_: stopping.set())
    command = [sys.executable, str(ROOT / 'tools/demo/capture_loopback.py'),
               '--output', str(output), '--native-pipe', str(fifo), '--device', args.device]
    if args.obs_relay:
        command.append('--obs-relay')
    launched = False
    with (output / 'capture-process.log').open('w') as log:
        # Ctrl+C must stop the native producer before its FIFO reader; isolate
        # the receiver from the launcher's foreground terminal signal group.
        receiver = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT,
                                    start_new_session=True)
        try:
            deadline = time.monotonic() + 20
            while not (output / 'capture-ready.json').exists():
                if receiver.poll() is not None:
                    raise RuntimeError(f'MIDI receiver failed; see {output / "capture-process.log"}')
                if stopping.wait(.05):
                    return
                if time.monotonic() > deadline:
                    raise RuntimeError('MIDI receiver readiness timeout')
            subprocess.run(['open', '-n', '-a', str(app), '--args', '--output', str(output),
                            '--pipe', str(fifo), '--device', args.device], check=True)
            launched = True
            print('Select "Lighthouse MIDI Logger" as MIDI output in SimpleAVShow.', flush=True)
            print('Select BlackHole 16ch as audio output to see the input meters.', flush=True)
            print(f'Logs and 16-channel audio: {output}', flush=True)
            print('Close the recorder window or press Ctrl+C here to finish.', flush=True)
            deadline = time.monotonic() + 300
            while not (output / 'native-capture-ready.json').exists():
                error = output / 'native-capture-error.json'
                if error.exists():
                    raise RuntimeError(error.read_text())
                if (output / 'native-capture-summary.json').exists():
                    return
                if receiver.poll() is not None:
                    raise RuntimeError('The MIDI receiver stopped unexpectedly')
                if stopping.wait(.1):
                    return
                if time.monotonic() > deadline:
                    raise RuntimeError('Native input readiness timeout; allow its microphone permission')
            print('READY: live MIDI logger and BlackHole input meters.', flush=True)
            while not stopping.wait(.2):
                if (output / 'native-capture-summary.json').exists():
                    break
                if receiver.poll() is not None:
                    raise RuntimeError('The MIDI/audio receiver stopped unexpectedly')
        finally:
            # Drain the native FIFO before stopping its Python reader/writer.
            if launched:
                (output / 'stop-native-capture').touch()
                deadline = time.monotonic() + 10
                while not (output / 'native-capture-summary.json').exists() and time.monotonic() < deadline:
                    time.sleep(.05)
            (output / 'stop-capture').touch()
            try:
                receiver.wait(timeout=25)
            except subprocess.TimeoutExpired:
                receiver.terminate()
                try:
                    receiver.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    receiver.kill()
                    receiver.wait()
            fifo.unlink(missing_ok=True)
    if receiver.returncode:
        raise RuntimeError(f'Receiver did not finish cleanly; see {output / "capture-process.log"}')


if __name__ == '__main__':
    main()
