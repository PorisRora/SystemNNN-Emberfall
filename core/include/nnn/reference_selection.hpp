#pragma once

#include "nnn/script.hpp"
#include <algorithm>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace nnn::reference_selection {

// Setup strings and variable names are the game's original bytes.
struct Setup {
    std::function<int(const std::string&,int)> number;
    std::function<std::string(const std::string&,const std::string&)> text;
};
using Read=std::function<Value(const std::string&)>;
using Write=std::function<void(const std::string&,Value)>;

inline constexpr int enabled=1;
struct Row {
    int id=0,flags=0;
    std::string label,asset,detail;
};
struct Page {
    // Page indices are zero-based. Group and row IDs start at 1.
    int page=0,pages=1,group=0,selection=0;
    std::vector<Row> rows;
};

inline bool flag(const Read& read,const std::string& variable) {
    return !variable.empty() && read(variable)!=0;
}

class Hints {
public:
    Hints(Setup select,Setup print):select_(std::move(select)),print_(std::move(print)) {}

    bool unlocked(const Read& read,int group) const {
        return group>=1 && group<=group_count() &&
            flag(read,select_.text("group"+std::to_string(group)+"Flag",""));
    }
    bool has_group(int group) const {
        return group>=1 && group<=std::clamp(print_.number("hintAllNumber",1),0,256);
    }
    Page groups(const Read& read) const {
        Page result;
        const int count=group_count();
        std::string asset;
        for (int id=1;id<=count;++id) {
            const auto key="group"+std::to_string(id);
            asset=select_.text(key+"FileName",asset);
            if (flag(read,select_.text(key+"Flag","")))
                result.rows.push_back({id,enabled,select_.text(key+"Name",""),asset,{}});
        }
        return result;
    }
    Page pictures(const Read& read,int group,int page) const {
        Page result;
        if (!has_group(group)) return result;
        result.group=group;
        const int per_page=std::clamp(print_.number("picNumX",2),1,64)*
            std::clamp(print_.number("picNumY",1),1,64);
        const auto key="hint"+std::to_string(group);
        const int count=std::clamp(print_.number(key+"Number",1),0,4096);
        result.pages=std::max(1,(count+per_page-1)/per_page);
        result.page=std::clamp(page,0,result.pages-1);
        const auto locked=print_.text("nopicFilename","");
        const int first=result.page*per_page+1;
        const int end=std::min(first+per_page,count+1);
        for (int id=first;id<end;++id) {
            const auto suffix=std::to_string(id);
            const bool seen=flag(read,print_.text(key+"flag"+suffix,""));
            result.rows.push_back({id,seen?enabled:0,{},seen?print_.text(key+"filename"+suffix,""):locked,{}});
        }
        return result;
    }

private:
    int group_count() const {return std::clamp(select_.number("groupNumber",1),0,256);}

    Setup select_,print_;
};

inline constexpr int max_zukan_entries=4096;
inline constexpr char default_zukan_variable[]="\x90\x7d\x8a\xd3\x94\xd4\x8d\x86"; // 図鑑番号

class Zukan {
public:
    explicit Zukan(Setup setup):setup_(std::move(setup)) {}

    std::string selection_variable() const {return setup_.text("returnVarName",default_zukan_variable);}
    Page grid(const Read& read,int selection) const {
        Page result;
        const int count=entry_count();
        result.selection=std::clamp(selection,0,count);
        if (result.selection!=0) return result;
        const auto sheet=setup_.text("filenameSeat","");
        const int selectable=selectable_count();
        for (int first=1;first<=count;first+=bits_per_variable) {
            const auto variable=appear_variable(first);
            if (variable.empty()) continue;
            const auto bits=read(variable);
            const int end=std::min(first+bits_per_variable,count+1);
            for (int id=first;id<end;++id)
                if (bits&bit(id)) result.rows.push_back({id,id<=selectable?enabled:0,{},sheet,{}});
        }
        return result;
    }
    bool choose(const Read& read,const Write& write,int id) const {
        if (id<1 || id>entry_count() || id>selectable_count()) return false;
        const auto variable=appear_variable(id);
        if (variable.empty() || !(read(variable)&bit(id))) return false;
        const auto result=selection_variable();
        if (!result.empty()) write(result,id);
        return true;
    }
    void clear(const Write& write) const {
        const auto variable=selection_variable();
        if (!variable.empty()) write(variable,0);
    }
    bool unlock(const Read& read,const Write& write,int id) const {
        const int blocks=(entry_count()+bits_per_variable-1)/bits_per_variable;
        if (id<1 || id>blocks*bits_per_variable) return false;
        const auto variable=appear_variable(id);
        if (!variable.empty()) write(variable,read(variable)|bit(id));
        return true;
    }

private:
    static constexpr int bits_per_variable=30;
    static Value bit(int id) {return Value{1}<<((id-1)%bits_per_variable);}
    int entry_count() const {return std::clamp(setup_.number("zukanNumber",1),0,max_zukan_entries);}
    int selectable_count() const {
        return std::clamp(setup_.number("zukanNumberX",5),0,max_zukan_entries)*
            std::clamp(setup_.number("zukanNumberY",4),0,max_zukan_entries);
    }
    std::string appear_variable(int id) const {
        static constexpr char default_appear[]="\x90\x7d\x8a\xd3\x8c\xa9\x82\xbd\x83\x74\x83\x89\x83\x4f"; // 図鑑見たフラグ
        const int block=(id-1)/bits_per_variable+1;
        const auto fallback=block==1?setup_.text("appearVarName",default_appear):std::string(default_appear);
        return setup_.text("appearVarName"+std::to_string(block),fallback);
    }

    Setup setup_;
};

} // namespace nnn::reference_selection
