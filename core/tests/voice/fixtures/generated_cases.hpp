#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace dw016_fixture {
struct Frame { std::uint32_t return_pc,id; };
struct Step { std::uint32_t return_code,pc; std::vector<Frame> frames; };
struct State { std::uint32_t pc; std::vector<Frame> frames; };
struct Case { std::string name,filename; std::vector<Step> steps; std::string callback_json; std::vector<State> callback_states; };
inline std::vector<Case> cases() { return {
{"voice_legacy7","voice_legacy7.spt",{{572653569U,44U,{}},{1431699461U,44U,{}}},R"json([{"kind":"numeric_script","code":858980457,"para1":2,"parameters":[0,-999],"para3":0},{"kind":"script","code":572653569,"para1":0,"text_hex":"4d","para3":0}])json",{{32U,{}},{38U,{}}}},
{"voice_labels","voice_labels.spt",{{572653569U,44U,{}},{1431699461U,44U,{}}},R"json([{"kind":"numeric_script","code":858980457,"para1":2,"parameters":[0,-999],"para3":0},{"kind":"script","code":572653569,"para1":0,"text_hex":"4d","para3":0}])json",{{32U,{}},{38U,{}}}},
}; }
} // namespace dw016_fixture
