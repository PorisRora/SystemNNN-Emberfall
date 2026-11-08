#pragma once

#include "nnn/game_name_list.hpp"
#include "nnn/runtime_context.hpp"
#include <algorithm>
#include <numeric>
#include <string_view>

namespace nnn {
namespace optional_catalog_detail {

inline constexpr std::size_t max_tabs=256, max_voice_entries=65536;

inline int compare(std::string_view a,std::string_view b) noexcept {
    const auto lower=[](unsigned char c) -> unsigned char {
        return c>='A' && c<='Z' ? static_cast<unsigned char>(c+'a'-'A') : c;
    };
    for (std::size_t i=0;i<std::min(a.size(),b.size());++i) {
        const int delta=lower(static_cast<unsigned char>(a[i]))-lower(static_cast<unsigned char>(b[i]));
        if (delta!=0) return delta;
    }
    if (a.size()==b.size()) return 0;
    return a.size()<b.size() ? -1 : 1;
}

inline std::string_view unknown_title(bool one_byte) noexcept {
    return one_byte ? "???" : "\x81\x48\x81\x48\x81\x48";
}

} // namespace optional_catalog_detail

struct TermEntry {
    Value id=0;
    std::string title;
    Value type=0,sort=0;
    std::string text_name;
    bool unlocked=false,is_new=false;
};

class TermCatalog {
public:
    explicit TermCatalog(const std::filesystem::path& root,const resource::FilenameEncoder& encode={})
        : root_(root),encode_(encode) {
        using namespace optional_catalog_detail;
        const auto path=game_config::find(root,"init","printterm.xtx",encode);
        configured_=!path.empty();
        const auto settings=game_config::read_text(path);
        const game_config::NameListSetup setup(settings);
        filters_.resize(static_cast<std::size_t>(std::clamp(setup.integer("termTypeNumber",1),Value{1},static_cast<Value>(max_tabs))));
        per_page_=std::clamp(setup.integer("termPerPage",10),Value{1},static_cast<Value>(runtime_term_count));
        for (std::size_t i=0;i<filters_.size();++i) {
            const auto category=std::to_string(i+1);
            const auto count=std::clamp(setup.integer("termTypeDataNumber"+category,1),Value{0},static_cast<Value>(runtime_term_count));
            for (Value j=1;j<=count;++j) {
                const auto type=setup.integer("termTypeData"+category+"_"+std::to_string(j),static_cast<Value>(i+1));
                if (type!=0) filters_[i].push_back(type);
            }
        }
        const auto list=game_config::read(root,"spt","termList.xtx",encode);
        std::size_t cursor=0;
        std::array<std::string_view,4> fields;
        while (game_config::read_name_row(list,cursor,fields)) {
            if (terms_.size()>=runtime_term_count) throw std::runtime_error("Term list exceeds TERM slot count");
            terms_.push_back({static_cast<Value>(terms_.size()),std::string(fields[0]),game_config::name_number(fields[1]),
                game_config::name_number(fields[2]),std::string(fields[3])});
        }
        order_.resize(terms_.size());
        std::iota(order_.begin(),order_.end(),std::size_t{0});
        // commonPrintTerm sets sort-by-title only, leaving type and param
        // available for filtering and data, respectively.
        std::stable_sort(order_.begin(),order_.end(),[&](std::size_t a,std::size_t b) {
            return compare(terms_[a].title,terms_[b].title)<0;
        });
    }

    bool configured() const noexcept { return configured_; }
    std::size_t category_count() const noexcept { return filters_.size(); }
    Value per_page() const noexcept { return per_page_; }
    static std::string_view unknown_title(bool one_byte) noexcept { return optional_catalog_detail::unknown_title(one_byte); }

    std::vector<TermEntry> entries(Value category,const RuntimeContext& runtime) const {
        const auto index=static_cast<std::size_t>(category);
        if (index>=filters_.size()) return {};
        const auto& filter=filters_[index];
        const auto progress=runtime.optional_progress_view();
        std::vector<TermEntry> rows;
        for (auto id:order_) {
            const auto& term=terms_[id];
            if (filter.empty() || std::find(filter.begin(),filter.end(),term.type)!=filter.end())
                rows.push_back(with_progress(term,*progress));
        }
        return rows;
    }
    std::optional<TermEntry> entry(Value id,const RuntimeContext& runtime) const {
        const auto index=static_cast<std::size_t>(id);
        if (index>=terms_.size()) return {};
        return with_progress(terms_[index],*runtime.optional_progress_view());
    }
    std::vector<std::string> text(Value id) const {
        const auto index=static_cast<std::size_t>(id);
        if (index>=terms_.size()) return {};
        const auto name=resource::normalize_cp932_path(terms_[index].text_name);
        if (name.empty()) return {};
        const auto path=game_config::find_name_list(root_,"nya/term/"+name+".xtx",encode_);
        const auto data=game_config::read_text(path);
        std::vector<std::string> lines;
        std::size_t cursor=0;
        while (const auto line=game_config::bounded_name_token(data,cursor)) {
            if (lines.size()>=runtime_term_count) throw std::runtime_error("Term description exceeds line limit");
            lines.emplace_back(*line);
        }
        return lines;
    }

private:
    static TermEntry with_progress(TermEntry term,const RuntimeOptionalProgress& progress) {
        term.unlocked=progress.term_unlocked(term.id);
        term.is_new=term.unlocked && !progress.term_read(term.id);
        return term;
    }
    std::filesystem::path root_;
    resource::FilenameEncoder encode_;
    bool configured_=false;
    Value per_page_=10;
    std::vector<std::vector<Value>> filters_;
    std::vector<TermEntry> terms_;
    std::vector<std::size_t> order_;
};

// Records in voiceflag.tbl are sorted by raw filename. A nonzero short at
// offset 10 points back to the first variant, and offset 12 is the SYS flag ID.
class VoiceFlagTable {
public:
    struct Range { std::size_t first,end; };

