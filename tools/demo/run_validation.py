#!/usr/bin/env python3
"""Run three rehearsals, capture independent MIDI and audio, analyze, and optionally record OBS.

The default requires BlackHole 16ch. --software deliberately labels the weaker
software-mixer evidence and does not claim hardware routing or screen capture.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import time
import os

from obs_control import OBS

ROOT=Path(__file__).resolve().parents[2]


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--output',type=Path,default=ROOT/'artifacts/last-lighthouse/live-validation')
    ap.add_argument('--device',default='BlackHole 16ch')
    ap.add_argument('--software',action='store_true')
    ap.add_argument('--obs',action='store_true')
    ap.add_argument('--long-hold',action='store_true',help='Also run an actual five-minute stage hold')
    ap.add_argument('--buffer',type=int,default=512)
    ap.add_argument('--scenario',choices=['routing','async','show','long-hold'],help='Run one scenario only')
    ap.add_argument('--python-input',action='store_true',help='Use direct Python input instead of the macOS collector app (requires host audio permission)')
    args=ap.parse_args();args.output=args.output.resolve()
    runner=ROOT/'build/lighthouse_validation'
    demos=ROOT/'demos/last-lighthouse'
    if not args.software:
        inventory=json.loads(subprocess.check_output([str(runner),'--list-devices']))
        if not any(d['name']==args.device or d['name'].endswith(': '+args.device) for d in inventory['outputs']):
            raise RuntimeError(f'Required loopback output {args.device} is unavailable. Install it first. --software is a separate, explicitly weaker proof.')
    projects={'routing':'routing-check.savshow','async':'async-check.savshow',
              'show':'last-lighthouse.savshow','long-hold':'last-lighthouse.savshow'}
    scenarios=[(name,projects[name]) for name in ['routing','async','show']]
    if args.long_hold:scenarios.append(('long-hold','last-lighthouse.savshow'))
    if args.scenario:scenarios=[(args.scenario,projects[args.scenario])]
    args.output.mkdir(parents=True,exist_ok=True)
    results=[]
    for scenario,project in scenarios:
        out=args.output/scenario
        if out.exists():raise RuntimeError(f'Refusing to overwrite existing evidence: {out}; choose a fresh --output directory')
        out.mkdir()
        capture_log=(out/'capture-process.log').open('w')
        command=[sys.executable,str(ROOT/'tools/demo/capture_loopback.py'),'--output',str(out),'--device',args.device]
        record_obs=args.obs and scenario=='show'
        native=not args.software and sys.platform=='darwin' and not args.python_input
        if native:
            fifo=out/'native-input.f32';os.mkfifo(fifo,0o600);command+=['--native-pipe',str(fifo)]
        if args.software:
            command+=['--software-wav',str(out/'software-mix-16ch.wav')] if record_obs else ['--midi-only']
        if record_obs:command+=['--obs-relay']
        capture=subprocess.Popen(command,stdout=capture_log,stderr=subprocess.STDOUT)
        obs=None;recording=None;playback=None;obs_started=False
        def stop_native():
            if not native:return
            (out/'stop-native-capture').touch()
            deadline=time.monotonic()+10
            while not (out/'native-capture-summary.json').exists() and time.monotonic()<deadline:time.sleep(.05)
        try:
            deadline=time.monotonic()+20
            while not (out/'capture-ready.json').exists():
                if capture.poll() is not None:raise RuntimeError('Recorder failed; see capture-process.log')
                if time.monotonic()>deadline:raise RuntimeError('Recorder readiness timeout')
                time.sleep(.05)
            if native:
                subprocess.run(['open','-n','-a',str(ROOT/'build/lighthouse_loopback.app'),'--args','--output',str(out),'--pipe',str(fifo),'--device',args.device],check=True)
                deadline=time.monotonic()+300
                while not (out/'native-capture-ready.json').exists():
                    if (out/'native-capture-error.json').exists():raise RuntimeError((out/'native-capture-error.json').read_text())
                    if time.monotonic()>deadline:raise RuntimeError('Native collector readiness timeout; allow its microphone permission')
                    time.sleep(.05)
            if record_obs:
                obs=OBS();setup=obs.setup(out)
                (out/'obs-setup.json').write_text(json.dumps(setup,indent=2)+'\n')
                if args.software:
                    # OBS's image source reloads changed files once per second.
                    # These are actual Qt widget snapshots, not OS screen capture.
                    existing={i['inputName'] for i in obs.call('GetInputList')['inputs']}
                    settings=dict(file=str(out/'live-editor.jpg'),unload=False)
                    name='Native editor snapshots'
                    if name not in existing:obs.call('CreateInput',sceneName='Live rehearsal',inputName=name,inputKind='image_source',inputSettings=settings,sceneItemEnabled=True)
                    else:obs.call('SetInputSettings',inputName=name,inputSettings=settings,overlay=True)
                    item=obs.call('GetSceneItemId',sceneName='Live rehearsal',sourceName=name)['sceneItemId']
                    obs.call('SetSceneItemEnabled',sceneName='Live rehearsal',sceneItemId=item,sceneItemEnabled=True)
                    obs.call('SetSceneItemTransform',sceneName='Live rehearsal',sceneItemId=item,sceneItemTransform=dict(boundsType='OBS_BOUNDS_SCALE_INNER',boundsWidth=1920,boundsHeight=1020,positionX=0,positionY=0))
                    label='Software evidence label'
                    settings=dict(text='NATIVE WIDGET SNAPSHOTS (~1 fps)  |  SOFTWARE MIXER  |  REAL CoreMIDI LOOPBACK  |  Hardware audio unverified',font=dict(face='Arial',size=24))
                    if label not in existing:obs.call('CreateInput',sceneName='Live rehearsal',inputName=label,inputKind='text_ft2_source_v2',inputSettings=settings,sceneItemEnabled=True)
                    else:obs.call('SetInputSettings',inputName=label,inputSettings=settings,overlay=True)
                    item=obs.call('GetSceneItemId',sceneName='Live rehearsal',sourceName=label)['sceneItemId']
                    obs.call('SetSceneItemEnabled',sceneName='Live rehearsal',sceneItemId=item,sceneItemEnabled=True)
                    obs.call('SetSceneItemTransform',sceneName='Live rehearsal',sceneItemId=item,sceneItemTransform=dict(positionX=35,positionY=1030))
                    for old in ['Native rehearsal screen','Native editor widget render','MIDI logger and BlackHole meters','Hardware evidence label']:
                        if old in existing:
                            item=obs.call('GetSceneItemId',sceneName='Live rehearsal',sourceName=old)['sceneItemId']
                            obs.call('SetSceneItemEnabled',sceneName='Live rehearsal',sceneItemId=item,sceneItemEnabled=False)
                else:
                    existing={i['inputName'] for i in obs.call('GetInputList')['inputs']}
                    # Recreate sources to discard ScreenCaptureKit's cached list
                    # of windows from the preceding collector/rehearsal process.
                    fresh=['Native rehearsal screen','MIDI logger and BlackHole meters']
                    for name in fresh:
                        if name in existing:obs.call('RemoveInput',inputName=name)
                    deadline=time.monotonic()+10
                    while any(i['inputName'] in fresh for i in obs.call('GetInputList')['inputs']):
                        if time.monotonic()>deadline:raise RuntimeError('OBS did not release preceding window sources')
                        time.sleep(.05)
                    existing-=set(fresh)
                    obs.call('CreateInput',sceneName='Live rehearsal',inputName='Native rehearsal screen',inputKind='screen_capture',inputSettings=dict(type=1,show_cursor=False),sceneItemEnabled=True)
                    for old in ['Native editor widget render','Native editor snapshots','Software evidence label']:
                        if old in existing:
                            item=obs.call('GetSceneItemId',sceneName='Live rehearsal',sourceName=old)['sceneItemId']
                            obs.call('SetSceneItemEnabled',sceneName='Live rehearsal',sceneItemId=item,sceneItemEnabled=False)
            runner_log=(out/'runner-process.log').open('w')
            command=[str(runner),'--project',str(demos/project),'--output',str(out),'--scenario',scenario,'--device',args.device,'--buffer',str(args.buffer)]
            if args.software:command+=['--software']
            playback=subprocess.Popen(command,stdout=runner_log,stderr=subprocess.STDOUT)
            if record_obs:
                deadline=time.monotonic()+10
                while not (out/'loaded-editor.png').exists():
                    if playback.poll() is not None:raise RuntimeError('Runner setup failed; see runner-process.log')
                    if time.monotonic()>deadline:raise RuntimeError('Runner readiness timeout')
                    time.sleep(.05)
                if not args.software:
                    windows=obs.call('GetInputPropertiesListPropertyItems',inputName='Native rehearsal screen',propertyName='window')['propertyItems']
                    choices=[i for i in windows if 'Lighthouse' in i['itemName'] and 'SimpleAVShow' in i['itemName']]
                    if len(choices)!=1:raise RuntimeError('OBS cannot uniquely capture the native rehearsal window; enable macOS screen capture permission')
                    obs.call('SetInputSettings',inputName='Native rehearsal screen',inputSettings=dict(type=1,window=choices[0]['itemValue']),overlay=True)
                    # ScreenCaptureKit also captures application audio. Only the
                    # independently received BlackHole stems belong in this mix.
                    obs.call('SetInputMute',inputName='Native rehearsal screen',inputMuted=True)
                    obs.call('SetInputAudioTracks',inputName='Native rehearsal screen',inputAudioTracks={str(t):False for t in range(1,7)})
                    item=obs.call('GetSceneItemId',sceneName='Live rehearsal',sourceName='Native rehearsal screen')['sceneItemId']
                    obs.call('SetSceneItemEnabled',sceneName='Live rehearsal',sceneItemId=item,sceneItemEnabled=True)
                    obs.call('SetSceneItemTransform',sceneName='Live rehearsal',sceneItemId=item,sceneItemTransform=dict(boundsType='OBS_BOUNDS_SCALE_INNER',boundsWidth=1440 if native else 1920,boundsHeight=1020,positionX=0,positionY=0))
                    if native:
                        choices=[i for i in windows if 'Lighthouse Loopback Recorder' in i['itemName']]
                        if len(choices)!=1:raise RuntimeError('OBS cannot uniquely capture the live MIDI/input-meter window')
                        name='MIDI logger and BlackHole meters'
                        settings=dict(type=1,window=choices[0]['itemValue'],show_cursor=False)
                        if name not in existing:obs.call('CreateInput',sceneName='Live rehearsal',inputName=name,inputKind='screen_capture',inputSettings=settings,sceneItemEnabled=True)
                        else:obs.call('SetInputSettings',inputName=name,inputSettings=settings,overlay=True)
                        obs.call('SetInputMute',inputName=name,inputMuted=True)
                        obs.call('SetInputAudioTracks',inputName=name,inputAudioTracks={str(t):False for t in range(1,7)})
                        item=obs.call('GetSceneItemId',sceneName='Live rehearsal',sourceName=name)['sceneItemId']
                        obs.call('SetSceneItemEnabled',sceneName='Live rehearsal',sceneItemId=item,sceneItemEnabled=True)
                        obs.call('SetSceneItemTransform',sceneName='Live rehearsal',sceneItemId=item,sceneItemTransform=dict(boundsType='OBS_BOUNDS_SCALE_INNER',boundsWidth=480,boundsHeight=1020,positionX=1440,positionY=0))
                    deadline=time.monotonic()+10
                    names=['Native rehearsal screen']+(['MIDI logger and BlackHole meters'] if native else [])
                    while True:
                        sizes=[obs.call('GetSceneItemTransform',sceneName='Live rehearsal',sceneItemId=obs.call('GetSceneItemId',sceneName='Live rehearsal',sourceName=n)['sceneItemId'])['sceneItemTransform']['sourceWidth'] for n in names]
                        if all(s>0 for s in sizes):break
                        if time.monotonic()>deadline:raise RuntimeError('OBS window capture failed to produce both video sources')
                        time.sleep(.1)
                    obs.call('SaveSourceScreenshot',sourceName='Live rehearsal',imageFormat='png',imageFilePath=str(out/'combined-obs-preview.png'))
                obs.call('StartRecord');obs_started=True
            result=playback.wait(timeout=390 if scenario=='long-hold' else 150)
            time.sleep(1)
            if obs_started:
                recording=Path(obs.call('StopRecord')['outputPath']);obs_started=False
                (out/'obs-recording.json').write_text(json.dumps(dict(path=str(recording)),indent=2)+'\n')
                # StopRecord may precede the asynchronous final muxer/file flush.
                previous=-1;stable=0;deadline=time.monotonic()+15
                while stable<4 and time.monotonic()<deadline:
                    size=recording.stat().st_size if recording.exists() else 0
                    stable=stable+1 if size==previous and size>0 else 0
                    previous=size;time.sleep(.5)
            stop_native();(out/'stop-capture').touch();capture.wait(timeout=25)
            analyze=[sys.executable,str(ROOT/'tools/demo/analyze_evidence.py'),str(out),'--project',str(demos/project)]
            if recording:analyze+=['--obs',str(recording)]
            analyzed=subprocess.run(analyze).returncode
            results.append(dict(scenario=scenario,runner_exit=result,analysis_exit=analyzed))
        finally:
            if obs_started:
                try:obs.call('StopRecord')
                except Exception:pass
            if obs:obs.ws.close()
            if playback and playback.poll() is None:playback.terminate();playback.wait(timeout=10)
            stop_native()
            (out/'stop-capture').touch()
            if capture.poll() is None:capture.wait(timeout=25)
            capture_log.close()
    report=dict(passed=all(r['runner_exit']==0 and r['analysis_exit']==0 for r in results),
        backend='software mixer; hardware audio unverified' if args.software else 'BlackHole CoreAudio loopback',results=results)
    (args.output/'validation-summary.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2));raise SystemExit(0 if report['passed'] else 1)


if __name__=='__main__':main()
