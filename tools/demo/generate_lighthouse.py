#!/usr/bin/env python3
"""Create original, reproducible show media (numpy, Pillow, ffmpeg; macOS say optional)."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import subprocess
import wave

import numpy as np
from PIL import Image, ImageDraw, ImageFont

RATE = 48000
ROOT = Path(__file__).resolve().parents[2]


def run(*args):
    subprocess.run(list(map(str, args)), check=True)


def wav(path, audio):
    audio = np.asarray(audio)
    with wave.open(str(path), "wb") as f:
        f.setnchannels(1 if audio.ndim == 1 else audio.shape[1])
        f.setsampwidth(2)
        f.setframerate(RATE)
        f.writeframes((np.clip(audio, -.95, .95) * 32767).astype('<i2').tobytes())


def track(name, visual=False, asynchronous=False, left=-1, right=-1, muted=False):
    return dict(id=name.lower().replace(' ', '-'), name=name, visual=visual,
                async_=asynchronous, muted=muted, gain=1, pan=0, left=left, right=right)


def clip(id, asset, track, start, duration, **kw):
    c = dict(id=id, asset=asset, text='', track=track, start=round(start*RATE),
             duration=round(duration*RATE), offset=0, loop=False, gain=1, pan=0,
             x=0, y=0, scale=1, opacity=1, volume=[], balance=[], positionX=[],
             positionY=[], size=[], alpha=[])
    c.update(kw)
    return c


def curve(*points):
    return [[round(t*RATE), v] for t, v in points]


def marker(id, name, t, msg=(), hold=False, timing='arrival'):
    return dict(id=id, name=name, time=round(t*RATE), hold=hold, timing=timing, message=list(msg))


def project(title, duration, tracks, clips, markers, assets):
    for t in tracks:
        t['async'] = t.pop('async_')
    return dict(format='simpleavshow', version=1, title=title, width=1280, height=720,
                fps=24, duration=round(duration*RATE), tracks=tracks, clips=clips,
                markers=markers, assets=assets)


def projection(path, scene, duration=16):
    """Render moving waves, rain, stars and a rotating lighthouse beam to actual H.264."""
    w, h = 1280, 720
    rng = np.random.default_rng(21)
    stars = rng.uniform([0, 0], [w, 400], (65, 2))
    rain = rng.uniform([0, -h], [w, h], (160, 2))
    fontpath = '/System/Library/Fonts/Supplemental/Arial.ttf'
    font = ImageFont.truetype(fontpath, 26) if Path(fontpath).exists() else ImageFont.load_default()
    pipe = subprocess.Popen(['ffmpeg', '-v', 'error', '-y', '-f', 'rawvideo', '-pix_fmt',
        'rgb24', '-s', f'{w}x{h}', '-r', '24', '-i', '-', '-an', '-c:v', 'libx264',
        '-preset', 'fast', '-crf', '23', '-pix_fmt', 'yuv420p', '-movflags', '+faststart', str(path)], stdin=subprocess.PIPE)
    yy = np.linspace(0, 1, h)[:, None, None]
    top, bottom = (([7, 18, 34], [28, 64, 77]) if scene != 'dawn' else ([44, 49, 81], [216, 132, 91]))
    gradient = np.broadcast_to(np.array(top)*(1-yy)+np.array(bottom)*yy, (h, w, 3)).astype('uint8').copy()
    try:
        for i in range(duration*24):
            t = i/24
            im = Image.fromarray(gradient.copy())
            d = ImageDraw.Draw(im, 'RGBA')
            for sx, sy in stars:
                d.ellipse((sx, sy, sx+2, sy+2), fill=(230, 227, 194, int(80+50*math.sin(t+sx))))
            if scene == 'dawn':
                d.ellipse((170, 340-t*3, 330, 500-t*3), fill=(255, 191, 115, 140))
            # A widening moving cone, intentionally slow enough to see the held frame.
            beam = 70*math.sin(t*.45)
            d.polygon([(980, 192), (0, 170+beam), (0, 330+beam)], fill=(248, 224, 160, 28))
            d.polygon([(880, 560), (918, 248), (1028, 248), (1062, 560)], fill=(17, 23, 33, 255))
            d.rectangle((906, 190, 1038, 249), fill=(24, 31, 41, 255))
            d.rectangle((920, 203, 1024, 231), fill=(253, 216, 129, 235))
            d.polygon([(892, 190), (972, 140), (1051, 190)], fill=(13, 18, 28, 255))
            d.rectangle((959, 429, 991, 560), fill=(4, 9, 16, 255))
            for row in range(9):
                y = 486+row*29
                pts = [(x, y+math.sin(x*.012-t*(1.5 if scene=='storm' else .7)+row)*12) for x in range(-20, w+20, 10)]
                d.line(pts, fill=(66+row*5, 119+row*4, 142+row*3, 130), width=3)
            d.polygon([(740, 620), (900, 549), (1080, 550), (1279, 630), (1279, 720), (650, 720)], fill=(8, 16, 24, 255))
            if scene == 'storm':
                for rx, ry in rain:
                    y = (ry+t*300) % h
                    d.line([(rx, y), (rx-14, y+33)], fill=(159, 193, 210, 80), width=1)
            d.text((55, 52), 'THE LAST LIGHTHOUSE', font=font, fill=(244, 228, 194, 220))
            d.text((55, 88), {'night':'I  /  THE WATCH', 'storm':'II  /  THE SIGNAL', 'dawn':'III  /  HOME'}[scene], font=font, fill=(187, 205, 216, 200))
            pipe.stdin.write(im.tobytes())
    finally:
        pipe.stdin.close()
    if pipe.wait() != 0:
        raise RuntimeError('projection encoding failed')


def generate(out):
    out.mkdir(parents=True, exist_ok=True)
    media = out/'media'
    media.mkdir(exist_ok=True)
    # Original chamber score: D minor, Bb, F, C with arpeggiated bell/piano harmonics.
    t = np.arange(RATE*64)/RATE
    score = np.zeros((len(t), 2), dtype=np.float64)
    chords = [(50,57,62,65), (46,53,58,62), (41,48,53,57), (48,55,60,64)]
    for beat in range(128):
        start = int(beat*.5*RATE)
        n = min(int(1.4*RATE), len(t)-start)
        if n <= 0:
            continue
        u = np.arange(n)/RATE
        note = chords[(beat//8)%4][beat%4] + (12 if beat%2 else 0)
        freq = 440*2**((note-69)/12)
        sound = (.09*np.sin(2*np.pi*freq*u)+.035*np.sin(2*np.pi*freq*2*u))*np.exp(-u*3)*np.minimum(u/.008,1)
        score[start:start+n, beat%2] += sound
        score[start:start+n, 1-beat%2] += sound*.45
    wav(media/'score.wav', score)
    run('ffmpeg','-v','error','-y','-i',media/'score.wav','-codec:a','libmp3lame','-b:a','192k',media/'score.mp3')
    # Seamless wind/seabed with an identifiable moving source and stereo motion.
    u = np.arange(RATE*8)/RATE
    rng = np.random.default_rng(1729)
    noise = rng.normal(0, .008, len(u))
    bed = np.column_stack([.038*np.sin(2*np.pi*73*u)*(1+.3*np.sin(2*np.pi*u/8))+noise,
                           .038*np.sin(2*np.pi*97*u)*(1-.3*np.sin(2*np.pi*u/8))+noise])
    wav(media/'sea-loop.wav', bed)
    click = np.zeros(RATE*64)
    for beat in range(128):
        x=np.arange(1200)/RATE
        click[beat*24000:beat*24000+1200] = .16*np.sin(2*np.pi*(1760 if beat%4==0 else 1320)*x)*np.exp(-x*170)
    wav(media/'click.wav', click)
    # An async one-shot bell ends during the first hold; it must not restart.
    x=np.arange(RATE*3)/RATE
    bell=(np.sin(2*np.pi*523.25*x)+.4*np.sin(2*np.pi*1046.5*x))*.13*np.exp(-x*1.7)*np.minimum(x/.005,1)
    wav(media/'bell.wav', np.column_stack([bell, bell*.7]))
    x=np.arange(RATE*4)/RATE
    thunder=rng.normal(0,.065,len(x))*np.exp(-x*1.4)*np.minimum(x/.04,1)
    wav(media/'thunder.wav', np.column_stack([thunder, np.roll(thunder, 380)]))
    # Real spoken crew feed. FFmpeg's synthesized fallback remains reproducible elsewhere.
    guide=np.zeros(RATE*64)
    import shutil
    if shutil.which('say'):
        for when, words in [(1,'Stand by lighthouse and sea.'),(10,'Stand by actor entrance. Hold for the keeper.'),
                            (25,'Storm cue. Stand by the signal.'),(42,'Stand by dawn. Hold for the reply.'),
                            (53,'Final chorus. Lights to warm. Stand by blackout.')]:
            aiff=media/'guide-temp.aiff'
            pcm=media/'guide-temp.wav'
            run('say','-o',aiff,words)
            run('ffmpeg','-v','error','-y','-i',aiff,'-ar',RATE,'-ac','1',pcm)
            with wave.open(str(pcm),'rb') as f:
                speech=np.frombuffer(f.readframes(f.getnframes()),dtype='<i2').astype(float)/32768
            begin=when*RATE
            guide[begin:begin+len(speech)]=speech*.55
        aiff.unlink(); pcm.unlink()
    else:
        for when in [1,10,25,42,53]:
            x=np.arange(RATE)/RATE
            guide[when*RATE:(when+1)*RATE]=.1*np.sin(2*np.pi*660*x)*np.minimum(x/.01,1)*np.minimum((1-x)/.01,1)
    wav(media/'guide.wav',guide)
    # Diagnostic fingerprints: seven harmonics are deliberately avoided.
    frequencies=[211,307,419,557,683,809,947,1103]
    for ch,freq in enumerate(frequencies,1):
        x=np.arange(RATE*6)/RATE
        signal=.12*np.sin(2*np.pi*freq*x)
        signal[:480] *= np.arange(480)/480
        signal[-480:] *= np.arange(480)[::-1]/480
        signal[int(2*RATE)] = .7
        wav(media/f'channel-{ch:02}.wav',signal)
    for scene in ['night','storm','dawn']:
        print(f'Encoding {scene} projection',flush=True)
        projection(media/f'{scene}.mp4',scene)
    # A linked MP4 video/audio pair, with original audible radio static embedded.
    run('ffmpeg','-v','error','-y','-i',media/'storm.mp4','-i',media/'thunder.wav',
        '-c:v','copy','-c:a','aac','-t','4',media/'signal.mp4')
    im=Image.new('RGBA',(1280,720),(0,0,0,0)); d=ImageDraw.Draw(im)
    d.rectangle((45,42,1235,678),outline=(215,181,118,120),width=2)
    d.line((45,650,240,650),fill=(215,181,118,200),width=3)
    im.save(media/'proscenium.png')
    assets=[dict(id=p.stem if p.name != 'score.wav' else 'score-source-wav',name=p.name,path='media/'+p.name) for p in sorted(media.iterdir()) if p.suffix in ['.wav','.mp3','.mp4','.png']]
    tracks=[track('Sea and lighthouse',True),track('Signal film',True),track('Proscenium',True),track('Surtitles',True),
            track('Score MP3 - audience 1-2',left=0,right=1),
            track('Sea WAV - async 5-6',asynchronous=True,left=4,right=5),
            track('Crew guide - 3',left=2),track('Click - 4',left=3),
            track('Signal sound - 7-8',left=6,right=7),
            track('Keeper bell - async one-shot 7-8',asynchronous=True,left=6,right=7),
            track('Muted rehearsal safety',left=0,right=1,muted=True)]
    clips=[clip('watch','night',0,0,24,loop=True),clip('storm','storm',0,24,24,loop=True),clip('dawn','dawn',0,48,16),
           clip('radio-picture','signal',1,27,3.5,offset=12000,link='signal-pair',scale=.55,x=-.19,y=.05,
                alpha=curve((0,0),(.5,1),(3,1),(3.5,0)),size=curve((0,.48),(2,.55),(3.5,.52)),positionY=curve((0,.08),(3.5,0))),
           clip('frame','proscenium',2,0,64),
           clip('score','score',4,0,50,volume=curve((0,0),(2,1),(12,.7),(16,1),(43,.7),(48,1),(50,0)),
                balance=curve((0,-.2),(16,.2),(32,-.1),(48,.1),(50,0))),
           clip('finale-score','score-source-wav',4,48,16,offset=48*RATE,
                volume=curve((0,0),(2,1),(13,1),(16,0))),
           clip('sea','sea-loop',5,0,52,loop=True,volume=curve((0,0),(2,1),(44,1),(52,0))),
           clip('guide','guide',6,0,64),clip('click','click',7,0,64,gain=.65),
           clip('radio-sound','signal',8,27,3.5,offset=12000,link='signal-pair',gain=.65),
           clip('thunder','thunder',8,34,4,gain=.8),
           clip('keeper-bell','bell',9,10,10),
           clip('muted-score','score',10,0,64,gain=1)]
    for id,start,dur,text in [('title',2,8,'THE LAST LIGHTHOUSE\nA chamber musical in three movements'),
        ('keeper',12,10,'KEEPER\nIf I hold the light, will you find your way home?'),
        ('signal',27,12,'THE SIGNAL\nThree flashes across a restless sea.'),
        ('reply',43,9,'THE REPLY\nI see your light. Keep it burning.'),
        ('finale',53,10,'HOME\nThe morning belongs to those who waited.')]:
        clips.append(clip(id,'',3,start,dur,text=text,y=.28,scale=.6,alpha=curve((0,0),(1,1),(dur-1,1),(dur,0)),
            positionX=curve((0,-.03),(dur,.03))))
    markers=[marker('preset','LX 01 - blue night / Program 5',0,[0xC0,4]),
        marker('level','LX intensity / CC7',0,[0xB0,7,90]),
        marker('note-on','Bell reinforcement / Note C4',5,[0x90,60,80]),
        marker('poly','Note pressure',6,[0xA0,60,44]),marker('note-off','Release C4',7,[0x80,60,0]),
        marker('actor-arrival','HOLD - keeper entrance / blue LX',12,[0xB0,1,30],True),
        marker('actor-resume','GO - keeper ready / Program 6',12,[0xC0,5],True,'resume'),
        marker('bend','Pitch bend up',18,[0xE0,0,80]),marker('bend-center','Pitch bend center',19,[0xE0,0,64]),
        marker('pressure','Channel pressure',20,[0xD0,36]),
        marker('storm-lx','LX 02 - storm / CC1',24,[0xB0,1,100]),
        marker('signal-hold','HOLD - signal ready',30,[],True),
        marker('signal-go','GO - signal / educational SysEx',30,[0xF0,0x7D,1,2,3,0xF7],True,'resume'),
        marker('sustain','Sustain on',34,[0xB0,64,127]),marker('sustain-off','Sustain off',35,[0xB0,64,0]),
        marker('reply-hold','HOLD - wait for reply / dim LX',43,[0xB0,11,50],True),
        marker('reply-go','GO - reply received / warm LX',43,[0xC0,8],True,'resume'),
        marker('dawn-lx','LX 03 - dawn',48,[0xB0,11,110]),
        marker('cleanup','External all notes off',62,[0xB0,123,0]),marker('blackout','LX blackout',63,[0xB0,7,0])]
    show=project('The Last Lighthouse - a chamber musical',64,tracks,clips,markers,assets)
    (out/'last-lighthouse.savshow').write_text(json.dumps(show,indent=2)+'\n')
    # A distinct calibration project so no routing tones appear during the show.
    routes=[track(f'Channel {ch} - {freq} Hz',left=ch-1) for ch,freq in enumerate(frequencies,1)]
    calibration=project('The Last Lighthouse - eight-channel line check',8,routes,
        [clip(f'line-{ch}',f'channel-{ch:02}',ch-1,1,6) for ch in range(1,9)],[],assets)
    (out/'routing-check.savshow').write_text(json.dumps(calibration,indent=2)+'\n')
    # A short project exercises pause/inside/outside/end seeks with an async cursor.
    probe=project('The Last Lighthouse - async pause and seek check',20,
        [track('Async sea',asynchronous=True,left=4,right=5),track('Ordinary click',left=3),
         track('Async bell',asynchronous=True,left=6,right=7)],
        [clip('probe-sea','sea-loop',0,0,16,loop=True),clip('probe-click','click',1,0,20),
         clip('probe-bell','bell',2,0,16)],[],assets)
    (out/'async-check.savshow').write_text(json.dumps(probe,indent=2)+'\n')
    inventory=[]
    for p in sorted(media.iterdir()):
        probe_data=json.loads(subprocess.check_output(['ffprobe','-v','error','-show_streams','-show_format','-of','json',str(p)]))
        probe_data['format']['filename']=str(p.relative_to(out))
        inventory.append(dict(file=str(p.relative_to(out)),bytes=p.stat().st_size,
            sha256=hashlib.sha256(p.read_bytes()).hexdigest(),probe=probe_data))
    (out/'asset-manifest.json').write_text(json.dumps(inventory,indent=2)+'\n')
    print(f'Created {len(assets)} real media assets and three shows in {out}',flush=True)


if __name__=='__main__':
    ap=argparse.ArgumentParser()
    ap.add_argument('--output',type=Path,default=ROOT/'demos/last-lighthouse')
    generate(ap.parse_args().output.resolve())
