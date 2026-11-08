"""Self-authored FILMTYPETIME bytes and fixed manual PCs, not a VM roundtrip."""
from pathlib import Path
import hashlib
import json
import struct

HERE = Path(__file__).resolve().parent
DATA, CONTROL, SYS, COMMAND = 0x66660001, 0x66660002, 0x66660007, 0x66660006
FILM, PRINT, END = 0x33330052, 0x22220001, 0x55560005
RAW = [0, 1, 0xffffffff, 0x7fffffff, 0x80000000, 0x80000001,
       0x7fff, 0x8000, 0x12345678, 0x80000007, 0x90000007]
# Fixed before encoding: eleven len5 records from32, PRINT87 len6 yields93,
# END93 len3 yields at93. These are the oracle, independent of either VM.
CALLBACK_PCS = [32, 37, 42, 47, 52, 57, 62, 67, 72, 77, 82, 87]
SIGNED = [0, 1, -1, 2147483647, -2147483648, -2147483647,
          32767, 32768, 305419896, -2147483641, -1879048185]
EVENTS = [{"kind": "script", "code": FILM, "para1": value, "text_hex": "", "para3": 0}
          for value in SIGNED]
EVENTS += [{"kind": "script", "code": PRINT, "para1": 0, "text_hex": "4d", "para3": 0}]
STATES = [{"pc": pc, "frames": []} for pc in CALLBACK_PCS]
EXEC = [{"return": PRINT, "pc_after": 93, "frames": []},
        {"return": END, "pc_after": 93, "frames": []}]


def program(labels):
    words = [0] * 32
    words[:3] = [32, DATA, 0x55550001]
    words[12:16] = [0x48545053, 0x45444145, 0x20203052, 0x00202020]
    for value in RAW:
        words.extend([5, SYS, FILM, 1, value])
    words.extend([6, COMMAND, PRINT, 0, 0, 0, 3, CONTROL, END])
    assert len(words) == 96

    def table(kind, field, payload, record=False):
        words[field] = len(words) + (0 if record else 4)
        words.extend([4 + len(payload), DATA, 0x55550002, kind, *payload])

    if labels:
        table(0x44440003, 9, [])
        table(0x44440003, 16, [], True)
        table(0x44440003, 17, [], True)
    # Original seven tables: MES, STR, INT32, SUB, SELECT, COMMAND, SCRIPT.
    words[4] = 1
    table(0x44440002, 5, [len(words) + 5, 0x4d])
    table(0x44440001, 7, [])
    for kind, field in [(0x44440004, 18), (0x44440005, 20),
                        (0x44440006, 21), (0x44440007, 22), (0x44440008, 23)]:
        table(kind, field, [])
    return words


fixtures = []
for name, labels in [("raw_legacy7", False), ("raw_labels", True)]:
    words = program(labels)
    raw = struct.pack("<" + "I" * len(words), *[word ^ 0xffffffff for word in words])
    (HERE / (name + ".spt")).write_bytes(raw)
    (HERE / (name + ".decoded_words.json")).write_text(json.dumps(words, indent=2) + "\n", encoding="utf-8")
    fixtures.append({"name": name, "binary": name + ".spt", "sha256": hashlib.sha256(raw).hexdigest(),
                     "table_profile": "labels" if labels else "legacy7", "raw_values": RAW,
                     "expected": {"exec_calls": EXEC, "callback_events": EVENTS, "callback_states": STATES,
                                  "final_variables": {}, "error_count": 0, "end_reached": True},
                     "source_expected_text_null": [True] * 11 + [False]})
manifest = {"task": "DW-014", "authorship": "Manual raw words, signed values, PC/callback expectations; no VM generated the oracle.",
            "scope": "Both table profiles, Closed and Foreground load modes. Original source runs each binary once; new core runs all four combinations.",
            "raw_token_proof": {"SCRIPT_VAR_index7": "0x80000007 is passed as -2147483641, without GetVarData", "SCRIPT_NUMBER7": "0x90000007 is passed as -1879048185, not7"},
            "fixtures": fixtures}
(HERE / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
header = """#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace dw014_fixture {
struct Frame { std::uint32_t return_pc, id; };
struct Step { std::uint32_t return_code, pc; std::vector<Frame> frames; };
struct State { std::uint32_t pc; std::vector<Frame> frames; };
struct Case { std::string name, filename; std::vector<Step> steps; std::string callback_json;
              std::vector<State> callback_states; std::vector<std::uint32_t> raw_values; };
inline std::vector<Case> cases() { return {
"""
for case in fixtures:
    header += '{"' + case["name"] + '","' + case["binary"] + '",{{572653569U,93U,{}},{1431699461U,93U,{}}},'
    header += 'R"json(' + json.dumps(EVENTS, separators=(",", ":")) + ')json",{'
    header += ",".join("{" + str(pc) + "U,{}}" for pc in CALLBACK_PCS)
    header += "},{" + ",".join(str(v) + "U" for v in RAW) + "}},\n"
header += "}; }\n} // namespace dw014_fixture\n"
(HERE / "generated_cases.hpp").write_text(header, encoding="utf-8")
print("Generated2 fixed FILMTYPETIME scripts; 11raw words,12callbacks,2Exec returns each")
