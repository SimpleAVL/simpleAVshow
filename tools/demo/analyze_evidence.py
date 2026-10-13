#!/usr/bin/env python3
"""Fail-closed analysis of independently received samples/messages and runner telemetry."""
import argparse
import json
from pathlib import Path
import subprocess
import wave
import numpy as np

FREQ=[211,307,419,557,683,809,947,1103]


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('directory',type=Path)
    ap.add_argument('--project',required=True,type=Path)
    ap.add_argument('--obs',type=Path)
    args=ap.parse_args();root=args.directory
    summary=json.loads((root/'runner-summary.json').read_text())
    software=summary.get('backend','').startswith('software')
    midi_only=summary['scenario']=='midi'
    capture=json.loads((root/'capture-summary.json').read_text()) if (root/'capture-summary.json').exists() else {}
    events=[json.loads(line) for line in (root/'telemetry.jsonl').read_text().splitlines()]
    received=[json.loads(line) for line in (root/'midi-received.jsonl').read_text().splitlines()] if (root/'midi-received.jsonl').exists() else []
    project=json.loads(args.project.read_text())
    checks=[]
    def check(name,ok,**data):
        checks.append(dict(name=name,passed=bool(ok),**data))
        print(('PASS ' if ok else 'FAIL ')+name,flush=True)
    check('native runner passed',summary['passed'])
    origin=next(e for e in events if e['kind']=='origin')['monotonic_seconds']
    def midi_checks():
        expected=[m['message'] for m in sorted(project['markers'],key=lambda m:m['time']) if m['message']]
        actual=[m['bytes'] for m in received]
        check('all MIDI bytes received exactly once in cue order',actual==expected,expected=expected,received=actual)
        for marker in project['markers']:
            if marker['timing']!='resume' or not marker['message']:continue
            matching=next((r for r in received if r['bytes']==marker['message']),None)
            cont=next((e for e in events if e['kind']=='action' and e['name']=='Continue hold' and abs(e['show_seconds']-marker['time']/48000)<.001),None)
            delta=matching['monotonic']-(origin+cont['wall_seconds']) if matching and cont else 1000
            check(f'{marker["id"]} received after Continue only',-.01<=delta<.25,delay_seconds=delta)
    def save_result():
        result=dict(passed=all(c['passed'] for c in checks),scenario=summary['scenario'],
                    backend=summary.get('backend'),hardware_audio_verified=all(c['passed'] for c in checks) and not software and not midi_only,checks=checks)
        (root/'verification.json').write_text(json.dumps(result,indent=2)+'\n')
        return result
    if midi_only:
        midi_checks();result=save_result();raise SystemExit(0 if result['passed'] else 1)
    if not software:
        check('independent input has no dropped blocks',capture['input_overflows']==0 and capture['queue_overflows']==0,statistics=capture)
        check('native input transfer completed',not capture.get('write_failed',False))
        if 'native' in capture:
            check('all native input frames reached the WAV recorder',capture['frames']==capture['native']['frames'],native_frames=capture['native']['frames'],wav_frames=capture['frames'])
    with wave.open(str(root/('software-mix-16ch.wav' if software else 'loopback-16ch.wav')),'rb') as f:
        assert f.getnchannels()==16 and f.getframerate()==48000
        audio=np.frombuffer(f.readframes(f.getnframes()),dtype='<i2').reshape(-1,16).astype(np.float32)/32768
    check('unused channels 9-16 are silent',np.max(np.abs(audio[:,8:]))<.0001,peak=float(np.max(np.abs(audio[:,8:]))))
    if summary['scenario']=='routing':
        rms=np.sqrt(np.mean(audio[:,:8]**2,axis=1))
        active=np.where(rms>.02)[0]
        check('routing signal captured',len(active)>48000)
        if len(active)>48000:
            start=int(active[0]+48000)
            x=audio[start:start+48000,:8]
            spectrum=np.abs(np.fft.rfft(x,axis=0))*2/len(x)
            matrix=spectrum[FREQ,:]
            (root/'frequency-routing-matrix.json').write_text(json.dumps(matrix.tolist(),indent=2)+'\n')
            for ch,freq in enumerate(FREQ):
                wanted=float(matrix[ch,ch]); leakage=float(np.max(np.delete(matrix[ch,:],ch)))
                check(f'{freq} Hz reaches channel {ch+1} only',wanted>.09 and leakage<.0005,
                      wanted_amplitude=wanted,max_wrong_channel_amplitude=leakage)
            # Identical impulse offsets prove alignment across separately routed tracks.
            peaks=np.argmax(np.abs(audio[active[0]+80000:active[0]+120000,:8]),axis=0)
            check('channel line-check impulses aligned',int(np.ptp(peaks))<=2,offsets=peaks.tolist())
    else:
        # Callback/input stream warmup may produce a device-dependent constant latency;
        # timestamp mapping comes from a recorded monotonic origin written by the runner.
        offset=origin-capture.get('audio_start_monotonic',capture.get('start_monotonic',0)) if not software else 0
        def region(start,end):
            if software:
                times=[e['wall_seconds'] for e in events]
                frames=[e['rendered_frames'] for e in events]
                a=max(0,round(float(np.interp(start,times,frames))));b=min(len(audio),round(float(np.interp(end,times,frames))))
            else:
                a=max(0,round((offset+start)*48000));b=min(len(audio),round((offset+end)*48000))
            if b<=a:raise ValueError('empty evidence interval')
            return audio[a:b]
        if summary['scenario'] in ['show','long-hold']:
            arrivals=[e for e in events if e['kind']=='hold_arrival']
            resumes=[e for e in events if e['kind']=='action' and e['name']=='Continue hold']
            for arrival,resume in zip(arrivals,resumes):
                x=region(arrival['wall_seconds']+1.5,resume['wall_seconds']-.3)
                rms=np.sqrt(np.mean(x*x,axis=0))
                check(f'hold {arrival["number"]}: synchronous score/guide/click silent',float(np.max(rms[:4]))<.0002,rms=rms.tolist())
                check(f'hold {arrival["number"]}: async sea audible on 5-6',float(np.min(rms[4:6]))>.015)
                check(f'hold {arrival["number"]}: completed one-shot/effects silent',float(np.max(rms[6:8]))<.0002)
                if summary['scenario']=='long-hold':
                    elapsed=resume['wall_seconds']-arrival['wall_seconds']
                    check('actor hold lasts at least five wall-clock minutes',elapsed>=300,wall_seconds=elapsed)
                    held=[e for e in events if e['kind']=='sample' and arrival['wall_seconds']<=e['wall_seconds']<resume['wall_seconds']]
                    check('every sampled hold state retains the same show time',bool(held) and all(e['state']=='HOLDING' and e['show_seconds']==arrival['show_seconds'] for e in held),samples=len(held))
                    window=48000*5;count=len(x)//window
                    levels=np.sqrt(np.mean(x[:count*window,4:6].reshape(count,window,2)**2,axis=1))
                    check('sea is audible throughout every five-second hold window',count>0 and float(np.min(levels))>.015,windows=count,minimum_rms=float(np.min(levels)))
            if summary['scenario']=='show':
                midi_checks()
                pauses=[e for e in events if e['kind']=='action' and e['name']=='Manual pause']
                resumes=[e for e in events if e['kind']=='action' and e['name']=='Resume manual pause']
                for a,b in zip(pauses,resumes):
                    x=region(a['wall_seconds']+.5,b['wall_seconds']-.3)
                    rms=np.sqrt(np.mean(x*x,axis=0))
                    check('manual pause: async audible and sync silent',float(np.max(rms[:4]))<.0002 and float(np.min(rms[4:6]))>.015,rms=rms.tolist())
        elif summary['scenario']=='async':
            actions=[e for e in events if e['kind']=='action']
            def at(name):return next(e['wall_seconds'] for e in actions if e['name']==name)
            a=at('Pause with async loop and one-shot');b=at('Seek inside async interval');c=at('Seek exactly to async clip end')
            for name,start,end in [('pause',a+1.2,b-.2),('inside seek',b+.3,c-.2)]:
                x=region(start,end);rms=np.sqrt(np.mean(x*x,axis=0))
                check(f'{name}: sea continues, click/finished bell silent',float(np.min(rms[4:6]))>.015 and float(np.max(rms[[3,6,7]]))<.0002,rms=rms.tolist())
            x=region(c+.3,at('Play from inside async clip')-.2)
            check('exact-end and paused re-entry remain silent',float(np.max(np.abs(x)))<.0002)
            check('seeks emit no MIDI',not received,received=received)
        stop=next(e for e in reversed(events) if e['kind']=='action' and e['name']=='Stop All')
        x=region(stop['wall_seconds']+.15,stop['wall_seconds']+.4)
        check('Stop All captured as silence on every channel',float(np.max(np.abs(x)))<.0002)
    if args.obs:
        probe=json.loads(subprocess.check_output(['ffprobe','-v','error','-show_streams','-show_format','-of','json',str(args.obs)]))
        streams=probe['streams'];a=[s for s in streams if s['codec_type']=='audio'];v=[s for s in streams if s['codec_type']=='video']
        check('OBS recording contains video and six independent audio tracks',len(v)==1 and len(a)==6,
              streams=[dict(index=s['index'],codec=s['codec_name'],type=s['codec_type'],channels=s.get('channels')) for s in streams])
        (root/'obs-probe.json').write_text(json.dumps(probe,indent=2)+'\n')
        frames=[]
        duration=float(probe['format']['duration'])
        for t in [duration*.15,duration*.45,duration*.8]:
            raw=subprocess.check_output(['ffmpeg','-v','error','-ss',str(t),'-i',str(args.obs),'-map','0:v:0','-frames:v','1','-vf','scale=160:90','-pix_fmt','rgb24','-f','rawvideo','-'])
            frames.append(np.frombuffer(raw,dtype='uint8').astype(float))
        usable=all(len(f)==160*90*3 for f in frames)
        difference=float(np.mean(np.abs(frames[0]-frames[-1]))) if usable else 0
        check('OBS video contains visible changing native editor frames',usable and all(np.std(f)>15 for f in frames) and difference>2,
              frame_standard_deviations=[float(np.std(f)) if len(f) else 0 for f in frames],first_last_difference=difference)
        if 'native' in capture and usable:
            panels=[f.reshape(90,160,3)[:,120:,:] for f in frames]
            panel_difference=float(np.mean(np.abs(panels[0]-panels[-1])))
            check('OBS MIDI logger and live input meters are visible and changing',
                  all(np.std(p)>15 for p in panels) and panel_difference>.5,
                  frame_standard_deviations=[float(np.std(p)) for p in panels],first_last_difference=panel_difference)
        decoded=[]
        for i in range(len(a)):
            pcm=subprocess.check_output(['ffmpeg','-v','error','-i',str(args.obs),'-map',f'0:a:{i}',
                '-f','f32le','-ac','1','-ar','48000','-'])
            x=np.frombuffer(pcm,dtype='<f4')
            decoded.append(x)
            check(f'OBS audio track {i+1} contains live signal',len(x)>48000 and float(np.max(np.abs(x)))>.01,
                  peak=float(np.max(np.abs(x))) if len(x) else 0)
        if len(decoded)==6:
            count=min(len(x) for x in decoded)
            isolated=np.stack([x[:count:240] for x in decoded[1:]])
            correlations=np.abs(np.corrcoef(isolated))
            np.fill_diagonal(correlations,0)
            check('OBS isolated tracks are distinct signals, not duplicate mixes',np.isfinite(correlations).all() and float(np.max(correlations))<.95,
                  pairwise_absolute_correlations=correlations.tolist())
    result=save_result()
    raise SystemExit(0 if result['passed'] else 1)


if __name__=='__main__':main()
