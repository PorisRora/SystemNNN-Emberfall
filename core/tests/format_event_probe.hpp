#pragma once

#include "nnn/script.hpp"

#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

struct FormatEventProbe final : nnn::CScriptCallBack {
    nnn::CExecScript* vm=nullptr;
    std::vector<nnn::SystemEvent> events;
    std::vector<nnn::Word> pcs;
    std::vector<std::tuple<nnn::Word,nnn::Word,nnn::Word>> skipped;
    std::vector<std::optional<std::string>> predata;
    void ReceiveScriptCommand(nnn::Word,nnn::Value,const std::string&,nnn::Value)override{}
    void PreReceiveScriptData(nnn::Word,nnn::Value,const std::string&)override{}
    void PreReceiveOptionalScriptData(nnn::Word,nnn::Value,const std::string* name)override{
        predata.push_back(name?std::optional<std::string>(*name):std::nullopt);
    }
    void ReceiveUserCommand0(nnn::Word,const std::vector<nnn::Value>&)override{}
    void ReceiveUserFunction0(nnn::Word,const std::vector<nnn::Value>&)override{}
    nnn::Value GetVarData(nnn::Word)override{return 0;}
    nnn::Value GetCalcuVarData(nnn::Word)override{return 0;}
    void SetVarData(nnn::Word,nnn::Value)override{}
    bool ReceiveSystemEvent(const nnn::SystemEvent& event)override{events.push_back(event);pcs.push_back(vm->GetPC());return true;}
    void UnhandledInstruction(nnn::Word pc,nnn::Word identify,nnn::Word opcode)override{skipped.emplace_back(pc,identify,opcode);}
};

inline nnn::Value format_signed(nnn::Word value){
    return value<=INT32_MAX?static_cast<nnn::Value>(value):-1-static_cast<nnn::Value>(UINT32_MAX-value);
}

inline void format_expect_numeric(const std::vector<nnn::Byte>& bytes,nnn::ScriptControlProfile profile,
                                  nnn::Word identify,nnn::Word opcode,const std::vector<nnn::Word>& raw,
                                  std::optional<nnn::Word> tail=std::nullopt,
                                  std::optional<std::string> text=std::nullopt){
    FormatEventProbe host;nnn::CExecScript vm(host);host.vm=&vm;
    const auto loaded=vm.LoadBytes(bytes,"format",{},nullptr,profile);
    if(!loaded)throw std::runtime_error(std::string("format load: ")+nnn::error_name(loaded.code)+" "+loaded.detail);
    const auto length=static_cast<nnn::Word>(4U+raw.size()+(tail?1U:0U));
    const auto result=vm.Exec(1U+length);
    if(result.status.code!=nnn::ErrorCode::BudgetExhausted || result.pc!=32U+length ||
       result.work_used!=1U+length || host.events.size()!=1 || host.pcs!=std::vector<nnn::Word>{32} ||
       !host.skipped.empty())throw std::runtime_error("format dispatch/PC/work");
    std::vector<nnn::Value> expected;for(auto value:raw)expected.push_back(format_signed(value));
    const auto& event=host.events.front();
    const auto expected_tail=tail?std::optional<nnn::Value>(format_signed(*tail)):std::nullopt;
    if(event.identify!=identify || event.opcode!=opcode || event.payload!=nnn::SystemPayload::Numeric ||
       event.primary!=static_cast<nnn::Value>(raw.size()) || event.parameters!=expected ||
       event.trailing!=expected_tail || event.text!=text)throw std::runtime_error("format event payload");
    if(opcode==0x33330008U && identify==0x66660007U) {
        if(host.predata!=std::vector<std::optional<std::string>>{text})throw std::runtime_error("voice predata");
    } else if(!host.predata.empty())throw std::runtime_error("unexpected predata");
}

inline void format_expect_unknown(const std::vector<nnn::Byte>& bytes,nnn::ScriptControlProfile profile,
                                  nnn::Word identify,nnn::Word opcode,nnn::Word length){
    FormatEventProbe host;nnn::CExecScript vm(host);host.vm=&vm;
    const auto loaded=vm.LoadBytes(bytes,"format",{},nullptr,profile);
    if(!loaded)throw std::runtime_error(std::string("unknown load: ")+nnn::error_name(loaded.code)+" "+loaded.detail);
    const auto result=vm.Exec(1U+length);
    if(result.status.code!=nnn::ErrorCode::BudgetExhausted || result.pc!=32U+length ||
       result.work_used!=1U+length || !host.events.empty() ||
       host.skipped!=std::vector<std::tuple<nnn::Word,nnn::Word,nnn::Word>>{{32,identify,opcode}})
        throw std::runtime_error("unknown fallback/PC/work");
}
