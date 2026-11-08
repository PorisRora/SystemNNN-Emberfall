#pragma once

#include "nnn/inventory_catalog.hpp"
#include "nnn/runtime_context.hpp"
#include <cstdlib>

namespace nnn::status_party {

enum class Kind { Status, LegacyParty, Party };
enum class Action { Choose, Move, Preset };

struct Control {
    Value id, value;
    bool enabled, checked;
};

struct Visit {
    std::vector<Value> places, parameters, initial_data;
    unsigned elapsed=0;
};

struct Binding {
    std::string key, variable;
    Value fallback=0;
};

class Catalog {
public:
    Catalog(bool party,std::string_view setup_text,std::string_view face_text) {
        if (setup_text.empty()) return;
        const game_config::NameListSetup setup(setup_text);
        kind_=!party ? Kind::Status : setup.integer("oldVersion",0)!=0 ? Kind::LegacyParty : Kind::Party;
        if (kind_==Kind::LegacyParty) {
            load_legacy(setup);
            return;
        }
        if (setup.integer("freePrintFlag",0)!=0)
            for (std::size_t i=1;i<system_mode_names.size();++i)
                if (!system_mode_names[i].empty())
                    overlay_modes_[i]=setup.integer(std::string(system_mode_names[i])+"IsFreePrint",0)>0;
        const auto sets=count(setup,"setNumber",party ? 0 : 1);
        presets_=std::max(1,count(setup,"setPlaceSetNumber",1));
        preset_var_=setup.field("setPlaceSelectVarName").value_or("");
        radio_=setup.integer("useRadio",0)!=0 && presets_>1;
        for (Value id=1;id<=sets;++id)
            places_.emplace_back(setup.field("controlVarName"+std::to_string(id)).value_or(""));
        if (party) load_party(game_config::NameListSetup(face_text),setup,sets);
        else load_status(setup,sets);
    }

    bool configured() const noexcept { return display_data_; }
    Kind kind() const noexcept { return kind_; }
    bool local_text() const noexcept { return local_text_; }
    bool system_text() const noexcept { return system_text_; }
    bool overlay_for(RuntimeMode mode) const noexcept {
        const auto index=static_cast<std::size_t>(mode);
        return index<overlay_modes_.size() && overlay_modes_[index];
    }
    bool overlay_visible(const Visit& visit,const inventory::ReadVariable& read) const {
        const bool member=std::any_of(visit.places.begin(),visit.places.end(),[](Value order){return order>0;});
        if (kind_==Kind::Status) return member;
        return read_value(read,status_print_var_)!=0 &&
            (member || std::any_of(extras_.begin(),extras_.end(),[&](const auto& var){return read_value(read,var)==0;}));
    }
    const std::vector<Binding>& bindings() const noexcept { return bindings_; }

    Visit begin(const inventory::ReadVariable& read) const {
        Visit visit;
        load_places(visit,read);
        for (const auto& variable:parameters_) visit.parameters.push_back(read_value(read,variable));
        if (kind_!=Kind::Party)
            for (const auto& binding:bindings_) visit.initial_data.push_back(read_value(read,binding.variable,binding.fallback));
        return visit;
    }
    void refresh(Visit& visit,const inventory::ReadVariable& read) const {
        if (kind_==Kind::Party) load_places(visit,read);
    }

    Value data(const Visit& visit,std::size_t index,const inventory::ReadVariable& read) const {
        const auto& binding=bindings_[index];
        return kind_==Kind::Party ? read_value(read,binding.variable,binding.fallback) : visit.initial_data[index];
    }

    Value preset(const inventory::ReadVariable& read) const {
        const auto value=read_value(read,preset_var_);
        return value>=0 && value<presets_ ? value : 0;
    }

    std::vector<Control> controls(const Visit& visit,const inventory::ReadVariable& read) const {
        std::vector<Control> result;
        const auto add=[&](const auto& variables,Value first,Value stride,const auto& value_at,bool off_switch) {
            for (std::size_t i=0;i<variables.size();++i) {
                const auto value=value_at(i);
                result.push_back({first+static_cast<Value>(i)*stride,value,
                    binding_variable(read,variables[i]).has_value(),off_switch ? value<=0 : value>0});
            }
        };
        add(places_,1,1,[&](std::size_t i){return visit.places[i];},false);
        add(parameters_,-1,-1,[&](std::size_t i){return visit.parameters[i];},true);
        add(extras_,-1001,-1,[&](std::size_t i){return read_value(read,extras_[i]);},true);
        return result;
    }

    void advance(Visit& visit,unsigned frames) const noexcept {
        visit.elapsed+=std::min(frames,effect_-visit.elapsed);
    }
    Value progress(const Visit& visit) const noexcept {
        return effect_==0 ? 100 : static_cast<Value>((100ULL*visit.elapsed)/effect_);
    }

