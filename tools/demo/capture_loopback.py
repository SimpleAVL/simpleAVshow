#!/usr/bin/env python3
"""Independent CoreAudio input + native CoreMIDI receiver, optionally relayed to OBS.

Receives only. Never sends MIDI or renders show audio. 16-channel PCM WAV is the
primary routing evidence; OBS relays are derived from these live input samples.
"""
import argparse
import json
from pathlib import Path
import queue
import signal
import subprocess
import threading
import time
import wave

import numpy as np
import rtmidi
import sounddevice as sd


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--output',required=True,type=Path)
    ap.add_argument('--device',default='BlackHole 16ch')
    ap.add_argument('--obs-relay',action='store_true')
    ap.add_argument('--midi-only',action='store_true')
    ap.add_argument('--software-wav',type=Path,help='Relay a growing mixer WAV; explicitly NOT CoreAudio loopback')
    ap.add_argument('--native-pipe',type=Path,help='Receive unmodified float32 samples from the independent CoreAudio app')
    args=ap.parse_args()
    args.output.mkdir(parents=True,exist_ok=True)
    begin=time.monotonic()
    log=(args.output/'midi-received.jsonl').open('w',buffering=1)
    midi=rtmidi.MidiIn(name='Lighthouse Independent Receiver')
    midi.ignore_types(sysex=False,timing=False,active_sense=False)
    midi.open_virtual_port('Lighthouse MIDI Logger')
    def receive(event, _):
        message, delta=event
        log.write(json.dumps(dict(wall_seconds=time.monotonic()-begin,monotonic=time.monotonic(),
            bytes=message,hex=' '.join(f'{b:02X}' for b in message),delta=delta))+'\n')
    midi.set_callback(receive)
    stopping=threading.Event()
    signal.signal(signal.SIGINT,lambda *_:stopping.set())
    signal.signal(signal.SIGTERM,lambda *_:stopping.set())
    block_queue=queue.Queue(maxsize=3000)
    stats=dict(input_overflows=0,queue_overflows=0,frames=0,relay_errors=[],start_monotonic=begin)
    processes=[]
    # Track 1 full mix, 2 main, 3 guide, 4 click, 5 sea, 6 effects in OBS.
    stems=[('main',[0,1]),('guide',[2,2]),('click',[3,3]),('sea',[4,5]),('effects',[6,7])]
    if args.obs_relay:
        for i,(name,_) in enumerate(stems):
            err=(args.output/f'relay-{name}.log').open('w')
            processes.append(subprocess.Popen(['ffmpeg','-v','warning','-use_wallclock_as_timestamps','1','-f','f32le','-ar','48000',
                '-ac','2','-i','pipe:0','-af','aresample=async=1000:min_hard_comp=0.1:first_pts=0','-c:a','aac','-b:a','192k','-f','mpegts',
                f'udp://127.0.0.1:{18100+i}?pkt_size=1316'],stdin=subprocess.PIPE,stderr=err))
    def write_audio():
        # Uncompressed 16-bit loopback capture, with no channel remapping or mixing.
        with wave.open(str(args.output/('software-relay-16ch.wav' if args.software_wav else 'loopback-16ch.wav')),'wb') as f:
            f.setnchannels(16);f.setsampwidth(2);f.setframerate(48000)
            while True:
                block=block_queue.get()
                if block is None: break
                stats['frames']+=len(block)
                f.writeframesraw((np.clip(block,-1,1)*32767).astype('<i2').tobytes())
                for process,(_,channels) in zip(processes,stems):
                    try: process.stdin.write(block[:,channels].astype('<f4').tobytes())
                    except (BrokenPipeError,OSError) as e:
                        if str(e) not in stats['relay_errors']:stats['relay_errors'].append(str(e))
        for process in processes:
            try: process.stdin.close();process.wait(timeout=10)
            except (OSError,subprocess.TimeoutExpired):process.terminate()
    def audio_callback(indata,frames,timing,status):
        if 'audio_start_monotonic' not in stats:
            stats['audio_start_monotonic']=time.monotonic()-frames/48000
        if status:stats['input_overflows']+=1
        try:block_queue.put_nowait(indata.copy())
        except queue.Full:stats['queue_overflows']+=1
    writer=None; stream=None
    tail=None
    if args.native_pipe:
        stats['backend']='independent native CoreAudio app through float32 pipe'
        writer=threading.Thread(target=write_audio);writer.start()
        def follow_native():
            with args.native_pipe.open('rb',buffering=0) as f:
                pending=b''
                while not stopping.is_set():
                    chunk=f.read(512*16*4-len(pending))
                    if not chunk:break
                    pending+=chunk
                    if len(pending)==512*16*4:
                        block_queue.put(np.frombuffer(pending,dtype='<f4').reshape(-1,16).copy());pending=b''
        tail=threading.Thread(target=follow_native,daemon=True);tail.start()
    elif args.software_wav:
        stats['backend']='software WAV relay; no audio device input'
        writer=threading.Thread(target=write_audio);writer.start()
        def follow_file():
            while not args.software_wav.exists() and not stopping.wait(.05):pass
            if stopping.is_set():return
            with args.software_wav.open('rb') as f:
                f.seek(44)
                pending=b''
                while not stopping.is_set():
                    pending+=f.read(512*16*2-len(pending))
                    if len(pending)==512*16*2:
                        block_queue.put(np.frombuffer(pending,dtype='<i2').reshape(-1,16).astype('float32')/32768)
                        pending=b''
                    else:stopping.wait(.005)
        tail=threading.Thread(target=follow_file);tail.start()
    elif not args.midi_only:
        devices=sd.query_devices()
        candidates=[i for i,d in enumerate(devices) if d['name']==args.device and d['max_input_channels']>=16]
        if len(candidates)!=1:raise RuntimeError(f'16-channel input unavailable: {devices}')
        stream=sd.InputStream(device=candidates[0],channels=16,samplerate=48000,blocksize=512,
                              dtype='float32',callback=audio_callback)
        writer=threading.Thread(target=write_audio);writer.start();stream.start()
        stats['input_device']=dict(devices[candidates[0]])
    (args.output/'capture-ready.json').write_text(json.dumps(stats,indent=2)+'\n')
    print('READY: independent MIDI receiver'+(' and 16-channel CoreAudio capture' if stream else ''),flush=True)
    try:
        while not stopping.wait(.2):
            if (args.output/'stop-capture').exists():break
    finally:
        stopping.set()
        if stream:stream.stop();stream.close()
        if tail:tail.join(timeout=5)
        if writer:block_queue.put(None);writer.join(timeout=20)
        midi.cancel_callback();midi.close_port();log.close()
        stats['duration_seconds']=stats['frames']/48000
        stats['wall_seconds']=time.monotonic()-begin
        if args.native_pipe:
            native_path=args.output/'native-capture-summary.json'
            if not native_path.exists():
                stats['write_failed']=True
                (args.output/'capture-summary.json').write_text(json.dumps(stats,indent=2)+'\n')
                raise RuntimeError('The native input collector did not finish; inspect its permission/error status')
            native=json.loads(native_path.read_text())
            stats['native']=native
            stats['input_device']=native['input_device']
            stats['audio_start_monotonic']=native['audio_start_monotonic']
            stats['input_overflows']+=native['input_overflows']
            stats['queue_overflows']+=native['queue_overflows']
            stats['write_failed']=native['write_failed']
        (args.output/'capture-summary.json').write_text(json.dumps(stats,indent=2)+'\n')


if __name__=='__main__':main()
