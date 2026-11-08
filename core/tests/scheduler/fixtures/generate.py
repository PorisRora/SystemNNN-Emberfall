"""Self-authored canonical SPTs; PC/frame expectations below are fixed by hand."""
from pathlib import Path
import hashlib
import json
import struct

HERE=Path(__file__).resolve().parent
DATA=0x66660001;CONTROL=0x66660002;SYS=0x66660007
NEXT=0x55560006;SCRIPT=0x5556000a;SUBSCRIPT=0x55560016

def program(code, strings, labels, calls):
    words=[0]*32
    words[:3]=[32,DATA,0x55550001]
    words[12:16]=[0x48545053,0x45444145,0x20203052,0x00202020]
    words.extend(code)
    def table(kind,field,payload,record=False):
        words[field]=len(words)+(0 if record else 4)
        words.extend([4+len(payload),DATA,0x55550002,kind,*payload])
    words[8]=words[10]=len(labels)
    table(0x44440003,9,labels)
    table(0x44440003,16,[],True)
    table(0x44440003,17,[],True)
    def text(kind,field,countfield,values):
        words[countfield]=len(values)
        begin=len(words)+4
        payload=[0]*len(values)
        for i,value in enumerate(values):
            payload[i]=begin+len(payload)
            raw=value.encode('ascii')+b'\0'
            raw+=b'\0'*((-len(raw))%4)
            payload.extend(struct.unpack('<'+'I'*(len(raw)//4),raw))
        table(kind,field,payload)
    text(0x44440002,5,4,['M'])
    text(0x44440001,7,6,strings)
    table(0x44440004,18,[])
    table(0x44440005,20,[32])
    table(0x44440006,21,[])
    table(0x44440007,22,[])
    table(0x44440008,23,calls)
    return words

sub=[5,CONTROL,0x55560009,0,0]
ret=[3,CONTROL,0x55560007]
story=lambda index:[5,SYS,0x33330051,1,index]
models=[
 ('control',sub+[6,0x66660006,0x22220001,0,0,0]+[3,CONTROL,0x55560005]+story(1)+[5,CONTROL,SCRIPT,0,0]+ret,
  ['main','Control'],[46],[51]),
 ('main',sub+[3,CONTROL,NEXT]+story(1)+[5,CONTROL,SUBSCRIPT,0,0]+ret,
  ['_sub','Main'],[40],[45]),
 ('_sub',sub+[3,CONTROL,NEXT]+story(1)+[5,CONTROL,SUBSCRIPT,0,0]+ret,
  ['__sub','Sub'],[40],[45]),
 ('__sub',sub+[3,CONTROL,NEXT]+story(0)+[3,CONTROL,NEXT],['Deep'],[40],[]),
]
files=[]
for name,code,strings,labels,calls in models:
    words=program(code,strings,labels,calls)
    raw=struct.pack('<'+'I'*len(words),*[word^0xffffffff for word in words])
    (HERE/(name+'.spt')).write_bytes(raw)
    (HERE/(name+'.decoded_words.json')).write_text(json.dumps(words,indent=2)+'\n',encoding='utf8')
    files.append({'name':name,'filename':name+'.spt','sha256':hashlib.sha256(raw).hexdigest(),
        'words':len(words),'raw_target_strings':strings,'labels':labels,'script_registry':calls})

f=lambda:[{'return_pc':37,'id':0}]
steps=[
 {'code':SCRIPT,'executed_mode':0,'active_mode':1,'pc':56,'pcs':[56,0,0,0],'frames':[f(),[],[],[]],'slots':[0,None,None,None]},
 {'code':SUBSCRIPT,'executed_mode':1,'active_mode':2,'pc':50,'pcs':[56,50,0,0],'frames':[f(),f(),[],[]],'slots':[0,0,None,None]},
 {'code':SUBSCRIPT,'executed_mode':2,'active_mode':3,'pc':50,'pcs':[56,50,50,0],'frames':[f(),f(),f(),[]],'slots':[0,0,0,None]},
 {'code':NEXT,'executed_mode':3,'active_mode':2,'pc':45,'pcs':[56,50,50,45],'frames':[f(),f(),f(),f()],'slots':[0,0,0,None]},
 {'code':NEXT,'executed_mode':2,'active_mode':1,'pc':37,'pcs':[56,50,37,45],'frames':[f(),f(),[],f()],'slots':[0,0,0,None]},
 {'code':NEXT,'executed_mode':1,'active_mode':0,'pc':37,'pcs':[56,37,37,45],'frames':[f(),[],[],f()],'slots':[0,0,0,None]},
 {'code':0x22220001,'executed_mode':0,'active_mode':0,'pc':43,'pcs':[43,37,37,45],'frames':[[],[],[],f()],'slots':[0,0,0,None]},
 {'code':0x55560005,'executed_mode':0,'active_mode':0,'pc':43,'pcs':[43,37,37,45],'frames':[[],[],[],f()],'slots':[0,0,0,None]},
]
events=[
 {'mode':0,'pc':46,'code':0x33330051,'para1':1,'text_hex':b'Control'.hex(),'para3':0},
 {'mode':0,'pc':51,'code':SCRIPT,'para1':0,'text_hex':b'main'.hex(),'para3':0},
 {'mode':1,'pc':40,'code':0x33330051,'para1':1,'text_hex':b'Main'.hex(),'para3':0},
 {'mode':1,'pc':45,'code':SUBSCRIPT,'para1':0,'text_hex':b'_sub'.hex(),'para3':0},
 {'mode':2,'pc':40,'code':0x33330051,'para1':1,'text_hex':b'Sub'.hex(),'para3':0},
 {'mode':2,'pc':45,'code':SUBSCRIPT,'para1':0,'text_hex':b'__sub'.hex(),'para3':0},
 {'mode':3,'pc':40,'code':0x33330051,'para1':1,'text_hex':b'Deep'.hex(),'para3':0},
 {'mode':0,'pc':37,'code':0x22220001,'para1':0,'text_hex':b'M'.hex(),'para3':0},
]
manifest={'task':'DW-013','authorship':'All bytes and fixed PC/frame/slot/callback expectations self-authored; no current VM used as oracle.',
 'profile':'Three LABEL tails, original seven tables, canonical one slot0 SCRIPT registry; all SUB callsites mapped.',
 'files':files,'chain':{'root':'control.spt','reader_map':{'spt/main.spt':'main.spt','spt/_sub.spt':'_sub.spt','spt/__sub.spt':'__sub.spt'},
 'reader_requests':['spt/main.spt','spt/_sub.spt','spt/__sub.spt'],'steps':steps,'callback_events':events,
 'callback_event_counts_after_step':[2,4,6,7,7,7,8,8]}}
(HERE/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf8')
print('Generated4 self-authored foreground SPTs and fixed8-step oracle')