    std::optional<std::vector<inventory::VariableWrite>> apply(
            Visit& visit,Action action,Value value,const inventory::ReadVariable& read) const {
        if (kind_==Kind::LegacyParty || visit.elapsed<effect_) return {};
        if (action==Action::Preset) {
            const auto variable=binding_variable(read,preset_var_);
            if (!radio_ || value<0 || value>=presets_) return {};
            if (!variable) return std::vector<inventory::VariableWrite>{};
            return std::vector<inventory::VariableWrite>{{variable->id,variable->value,value}};
        }
        const auto places=static_cast<Value>(places_.size());
        if (action==Action::Move) {
            if (value==0 || value < -places || value>places) return {};
            const auto id=static_cast<std::size_t>(std::abs(value)-1);
            if (!binding_variable(read,places_[id])) return {};
            auto& place=visit.places[id];
            const auto from=std::abs(place),to=from+(value<0 ? -1 : 1);
            const auto other=std::find_if(visit.places.begin(),visit.places.end(),
                [to](Value position){return std::abs(position)==to;});
            if (to<1 || to>places || other==visit.places.end()) return {};
            place=place<0 ? -to : to;
            *other=*other<0 ? -from : from;
        } else if (value>0 && value<=places) {
            const auto id=static_cast<std::size_t>(value-1);
            if (!binding_variable(read,places_[id])) return {};
            visit.places[id]=-visit.places[id];
        } else if (value<0 && value>=-static_cast<Value>(parameters_.size())) {
            const auto id=static_cast<std::size_t>(-value-1);
            const auto next=flipped(visit.parameters[id]);
            if (!next || !binding_variable(read,parameters_[id])) return {};
            visit.parameters[id]=*next;
            return writes(parameters_,visit.parameters,read);
        } else if (value<=-1001 && value>=-1000-static_cast<Value>(extras_.size())) {
            const auto id=static_cast<std::size_t>(-value-1001);
            const auto variable=binding_variable(read,extras_[id]);
            if (!variable) return {};
            const auto next=flipped(variable->value);
            if (!next) return {};
            auto result=writes(places_,visit.places,read);
            result.insert(result.begin(),{variable->id,variable->value,*next});
            return result;
        } else return {};
        return writes(places_,visit.places,read);
    }

private:
    using Setup=game_config::NameListSetup;
    static constexpr Value max_fields=256;
    static Value count(const Setup& setup,const std::string& key,Value fallback) {
        return std::clamp(setup.integer(key,fallback),Value{0},max_fields);
    }
    static std::optional<inventory::Variable> binding_variable(
            const inventory::ReadVariable& read,const std::string& variable) {
        return variable.empty() ? std::nullopt : read(variable);
    }
    static Value read_value(const inventory::ReadVariable& read,const std::string& variable,Value fallback=0) {
        const auto bound=binding_variable(read,variable);
        return bound ? bound->value : fallback;
    }
    static std::optional<Value> flipped(Value value) {
        if (value<std::numeric_limits<Value>::min()+2) return {};
        return 1-value;
    }
    void load_places(Visit& visit,const inventory::ReadVariable& read) const {
        const auto sets=static_cast<Value>(places_.size());
        visit.places.resize(places_.size());
        for (Value id=1;id<=sets;++id) {
            const auto index=static_cast<std::size_t>(id-1);
            const auto value=read_value(read,places_[index]);
            visit.places[index]=value!=0 && value>=-sets && value<=sets ? value : id;
        }
    }
    static std::vector<inventory::VariableWrite> writes(const std::vector<std::string>& variables,
            const std::vector<Value>& values,const inventory::ReadVariable& read) {
        std::vector<inventory::VariableWrite> result;
        for (std::size_t i=0;i<variables.size();++i)
            if (const auto variable=binding_variable(read,variables[i]))
                result.push_back({variable->id,variable->value,values[i]});
        return result;
    }
    bool add(const Setup& setup,std::string key,Value fallback=0,bool always=false) {
        const auto variable=setup.field(key);
        if (variable || always) {
            if (bindings_.size()>=8192) throw std::runtime_error("Status variable bindings exceed the display limit");
            bindings_.push_back({game_config::ascii_lower(std::move(key)),std::string(variable.value_or("")),fallback});
        }
        return variable && !variable->empty();
    }
    void display_if(bool present) noexcept { display_data_=display_data_ || present; }
    void use_text(Value type) noexcept {
        local_text_=local_text_ || type==2;
        system_text_=system_text_ || type==3;
    }

