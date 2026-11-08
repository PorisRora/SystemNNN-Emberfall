"""Authored channel1 and sequential channel0/channel1 raw SPTs; manual oracle."""
from pathlib import Path
import hashlib
import json
import struct

HERE = Path(__file__).resolve().parent
VOICE, PRINT, END = 0x33330069, 0x22220001, 0x55560005


def program(channels, labels):
    words = [0] * 32
    words[:3] = [32, 0x66660001, 0x55550001]
    words[12:16] = [0x48545053, 0x45444145, 0x20203052, 0x00202020]
    for channel in channels:
        words.extend([6, 0x66660007, VOICE, 2, channel, 0xfffffc19])
    words.extend([6, 0x66660006, PRINT, 0, 0, 0, 3, 0x66660002, END])

    def table(kind, field, payload, record=False):
        words[field] = len(words) + (0 if record else 4)
        words.extend([4 + len(payload), 0x66660001, 0x55550002, kind, *payload])

    if labels:
        table(0x44440003, 9, [])
        table(0x44440003, 16, [], True)
        table(0x44440003, 17, [], True)
    words[4] = 1
    table(0x44440002, 5, [len(words) + 5, 0x4d])
    table(0x44440001, 7, [])
    for kind, field in [(0x44440004, 18), (0x44440005, 20), (0x44440006, 21),
                        (0x44440007, 22), (0x44440008, 23)]:
        table(kind, field, [])
    return words


fixtures = []
for name, channels in [("voice1", [1]), ("voice01", [0, 1])]:
    for profile, labels in [("legacy7", False), ("labels", True)]:
        case_name = name + "_" + profile
        words = program(channels, labels)
        data = struct.pack("<" + "I" * len(words), *[x ^ 0xffffffff for x in words])
        (HERE / (case_name + ".spt")).write_bytes(data)
        (HERE / (case_name + ".decoded_words.json")).write_text(
            json.dumps(words, indent=2) + "\n", encoding="utf-8")
        # All PCs/returns below are fixed manually, not calculated by a VM.
        numeric_pcs, print_pc, end_pc = ([32], 38, 44) if name == "voice1" else ([32, 38], 44, 50)
        events = [{"kind": "numeric_script", "code": VOICE, "para1": 2,
                   "parameters": [ch, -999], "para3": 0} for ch in channels]
        events.append({"kind": "script", "code": PRINT, "para1": 0, "text_hex": "4d", "para3": 0})
        expected = {
            "exec_calls": [{"return": PRINT, "pc_after": end_pc, "frames": []},
                           {"return": END, "pc_after": end_pc, "frames": []}],
            "callback_events": events,
            "callback_states": [{"pc": pc, "frames": []} for pc in [*numeric_pcs, print_pc]],
            "final_variables": {}, "error_count": 0, "end_reached": True,
        }
        fixtures.append({"name": case_name, "binary": case_name + ".spt", "channels": channels,
                         "sha256": hashlib.sha256(data).hexdigest(), "table_profile": profile,
                         "expected": expected})

(HERE / "manifest.json").write_text(json.dumps({
    "task": "DW-017", "authorship": "Manual raw tuple/PC/callback values; neither VM generates expected.",
    "fixtures": fixtures}, indent=2) + "\n", encoding="utf-8")
header = """#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace dw017_fixture {
struct Frame { std::uint32_t return_pc,id; };
struct Step { std::uint32_t return_code,pc; std::vector<Frame> frames; };
struct State { std::uint32_t pc; std::vector<Frame> frames; };
struct Case { std::string name,filename; std::vector<Step> steps; std::string callback_json; std::vector<State> callback_states; };
inline std::vector<Case> cases() { return {
"""
for case in fixtures:
    expected = case["expected"]
    steps = ",".join("{" + str(step["return"]) + "U," + str(step["pc_after"]) + "U,{}}"
                     for step in expected["exec_calls"])
    states = ",".join("{" + str(state["pc"]) + "U,{}}" for state in expected["callback_states"])
    header += '{"' + case["name"] + '","' + case["binary"] + '",{' + steps + '},R"json('
    header += json.dumps(expected["callback_events"], separators=(",", ":")) + ')json",{' + states + '}},\n'
header += "}; }\n} // namespace dw017_fixture\n"
(HERE / "generated_cases.hpp").write_text(header, encoding="utf-8")
print("Generated4 authored channel1/sequential0+1 SPT fixtures")
