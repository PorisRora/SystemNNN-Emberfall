#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace dw018_fixture {
struct Frame { std::uint32_t return_pc,id; };
struct Step { std::uint32_t return_code,pc; std::vector<Frame> frames; };
struct State { std::uint32_t pc; std::vector<Frame> frames; };
struct Case { std::string name,filename; std::vector<Step> steps; std::string callback_json; std::vector<State> callback_states; };
inline std::vector<Case> cases() { return {
{"voice_stop_legacy7","voice_stop_legacy7.spt",{{572653569U,45U,{}},{1431699461U,45U,{}}},R"json([{"kind":"predata","code":858980360,"para1":1,"text_hex":""},{"kind":"numeric_script","code":858980360,"para1":2,"parameters":[-1,2],"para3":0},{"kind":"script","code":572653569,"para1":0,"text_hex":"4d","para3":0}])json",{{32U,{}},{32U,{}},{39U,{}}}},
{"voice_stop_labels","voice_stop_labels.spt",{{572653569U,45U,{}},{1431699461U,45U,{}}},R"json([{"kind":"predata","code":858980360,"para1":1,"text_hex":""},{"kind":"numeric_script","code":858980360,"para1":2,"parameters":[-1,2],"para3":0},{"kind":"script","code":572653569,"para1":0,"text_hex":"4d","para3":0}])json",{{32U,{}},{32U,{}},{39U,{}}}},
}; }
} // namespace dw018_fixture