    void load_status(const Setup& setup,Value sets) {
        effect_=static_cast<unsigned>(std::max(0,setup.integer("effectTime",0)));
        for (Value id=1;id<=sets;++id) {
            const auto tag="set"+std::to_string(id);
            const auto graphs=count(setup,tag+"GraphNumber",1);
            const auto type=setup.integer(tag+"GraphType",1);
            if (type<1 || type>5) continue;
            display_if(graphs>0 && (setup.field(tag+"GraphNumber") || setup.field(tag+"GraphType") ||
                       setup.field("controlVarName"+std::to_string(id))));
            for (Value n=1;n<=graphs;++n) {
                const auto key=tag+"_"+std::to_string(n);
                display_if(add(setup,key+"VarName") || setup.field(key+"filename") || setup.field(key+"SizeX"));
            }
        }
    }

    void load_legacy(const Setup& setup) {
        const auto slots=count(setup,"printNumber",1);
        const auto members=count(setup,"partyNumber",slots);
        const auto fields=count(setup,"statusNumber",1);
        const auto extras=count(setup,"etcNumber",0);
        std::vector<Value> types;
        Value type=1;
        for (Value field=1;field<=fields;++field) {
            type=setup.integer("status"+std::to_string(field)+"Type",type);
            types.push_back(type);
        }
        for (Value slot=1;slot<=slots;++slot) add(setup,"print"+std::to_string(slot)+"Var",slot,true);
        if (slots>0 && members>0) {
            display_if(setup.field("filenamePlate").has_value());
            for (Value member=1;member<=members;++member) {
                const auto key="party"+std::to_string(member);
                display_if(setup.field(key+"Filename").has_value());
                for (Value field=1;field<=fields;++field) {
                    const auto column_type=types[static_cast<std::size_t>(field-1)];
                    const auto param=key+"_"+std::to_string(field);
                    // The legacy CheckNK also compares the zero-based member against the status count.
                    if (column_type==1 && member<=fields+1) display_if(add(setup,param+"Var",-1));
                    if (column_type==2 || column_type==3) {
                        use_text(column_type);
                        display_if(setup.field(param+"textNumber").has_value());
                    }
                }
            }
        }
        for (Value i=1;i<=extras;++i) {
            const auto key="etc"+std::to_string(i);
            display_if(add(setup,key+"Var") || setup.field(key+"suujiFilename"));
        }
    }

    void load_party(const Setup& face,const Setup& setup,Value sets) {
        const auto members=count(face,"charaNumber",1);
        const auto texts=count(face,"textNumber",0);
        const auto digits=count(face,"suujiNumber",0);
        const auto bars=count(face,"barNumber",0);
        const auto extras=count(face,"etcNumber",0);
        const auto params=std::min(max_fields,1+texts+digits+bars);
        for (Value i=1;i<=params;++i)
            parameters_.emplace_back(setup.field("paramOffVarName"+std::to_string(i)).value_or(""));
        status_print_var_=face.field("statusPrintVarName").value_or("");
        add(face,"statusPrintVarName");
        add(face,"placeTypeVarName");
        for (Value i=1;i<=members && sets>0;++i) {
            const auto id=std::to_string(i);
            display_if(add(face,"status"+id+"PlayerVarName") || face.field("filenameFace"+id+"_1"));
            add(face,"chara"+id+"VarName");
            for (Value n=1;n<=texts;++n) display_if(add(face,"text"+std::to_string(n)+"_"+id+"VarName"));
            for (Value n=1;n<=digits;++n) display_if(add(face,"data"+std::to_string(n)+"_"+id+"VarName"));
            for (Value n=1;n<=bars;++n) {
                const auto key="bar"+std::to_string(n)+"_"+id;
                display_if(add(face,key+"VarName"));
                add(face,key+"MaxVarName",100,true);
            }
        }
        if (sets>0 && members>0) {
            display_if(face.field("filenamePlate").has_value());
            for (Value n=1;n<=texts;++n) {
                const auto key="text"+std::to_string(n);
                use_text(face.integer(key+"Type",0));
                display_if(face.field(key+"Message1_1").has_value());
            }
        }
        for (Value i=1;i<=extras;++i) {
            const auto key="etc"+std::to_string(i);
            extras_.emplace_back(setup.field(key+"ControlVarName").value_or(""));
            display_if(add(face,key+"VarName") || face.field(key+"PlateFilename"));
        }
        if (sets>0 || extras>0) {
            const auto bound=[](const auto& vars) {
                return std::any_of(vars.begin(),vars.end(),[](const auto& var){return !var.empty();});
            };
            display_if(bound(places_) || bound(parameters_) || bound(extras_));
        }
    }

    Kind kind_=Kind::Status;
    Value presets_=1;
    unsigned effect_=0;
    bool radio_=false,display_data_=false,local_text_=false,system_text_=false;
    std::string preset_var_,status_print_var_;
    std::array<bool,system_mode_names.size()> overlay_modes_{};
    std::vector<std::string> places_,parameters_,extras_;
    std::vector<Binding> bindings_;
};
} // namespace nnn::status_party
