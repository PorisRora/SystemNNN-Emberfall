#pragma once
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
{"raw_legacy7","raw_legacy7.spt",{{572653569U,93U,{}},{1431699461U,93U,{}}},R"json([{"kind":"script","code":858980434,"para1":0,"text_hex":"","para3":0},{"kind":"script","code":858980434,"para1":1,"text_hex":"","para3":0},{"kind":"script","code":858980434,"para1":-1,"text_hex":"","para3":0},{"kind":"script","code":858980434,"para1":2147483647,"text_hex":"","para3":0},{"kind":"script","code":858980434,"para1":-2147483648,"text_hex":"","para3":0},{"kind":"script","code":858980434,"para1":-2147483647,"text_hex":"","para3":0},{"kind":"script","code":858980434,"para1":32767,"text_hex":"","para3":0},{"kind":"script","code":858980434,"para1":32768,"text_hex":"","para3":0},{"kind":"script","code":858980434,"para1":305419896,"text_hex":"","para3":0},{"kind":"script","code":858980434,"para1":-2147483641,"text_hex":"","para3":0},{"kind":"script","code":858980434,"para1":-1879048185,"text_hex":"","para3":0},{"kind":"script","code":572653569,"para1":0,"text_hex":"4d","para3":0}])json",{{32U,{}},{37U,{}},{42U,{}},{47U,{}},{52U,{}},{57U,{}},{62U,{}},{67U,{}},{72U,{}},{77U,{}},{82U,{}},{87U,{}}},{0U,1U,4294967295U,2147483647U,2147483648U,2147483649U,32767U,32768U,305419896U,2147483655U,2415919111U}},
{"raw_labels","raw_labels.spt",{{572653569U,93U,{}},{1431699461U,93U,{}}},R"json([{"kind":"script","code":858980434,"para1":0,"text_hex":"","para3":0},{"kind":"script","code":858980434,"para1":1,"text_hex":"","para3":0},{"kind":"script","code":858980434,"para1":-1,"text_hex":"","para3":0},{"kind":"script","code":858980434,"para1":2147483647,"text_hex":"","para3":0},{"kind":"script","code":858980434,"para1":-2147483648,"text_hex":"","para3":0},{"kind":"script","code":858980434,"para1":-2147483647,"text_hex":"","para3":0},{"kind":"script","code":858980434,"para1":32767,"text_hex":"","para3":0},{"kind":"script","code":858980434,"para1":32768,"text_hex":"","para3":0},{"kind":"script","code":858980434,"para1":305419896,"text_hex":"","para3":0},{"kind":"script","code":858980434,"para1":-2147483641,"text_hex":"","para3":0},{"kind":"script","code":858980434,"para1":-1879048185,"text_hex":"","para3":0},{"kind":"script","code":572653569,"para1":0,"text_hex":"4d","para3":0}])json",{{32U,{}},{37U,{}},{42U,{}},{47U,{}},{52U,{}},{57U,{}},{62U,{}},{67U,{}},{72U,{}},{77U,{}},{82U,{}},{87U,{}}},{0U,1U,4294967295U,2147483647U,2147483648U,2147483649U,32767U,32768U,305419896U,2147483655U,2415919111U}},
}; }
} // namespace dw014_fixture
