#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace dw015_fixture {
struct Frame { std::uint32_t return_pc,id; };
struct Step { std::uint32_t return_code,pc; std::vector<Frame> frames; };
struct State { std::uint32_t pc; std::vector<Frame> frames; };
struct Case { std::string name,filename; std::vector<Step> steps; std::string callback_json; std::vector<State> callback_states; };
inline std::vector<Case> cases() { return {
{"stop_legacy7","stop_legacy7.spt",{{572653569U,44U,{}},{1431699461U,44U,{}}},R"json([{"kind":"numeric_script","code":858980359,"para1":2,"parameters":[-1,0],"para3":0},{"kind":"script","code":572653569,"para1":0,"text_hex":"4d","para3":0}])json",{{32U,{}},{38U,{}}}},
{"stop_labels","stop_labels.spt",{{572653569U,44U,{}},{1431699461U,44U,{}}},R"json([{"kind":"numeric_script","code":858980359,"para1":2,"parameters":[-1,0],"para3":0},{"kind":"script","code":572653569,"para1":0,"text_hex":"4d","para3":0}])json",{{32U,{}},{38U,{}}}},
}; }
} // namespace dw015_fixture
