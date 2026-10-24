#!/usr/bin/env python3
"""Generate the original 'Copper Circuit' demo. No third-party music/assets."""
from pathlib import Path
import struct
PPQ=480

def vlq(n):
    result=[n&127]
    while n>>7:
        n>>=7; result.insert(0,(n&127)|128)
    return bytes(result)

def chunk(events):
    data=bytearray(); prev=0
    for tick,order,event in sorted(events,key=lambda x:(x[0],x[1])):
        data+=vlq(tick-prev)+event; prev=tick
    data+=vlq(PPQ*64-prev)+b'\xff\x2f\0'
    return b'MTrk'+struct.pack('>I',len(data))+data

def note(events,ch,pitch,start,length,vel):
    events.append((start,1,bytes([0x90|ch,pitch,vel])))
    events.append((start+length,0,bytes([0x80|ch,pitch,0])))

name=b'Copper Circuit'
tracks=[chunk([(0,0,b'\xff\x03'+vlq(len(name))+name),(0,1,b'\xff\x51\x03\x07\xa1\x20')])]
chords=[(57,60,64,69),(53,57,60,65),(48,55,60,64),(55,59,62,67)]*4
for ch,patch in [(0,10),(1,89),(2,38),(9,0)]:
    ev=[(0,0,bytes([0xc0|ch,patch]))]
    for bar,notes in enumerate(chords):
        start=bar*PPQ*4
        if ch==0:
            for step in range(8): note(ev,ch,notes[[0,1,2,3,2,1,3,2][step]]+12,start+step*240,190,77 if step%2 else 95)
        elif ch==1:
            for pitch in notes: note(ev,ch,pitch,start,PPQ*4-80,46)
        elif ch==2:
            for beat in range(4): note(ev,ch,notes[0]-24+(12 if beat==3 else 0),start+beat*PPQ,300,86)
        else:
            for step in range(8): note(ev,ch,42,start+step*240,80,43 if step%2 else 58)
            for beat in [0,2]: note(ev,ch,36,start+beat*PPQ,120,100)
            for beat in [1,3]: note(ev,ch,38,start+beat*PPQ,120,83)
    tracks.append(chunk(ev))
Path(__file__).with_name('copper-circuit.mid').write_bytes(b'MThd'+struct.pack('>IHHH',6,1,len(tracks),PPQ)+b''.join(tracks))