    explicit VoiceFlagTable(const std::filesystem::path& root,const resource::FilenameEncoder& encode={}) {
        const auto path=game_config::find(root,"spt","voiceflag.tbl",encode);
        if (path.empty()) return;
        constexpr std::size_t header_bytes=32, record_bytes=16;
        const auto length=std::filesystem::file_size(path);
        if (length<header_bytes || length>header_bytes+runtime_voice_flag_count*record_bytes)
            throw std::runtime_error("Invalid voiceflag.tbl extent");
        std::ifstream input(path,std::ios::binary);
        std::string bytes(static_cast<std::size_t>(length),'\0');
        if (!input.read(bytes.data(),static_cast<std::streamsize>(bytes.size())) ||
            input.peek()!=std::char_traits<char>::eof() || input.bad())
            throw std::runtime_error("Cannot read complete voiceflag.tbl");
        const auto byte=[&](std::size_t offset) { return static_cast<Word>(static_cast<unsigned char>(bytes[offset])); };
        const auto word=[&](std::size_t offset) {
            return byte(offset)|(byte(offset+1)<<8U)|(byte(offset+2)<<16U)|(byte(offset+3)<<24U);
        };
        const auto count=word(16);
        if (bytes.compare(0,16,"VOICEFLAGANDNAME")!=0 ||
            count>runtime_voice_flag_count || bytes.size()!=header_bytes+static_cast<std::size_t>(count)*record_bytes)
            throw std::runtime_error("Invalid voiceflag.tbl header or record count");
        rows_.reserve(count);
        std::size_t first=0;
        for (std::size_t i=0;i<count;++i) {
            const auto offset=header_bytes+i*record_bytes;
            const auto field=std::string_view(bytes).substr(offset,10);
            const auto end=field.find('\0');
            if (end==0 || end==std::string_view::npos) throw std::runtime_error("Invalid voiceflag.tbl filename");
            const auto filename=field.substr(0,end);
            if (!rows_.empty() && optional_catalog_detail::compare(rows_.back().name,filename)>0)
                throw std::runtime_error("Unsorted voiceflag.tbl records");
            const auto delta=byte(offset+10)|(byte(offset+11)<<8U);
            if (delta==0) first=i;
            if (delta>32767 || delta!=i-first) throw std::runtime_error("Invalid voiceflag.tbl variant group");
            rows_.push_back({std::string(filename),word(offset+12),{first,0}});
        }
        auto end=rows_.size();
        for (std::size_t i=rows_.size();i>0;) {
            auto& range=rows_[--i].range;
            range.end=end;
            if (range.first==i) end=i;
        }
    }

    std::size_t size() const noexcept { return rows_.size(); }
    std::optional<Range> range(std::string_view filename) const noexcept {
        if (rows_.empty()) return {};
        std::size_t first=0,last=rows_.size()-1;
        while (first<last) {
            const auto middle=first+(last-first)/2;
            const auto order=optional_catalog_detail::compare(filename,rows_[middle].name);
            if (order==0) return rows_[middle].range;
            if (order<0) last=middle;
            else first=middle;
            // Match the source's last-row check without an iteration cap.
            if (first+1==last) {
                if (optional_catalog_detail::compare(filename,rows_[last].name)==0) return rows_[last].range;
                break;
            }
        }
        if (optional_catalog_detail::compare(filename,rows_[first].name)==0) return rows_[first].range;
        return {};
    }
    bool played(Range group,const RuntimeOptionalProgress& progress) const noexcept {
        if (group.end>rows_.size()) return false;
        for (auto i=group.first;i<group.end;++i) {
            const auto flag=rows_[i].flag;
            if (flag<runtime_voice_flag_count && progress.voice_played(static_cast<Value>(flag))) return true;
        }
        return false;
    }
private:
    struct Row {
        std::string name;
        Word flag;
        Range range;
    };
    std::vector<Row> rows_;
};

struct VoiceEntry {
    Value character=0,id=0;
    std::string control,filename,title;
    bool played=false,available=false,flag_dependent=false,valid_source=false;
};

class VoiceCatalog {
public:
    using ResolveVariable=std::function<Value(std::string_view)>;
    using ReadVariable=std::function<Value(Value)>;

