"""Two authored exact VOICE(-1,2), defaultVoice0 SPTs; manual event/PC oracle."""
from pathlib import Path
import hashlib
import json
import struct

HERE=Path(__file__).resolve().parent
VOICE,PRINT,END=0x33330008,0x22220001,0x55560005
EXPECTED={
    'exec_calls':[{'return':PRINT,'pc_after':45,'frames':[]},{'return':END,'pc_after':45,'frames':[]}],
    'callback_events':[
        {'kind':'predata','code':VOICE,'para1':1,'text_hex':''},
        {'kind':'numeric_script','code':VOICE,'para1':2,'parameters':[-1,2],'para3':0},
        {'kind':'script','code':PRINT,'para1':0,'text_hex':'4d','para3':0}],
    'callback_states':[{'pc':32,'frames':[]},{'pc':32,'frames':[]},{'pc':39,'frames':[]}],
    'final_variables':{},'error_count':0,'end_reached':True}


def program(labels):
    words=[0]*32;words[:3]=[32,0x66660001,0x55550001]
    words[12:16]=[0x48545053,0x45444145,0x20203052,0x00202020]
    words.extend([7,0x66660007,VOICE,2,0xffffffff,2,0,
                  6,0x66660006,PRINT,0,0,0,3,0x66660002,END])
    def table(kind,field,payload,record=False):
        words[field]=len(words)+(0 if record else 4)
        words.extend([4+len(payload),0x66660001,0x55550002,kind,*payload])
    if labels:
        table(0x44440003,9,[]);table(0x44440003,16,[],True);table(0x44440003,17,[],True)
    words[4]=1;table(0x44440002,5,[len(words)+5,0x4d]);table(0x44440001,7,[])
    for kind,field in [(0x44440004,18),(0x44440005,20),(0x44440006,21),(0x44440007,22),(0x44440008,23)]:table(kind,field,[])
    return words


fixtures=[]
for profile,labels in [('legacy7',False),('labels',True)]:
    name='voice_stop_'+profile;words=program(labels)
    data=struct.pack('<'+'I'*len(words),*[w^0xffffffff for w in words])
    (HERE/(name+'.spt')).write_bytes(data)
    (HERE/(name+'.decoded_words.json')).write_text(json.dumps(words,indent=2)+'\n',encoding='utf-8')
    fixtures.append({'name':name,'binary':name+'.spt','sha256':hashlib.sha256(data).hexdigest(),
                     'table_profile':profile,'expected':EXPECTED})
(HERE/'manifest.json').write_text(json.dumps({'task':'DW-018',
    'authorship':'Fixed raw wire, callback order, PC and return oracle, independent of both VMs.',
    'source_null_predata':'Original NULL is separately observed; portable text is empty.',
    'fixtures':fixtures},indent=2)+'\n',encoding='utf-8')
header='''#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace dw018_fixture {
struct Frame { std::uint32_t return_pc,id; };
struct Step { std::uint32_t return_code,pc; std::vector<Frame> frames; };
struct State { std::uint32_t pc; std::vector<Frame> frames; };
struct Case { std::string name,filename; std::vector<Step> steps; std::string callback_json; std::vector<State> callback_states; };
inline std::vector<Case> cases() { return {
'''
for case in fixtures:
    header+='{\"'+case['name']+'\",\"'+case['binary']+'\",{{572653569U,45U,{}},{1431699461U,45U,{}}},R"json('
    header+=json.dumps(EXPECTED['callback_events'],separators=(',',':'))+')json",{{32U,{}},{32U,{}},{39U,{}}}},\n'
header+='}; }\n} // namespace dw018_fixture\n'
(HERE/'generated_cases.hpp').write_text(header,encoding='utf-8')
print('Generated2 exact VOICE(-1,2) fixtures: predata32/numeric32/PRINT39/yield45/END45')
