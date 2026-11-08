#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace dw017_fixture {
struct Frame { std::uint32_t return_pc,id; };
struct Step { std::uint32_t return_code,pc; std::vector<Frame> frames; };
struct State { std::uint32_t pc; std::vector<Frame> frames; };
struct Case { std::string name,filename; std::vector<Step> steps; std::string callback_json; std::vector<State> callback_states; };
inline std::vector<Case> cases() { return {
{"voice1_legacy7","voice1_legacy7.spt",{{572653569U,44U,{}},{1431699461U,44U,{}}},R"json([{"kind":"numeric_script","code":858980457,"para1":2,"parameters":[1,-999],"para3":0},{"kind":"script","code":572653569,"para1":0,"text_hex":"4d","para3":0}])json",{{32U,{}},{38U,{}}}},
{"voice1_labels","voice1_labels.spt",{{572653569U,44U,{}},{1431699461U,44U,{}}},R"json([{"kind":"numeric_script","code":858980457,"para1":2,"parameters":[1,-999],"para3":0},{"kind":"script","code":572653569,"para1":0,"text_hex":"4d","para3":0}])json",{{32U,{}},{38U,{}}}},
{"voice01_legacy7","voice01_legacy7.spt",{{572653569U,50U,{}},{1431699461U,50U,{}}},R"json([{"kind":"numeric_script","code":858980457,"para1":2,"parameters":[0,-999],"para3":0},{"kind":"numeric_script","code":858980457,"para1":2,"parameters":[1,-999],"para3":0},{"kind":"script","code":572653569,"para1":0,"text_hex":"4d","para3":0}])json",{{32U,{}},{38U,{}},{44U,{}}}},
{"voice01_labels","voice01_labels.spt",{{572653569U,50U,{}},{1431699461U,50U,{}}},R"json([{"kind":"numeric_script","code":858980457,"para1":2,"parameters":[0,-999],"para3":0},{"kind":"numeric_script","code":858980457,"para1":2,"parameters":[1,-999],"para3":0},{"kind":"script","code":572653569,"para1":0,"text_hex":"4d","para3":0}])json",{{32U,{}},{38U,{}},{44U,{}}}},
}; }
} // namespace dw017_fixture