    explicit VoiceCatalog(const std::filesystem::path& root,const resource::FilenameEncoder& encode={}) {
        using namespace optional_catalog_detail;
        try { flags_.emplace(root,encode); }
        catch (const std::exception& error) { flag_table_error_=error.what(); }
        const auto path=game_config::find(root,"init","listenvoice.xtx",encode);
        configured_=!path.empty();
        const auto settings=game_config::read_text(path);
        const game_config::NameListSetup setup(settings);
        unknown_=setup.field("unknownVoice");
        voices_.resize(static_cast<std::size_t>(std::clamp(setup.integer("voiceCharaNumber",1),Value{1},static_cast<Value>(max_tabs))));
        per_page_=std::clamp(setup.integer("voicePerPage",10),Value{1},static_cast<Value>(max_voice_entries));
        std::size_t total=0;
        for (std::size_t i=0;i<voices_.size();++i) {
            const auto list=game_config::read(root,"nya","charaVoiceList"+std::to_string(i+1)+".xtx",encode);
            std::size_t cursor=0;
            std::array<std::string_view,3> fields;
            auto& character=voices_[i];
            while (game_config::read_name_row(list,cursor,fields)) {
                if (++total>max_voice_entries) throw std::runtime_error("Voice list exceeds entry limit");
                const bool numeric=!fields[0].empty() && fields[0].front()>='0' && fields[0].front()<='9';
                const auto type=numeric ? std::optional<Value>{game_config::name_number(fields[0])} : std::nullopt;
                const auto range=flags_ ? flags_->range(fields[1]) : std::nullopt;
                if (type==1 && flags_ && !range) ++missing_flag_ranges_;
                VoiceEntry entry{static_cast<Value>(i),static_cast<Value>(character.size()),std::string(fields[0]),
                    std::string(fields[1]),std::string(fields[2])};
                entry.valid_source=!resource::normalize_cp932_path(entry.filename).empty();
                character.push_back({std::move(entry),type,range});
            }
        }
    }

    bool configured() const noexcept { return configured_; }
    std::size_t character_count() const noexcept { return voices_.size(); }
    std::size_t entry_count(Value character) const noexcept {
        const auto index=static_cast<std::size_t>(character);
        return index<voices_.size() ? voices_[index].size() : 0;
    }
    bool has_flag_table() const noexcept { return flags_ && flags_->size()!=0; }
    const std::string& flag_table_error() const noexcept { return flag_table_error_; }
    std::size_t missing_flag_ranges() const noexcept { return missing_flag_ranges_; }
    Value per_page() const noexcept { return per_page_; }
    std::string_view unknown_title(bool one_byte) const noexcept {
        return unknown_ ? std::string_view(*unknown_) : optional_catalog_detail::unknown_title(one_byte);
    }
    std::vector<VoiceEntry> entries(Value character,const RuntimeContext& runtime,
        const ResolveVariable& resolve={},const ReadVariable& read={}) const {
        const auto index=static_cast<std::size_t>(character);
        if (index>=voices_.size()) return {};
        const auto progress=runtime.optional_progress_view();
        std::vector<VoiceEntry> rows;
        rows.reserve(voices_[index].size());
        for (const auto& voice:voices_[index]) rows.push_back(with_progress(voice,*progress,resolve,read));
        return rows;
    }
    std::optional<VoiceEntry> entry(Value character,Value id,const RuntimeContext& runtime,
        const ResolveVariable& resolve={},const ReadVariable& read={}) const {
        const auto c=static_cast<std::size_t>(character),i=static_cast<std::size_t>(id);
        if (c>=voices_.size() || i>=voices_[c].size()) return {};
        return with_progress(voices_[c][i],*runtime.optional_progress_view(),resolve,read);
    }

private:
    struct Voice {
        VoiceEntry entry;
        std::optional<Value> numeric;
        std::optional<VoiceFlagTable::Range> flags;
    };
    VoiceEntry with_progress(const Voice& voice,const RuntimeOptionalProgress& progress,
        const ResolveVariable& resolve,const ReadVariable& read) const {
        auto entry=voice.entry;
        const auto type=voice.numeric ? *voice.numeric : resolve ? resolve(entry.control) : -1;
        entry.flag_dependent=type==1;
        entry.valid_source=entry.valid_source && (!entry.flag_dependent || voice.flags.has_value());
        entry.played=entry.flag_dependent && flags_ && voice.flags && flags_->played(*voice.flags,progress);
        entry.available=entry.valid_source && (type<=0 || entry.played || (type>1 && read && read(type)!=0));
        return entry;
    }
    std::optional<VoiceFlagTable> flags_;
    std::string flag_table_error_;
    std::size_t missing_flag_ranges_=0;
    std::vector<std::vector<Voice>> voices_;
    std::optional<std::string> unknown_;
    bool configured_=false;
    Value per_page_=10;
};

} // namespace nnn
