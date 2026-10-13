#!/usr/bin/env python3
"""Small authenticated client for OBS's official websocket 5 API."""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import uuid
import websocket


class OBS:
    def __init__(self):
        config=json.loads((Path.home()/'Library/Application Support/obs-studio/plugin_config/obs-websocket/config.json').read_text())
        self.ws=websocket.create_connection(f'ws://127.0.0.1:{config["server_port"]}',timeout=15)
        hello=json.loads(self.ws.recv())['d']
        identify=dict(rpcVersion=1,eventSubscriptions=0)
        if 'authentication' in hello:
            auth=hello['authentication']
            secret=base64.b64encode(hashlib.sha256((config['server_password']+auth['salt']).encode()).digest()).decode()
            identify['authentication']=base64.b64encode(hashlib.sha256((secret+auth['challenge']).encode()).digest()).decode()
        self.ws.send(json.dumps(dict(op=1,d=identify)))
        if json.loads(self.ws.recv())['op']!=2:raise RuntimeError('OBS authentication failed')

    def call(self,name,**data):
        id=str(uuid.uuid4())
        self.ws.send(json.dumps(dict(op=6,d=dict(requestType=name,requestId=id,requestData=data))))
        while True:
            msg=json.loads(self.ws.recv())
            if msg['op']==7 and msg['d']['requestId']==id:
                r=msg['d']
                if not r['requestStatus']['result']:raise RuntimeError(f'{name}: {r["requestStatus"]}')
                return r.get('responseData',{})

    def setup(self,output):
        name='Lighthouse validation'
        profiles=self.call('GetProfileList')['profiles']
        if name not in profiles:self.call('CreateProfile',profileName=name)
        self.call('SetCurrentProfile',profileName=name)
        collections=self.call('GetSceneCollectionList')['sceneCollections']
        if name not in collections:self.call('CreateSceneCollection',sceneCollectionName=name)
        self.call('SetCurrentSceneCollection',sceneCollectionName=name)
        for category,key,value in [('Output','Mode','Advanced'),('AdvOut','RecType','Standard'),
            ('AdvOut','RecEncoder','obs_x264'),('AdvOut','RecTracks','63'),('AdvOut','RecFormat2','mkv'),
            ('AdvOut','RecFilePath',str(output.resolve())),('AdvOut','RecUseRescale','false'),
            ('Video','BaseCX','1920'),('Video','BaseCY','1080'),('Video','OutputCX','1920'),
            ('Video','OutputCY','1080'),('Video','FPSType','0'),('Video','FPSCommon','24'),
            ('Audio','SampleRate','48000'),('Audio','ChannelSetup','Stereo')]:
            self.call('SetProfileParameter',parameterCategory=category,parameterName=key,parameterValue=value)
        for i,label in enumerate(['Full mix','Audience score','Crew guide','Click','Async sea','Effects'],1):
            self.call('SetProfileParameter',parameterCategory='AdvOut',parameterName=f'Track{i}Name',parameterValue=label)
        self.call('SetRecordDirectory',recordDirectory=str(output.resolve()))
        # OBS constructs its Simple/Advanced output handler on profile activation.
        # Writing Mode alone does not reconstruct an already-active handler.
        other=next(p for p in self.call('GetProfileList')['profiles'] if p!=name)
        self.call('SetCurrentProfile',profileName=other)
        self.call('SetCurrentProfile',profileName=name)
        self.call('SetVideoSettings',baseWidth=1920,baseHeight=1080,outputWidth=1920,outputHeight=1080,fpsNumerator=24,fpsDenominator=1)
        scenes=self.call('GetSceneList')['scenes']
        scene='Live rehearsal'
        if not any(s['sceneName']==scene for s in scenes):self.call('CreateScene',sceneName=scene)
        self.call('SetCurrentProgramScene',sceneName=scene)
        existing={i['inputName'] for i in self.call('GetInputList')['inputs']}
        for old in sorted(existing):
            if old.startswith('BlackHole live '):self.call('RemoveInput',inputName=old)
        for i,stem in enumerate(['main','guide','click','sea','effects']):
            source=f'Live received {stem}'
            settings=dict(is_local_file=False,input=f'udp://127.0.0.1:{18100+i}?fifo_size=1000000&overrun_nonfatal=1',
                input_format='mpegts',buffering_mb=1,reconnect_delay_sec=1,restart_on_activate=True,
                clear_on_media_end=False,close_when_inactive=False)
            if source not in existing:self.call('CreateInput',sceneName=scene,inputName=source,inputKind='ffmpeg_source',inputSettings=settings,sceneItemEnabled=True)
            else:self.call('SetInputSettings',inputName=source,inputSettings=settings,overlay=True)
            self.call('SetInputAudioTracks',inputName=source,inputAudioTracks={str(t):t in [1,i+2] for t in range(1,7)})
            self.call('SetInputAudioMonitorType',inputName=source,monitorType='OBS_MONITORING_TYPE_NONE')
        return dict(profile=name,scene=scene,inputs=self.call('GetInputList'),video=self.call('GetVideoSettings'))


def main():
    ap=argparse.ArgumentParser();ap.add_argument('command',choices=['info','setup','start','stop','call'])
    ap.add_argument('--output',type=Path,default=Path('artifacts/last-lighthouse/obs'))
    ap.add_argument('--request');ap.add_argument('--data',default='{}')
    args=ap.parse_args();obs=OBS()
    if args.command=='info':result=dict(version=obs.call('GetVersion'),kinds=obs.call('GetInputKindList'),video=obs.call('GetVideoSettings'),inputs=obs.call('GetInputList'))
    elif args.command=='setup':args.output.mkdir(parents=True,exist_ok=True);result=obs.setup(args.output)
    elif args.command=='start':result=obs.call('StartRecord')
    elif args.command=='stop':result=obs.call('StopRecord')
    else:result=obs.call(args.request,**json.loads(args.data))
    print(json.dumps(result,indent=2));obs.ws.close()


if __name__=='__main__':main()
