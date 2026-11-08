#pragma once

#include "nnn/game_name_list.hpp"
#include "nnn/script.hpp"
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nnn::inventory {

struct Variable { Word id=0; Value value=0; };
using ReadVariable=std::function<std::optional<Variable>(std::string_view)>;
struct VariableWrite { Word id; Value expected,value; };
struct Purchase { Value item_id; std::vector<VariableWrite> writes; };
struct Cell { Value x=0,y=0,w=0,h=0; };

struct Layout {
    Value columns=4,rows=3,step_x=80,step_y=60;
    Cell first{100,100,80,60};
    bool print_names=false,low_cursor=false,print_plate_item=true;
    std::string no_item_asset,plate_asset,cursor_asset;
    std::vector<std::vector<Value>> categories;

    Value per_page() const noexcept { return columns*rows; }
    Cell cell(Value filtered_index) const noexcept {
        if (filtered_index<0) return {};
        const auto slot=filtered_index%per_page();
        return {first.x+(slot%columns)*step_x,first.y+(slot/columns)*step_y,first.w,first.h};
    }
};

struct Variant { std::string name; std::vector<std::string> description; };
struct ItemEntry {
    Value id=0,serial=0,type=0;
    std::string availability_var,filename;
    std::vector<Variant> variants;
};
struct ItemRow {
    Value id=0,serial=0,type=0,position=0,availability=0,variant=1;
    Cell cell;
    bool detail_selectable=false;
    std::string_view name;
    std::string asset;
    std::vector<std::string_view> description;
};
struct ItemPage {
    Value category=0,page=0,page_count=0;
    std::vector<ItemRow> rows;
};

struct ShopSetup {
    Layout layout;
    bool can_select_limit=false,can_select_no_point=false,print_new=false,print_sold_out=false;
    std::string point_var,hidden_name,new_asset,sold_out_asset;
    std::vector<std::string> seen_vars;
};
struct ShopEntry {
    Value id=0,serial=0,type=0,limit=0;
    std::optional<Value> price;
    std::string name,filename,appear_var,target_var;
    std::vector<std::string> description;
};
struct ShopRow {
    Value id=0,serial=0,type=0,position=0,appearance=0,owned=0,limit=0;
    std::optional<Value> price;
    Cell cell;
    bool sold_out=false,is_new=false,detail_selectable=false,purchase_enabled=false;
    std::string_view name;
    std::string asset;
    std::vector<std::string_view> description;
};
struct ShopPage {
    Value category=0,page=0,page_count=0,balance=0;
    std::vector<ShopRow> rows;
};

namespace detail {

inline constexpr std::size_t max_records=16384,max_description=16,max_variants=15;

inline std::optional<Value> decimal(std::string_view token) noexcept {
    if (token.empty()) return {};
    if (token.front()=='+') {
        token.remove_prefix(1);
        if (token.empty() || token.front()<'0' || token.front()>'9') return {};
    }
    Value value=0;
    const auto* end=token.data()+token.size();
    const auto converted=std::from_chars(token.data(),end,value);
    if (converted.ec!=std::errc{} || converted.ptr!=end) return {};
    return value;
}
inline Value required_number(std::string_view token) {
    const auto value=decimal(token);
    if (!value) throw std::runtime_error("Invalid item record number");
    return *value;
}

// Keep the next % record marker intact while checking every required field.
class Records {
public:
    explicit Records(std::string_view text) : text_(text),token_(game_config::bounded_name_token(text_,cursor_)) {}
    std::optional<std::string_view> record() {
        if (!token_) return {};
        if (token_->empty() || token_->front()!='%') throw std::runtime_error("Missing item record marker");
        const auto serial=token_->substr(1);
        advance();
        return serial;
    }
    std::optional<std::string_view> field() {
        if (!token_ || (!token_->empty() && token_->front()=='%')) return {};
        const auto result=token_;
        advance();
        return result;
    }
    std::string_view required() {
        const auto result=field();
        if (!result) throw std::runtime_error("Incomplete item record");
        return *result;
    }
private:
    void advance() { token_=game_config::bounded_name_token(text_,cursor_); }
    std::string_view text_;
    std::size_t cursor_=0;
    std::optional<std::string_view> token_;
};

inline std::string asset(std::string_view name) {
    return name.empty() ? std::string{} : resource::normalize_cp932_path("sys/"+std::string(name));
}
inline void add_description(std::vector<std::string>& lines,std::string_view text) {
    // The @ row is blank and still occupies a configured line position.
    if (lines.size()<max_description)
        lines.emplace_back(!text.empty() && text.front()=='@' ? std::string_view{} : text);
}
inline std::optional<Variable> read_variable(const ReadVariable& read,std::string_view name,bool sentinel=false) {
    if (!read || name.empty() || (sentinel && name.front()=='@')) return {};
    return read(name);
}
inline Value variable_value(const std::optional<Variable>& variable) noexcept {
    return variable ? variable->value : 0;
}
inline bool writable(const std::optional<Variable>& variable) noexcept {
    return variable && variable->id<32768U;
}

inline Layout layout(const game_config::NameListSetup& setup,bool shop) {
    Layout result;
    result.columns=setup.integer("itemBlockX",4);
    result.rows=setup.integer("itemBlockY",3);
    if (result.columns<=0 || result.rows<=0 || result.columns>256 || result.rows>256)
        throw std::runtime_error("Invalid item grid size");
    const auto coordinate=[&](std::string key,Value fallback) {
        const auto value=setup.integer(std::move(key),fallback);
        if (value< -1000000 || value>1000000) throw std::runtime_error("Item grid coordinate exceeds limit");
        return value;
    };
    result.first={coordinate("itemPrintX",100),coordinate("itemPrintY",100),
        coordinate("itemSizeX",80),coordinate("itemSizeY",60)};
    if (result.first.w<=0 || result.first.h<=0) throw std::runtime_error("Invalid item cell size");
    result.step_x=coordinate("itemPrintNextX",result.first.w);
    result.step_y=coordinate("itemPrintNextY",result.first.h);
    result.print_names=setup.integer("itemNamePrintFlag",0)!=0;
    result.low_cursor=setup.integer("lowCursor",0)!=0;
    result.print_plate_item=setup.integer("plateItemPrintFlag",1)!=0;
    result.no_item_asset=asset(setup.field("filenameNoItem").value_or(shop ? "ta_noshopitem" : "ta_noitem"));
    result.plate_asset=asset(setup.field("filenamePlate").value_or(shop ? "ta_shopitem_plate" : "ta_haveitem_plate"));
    if (setup.integer("useCursorPic",0)!=0)
        result.cursor_asset=asset(setup.field("filenameCursor").value_or(shop ? "ta_shopitem_cursor" : "ta_haveitem_cursor"));

    const auto category_count=setup.integer("itemTypeNumber",1);
    if (category_count<1 || category_count>256) throw std::runtime_error("Invalid item category count");
    result.categories.resize(static_cast<std::size_t>(category_count));
    for (Value group=0;group<category_count;++group) {
        const auto suffix=std::to_string(group+1);
        const auto count=std::clamp(setup.integer("itemTypeDataNumber"+suffix,1),Value{0},Value{256});
        auto& types=result.categories[static_cast<std::size_t>(group)];
        for (Value i=1;i<=count;++i)
            types.push_back(setup.integer("itemTypeData"+suffix+"_"+std::to_string(i),group+1));
        std::sort(types.begin(),types.end());
    }
    return result;
}
inline bool in_category(const Layout& layout,Value category,Value type) {
    const auto index=static_cast<std::size_t>(category);
    if (index>=layout.categories.size()) return false;
    const auto& types=layout.categories[index];
    return std::binary_search(types.begin(),types.end(),type);
}
template<class Entry>
std::vector<Value> category_ids(const std::vector<Entry>& entries,const Layout& layout,Value category) {
    std::vector<Value> ids;
    for (const auto& entry:entries)
        if (in_category(layout,category,entry.type)) ids.push_back(entry.id);
    return ids;
}
inline Value page_count(const Layout& layout,Value category,std::size_t count) noexcept {
    if (static_cast<std::size_t>(category)>=layout.categories.size()) return 0;
    const auto size=static_cast<Value>(count);
    return std::max(Value{1},(size+layout.per_page()-1)/layout.per_page());
}
inline Value signed_bits(Word bits) noexcept {
    return static_cast<Value>(std::int64_t{bits}-(bits>INT32_MAX ? 0x100000000LL : 0LL));
}

} // namespace detail

class ItemCatalog {
public:
    explicit ItemCatalog(const std::filesystem::path& root,const resource::FilenameEncoder& encode={}) {
        const auto config=game_config::find(root,"init","printItem.xtx",encode);
        if (config.empty()) return;
        const auto path=resource::find_file(resource::find_directory(root,"nya",encode),"haveItem.xtx",encode);
        if (path.empty()) return;
        const auto settings=game_config::read_text(config);
        const game_config::NameListSetup setup(settings);
        layout_=detail::layout(setup,false);
        suffix_=setup.field("suffix").value_or("_2");
        const auto data=game_config::read_text(path);
        detail::Records input(data);
        while (const auto serial=input.record()) {
            if (entries_.size()>=detail::max_records) throw std::runtime_error("Item catalogue exceeds record limit");
            ItemEntry entry;
            entry.id=static_cast<Value>(entries_.size());
            entry.serial=detail::required_number(*serial);
            entry.type=detail::required_number(input.required());
            entry.availability_var=input.required();
            entry.filename=input.required();
            entry.variants.push_back({std::string(input.required()),{}});
            while (const auto token=input.field()) {
                if (!token->empty() && token->front()=='!') {
                    if (entry.variants.size()>=detail::max_variants) throw std::runtime_error("Item has too many variants");
                    entry.variants.push_back({std::string(token->substr(1)),{}});
                } else {
                    detail::add_description(entry.variants.back().description,*token);
                }
            }
            entries_.push_back(std::move(entry));
        }
    }

    bool configured() const noexcept { return !entries_.empty(); }
    const Layout& setup() const noexcept { return layout_; }
    std::size_t category_count() const noexcept { return configured() ? layout_.categories.size() : 0U; }
    Value per_page() const noexcept { return layout_.per_page(); }
    const ItemEntry* entry(Value id) const noexcept {
        const auto index=static_cast<std::size_t>(id);
        return index<entries_.size() ? &entries_[index] : nullptr;
    }
    ItemPage page(Value category,Value page_index,const ReadVariable& read={}) const {
        ItemPage view;
        view.category=category;
        view.page=page_index;
        if (!configured()) return view;
        const auto ids=detail::category_ids(entries_,layout_,category);
        view.page_count=detail::page_count(layout_,category,ids.size());
        if (page_index<0 || page_index>=view.page_count) return view;
        const auto begin=page_index*per_page();
        const auto end=std::min(begin+per_page(),static_cast<Value>(ids.size()));
        for (Value position=begin;position<end;++position) {
            const auto& item=*entry(ids[static_cast<std::size_t>(position)]);
            const auto value=detail::variable_value(detail::read_variable(read,item.availability_var));
            const auto selected=value>=1 && static_cast<std::size_t>(value)<=item.variants.size() ? value : 1;
            const auto& variant=item.variants[static_cast<std::size_t>(selected-1)];
            const auto picture=value<=0 ? layout_.no_item_asset :
                detail::asset(value==1 ? item.filename : item.filename+suffix_);
            view.rows.push_back({item.id,item.serial,item.type,position,value,selected,layout_.cell(position),value>0,
                variant.name,picture,{variant.description.begin(),variant.description.end()}});
        }
        return view;
    }

private:
    Layout layout_;
    std::string suffix_;
    std::vector<ItemEntry> entries_;
};

class ShopCatalog {
public:
    // The displayed New marks use the cache from panel entry.
    struct Visit { std::vector<Byte> seen_by_record; };

    explicit ShopCatalog(const std::filesystem::path& root,const resource::FilenameEncoder& encode={}) {
        const auto config=game_config::find(root,"init","shop.xtx",encode);
        if (config.empty()) return;
        const auto settings=game_config::read_text(config);
        const game_config::NameListSetup setup(settings);
        const auto encrypted=setup.integer("angouFlag",1)!=0;
        const auto folder=resource::find_directory(root,encrypted ? "nya" : "nnndir",encode);
        const auto path=resource::find_file(folder,encrypted ? "shopItem.fxf" : "shopItem.txt",encode);
        if (path.empty()) return;
        setup_.layout=detail::layout(setup,true);
        setup_.can_select_limit=setup.integer("canSelectLimit",0)!=0;
        setup_.can_select_no_point=setup.integer("canSelectNoPoint",0)!=0;
        setup_.print_new=setup.integer("newMarkPrintFlag",0)!=0;
        setup_.print_sold_out=setup.integer("soldOutPrintFlag",0)!=0;
        setup_.point_var=setup.field("pointVarName").value_or("\x83\x51\x81\x5b\x83\x80\x93\xe0\x83\x7c\x83\x43\x83\x93\x83\x67");
        setup_.hidden_name=setup.field("notAppearName").value_or("ta_noshopitem");
        setup_.new_asset=detail::asset(setup.field("filenameNewMark").value_or(""));
        setup_.sold_out_asset=detail::asset(setup.field("filenameSoldOut").value_or(""));

        const auto data=game_config::read_text(path);
        detail::Records input(data);
        while (const auto serial=input.record()) {
            if (entries_.size()>=detail::max_records) throw std::runtime_error("Shop catalogue exceeds record limit");
            ShopEntry entry;
            entry.id=static_cast<Value>(entries_.size());
            entry.serial=detail::required_number(*serial);
            entry.type=detail::required_number(input.required());
            entry.price=detail::decimal(input.required());
            entry.limit=detail::required_number(input.required());
            entry.name=input.required();
            entry.filename=input.required();
            entry.appear_var=input.required();
            entry.target_var=input.required();
            while (const auto line=input.field()) detail::add_description(entry.description,*line);
            entries_.push_back(std::move(entry));
        }
        const auto seen_count=(entries_.size()+31U)/32U;
        setup_.seen_vars.resize(seen_count);
        for (std::size_t i=0;i<seen_count;++i)
            setup_.seen_vars[i]=setup.field("appearVar"+std::to_string(i+1U)).value_or("");
    }

    bool configured() const noexcept { return !entries_.empty(); }
    const ShopSetup& setup() const noexcept { return setup_; }
    std::size_t category_count() const noexcept { return configured() ? setup_.layout.categories.size() : 0U; }
    Value per_page() const noexcept { return setup_.layout.per_page(); }
    const ShopEntry* entry(Value id) const noexcept {
        const auto index=static_cast<std::size_t>(id);
        return index<entries_.size() ? &entries_[index] : nullptr;
    }
    Visit begin(const ReadVariable& read={}) const {
        Visit visit{std::vector<Byte>(entries_.size(),1)};
        for (std::size_t i=0;i<setup_.seen_vars.size();++i) {
            const auto variable=detail::read_variable(read,setup_.seen_vars[i]);
            if (!variable) continue;
            const auto count=std::min(std::size_t{32},entries_.size()-i*32U);
            for (std::size_t bit=0;bit<count;++bit)
                visit.seen_by_record[i*32U+bit]=static_cast<Byte>((static_cast<Word>(variable->value)>>bit)&1U);
        }
        return visit;
    }

    ShopPage page(Value category,Value page_index,const Visit& visit,const ReadVariable& read={}) const {
        ShopPage view;
        view.category=category;
        view.page=page_index;
        if (!configured()) return view;
        const auto& layout=setup_.layout;
        const auto balance=detail::read_variable(read,setup_.point_var);
        view.balance=detail::variable_value(balance);
        const auto ids=detail::category_ids(entries_,layout,category);
        view.page_count=detail::page_count(layout,category,ids.size());
        if (page_index<0 || page_index>=view.page_count) return view;
        const auto begin=page_index*per_page();
        const auto end=std::min(begin+per_page(),static_cast<Value>(ids.size()));
        for (Value position=begin;position<end;++position) {
            const auto& item=*entry(ids[static_cast<std::size_t>(position)]);
            const auto status=state(item,balance,read);
            const auto index=static_cast<std::size_t>(item.id);
            const bool new_mark=index<visit.seen_by_record.size() && visit.seen_by_record[index]==0;
            const auto picture=status.appearance>0 ? detail::asset(item.filename) : layout.no_item_asset;
            view.rows.push_back({item.id,item.serial,item.type,position,status.appearance,
                detail::variable_value(status.target),item.limit,item.price,layout.cell(position),status.sold_out,new_mark,
                status.detail_selectable,status.balance_after.has_value(),status.appearance==0 ? setup_.hidden_name : item.name,picture,
                {item.description.begin(),item.description.end()}});
        }
        return view;
    }

    std::optional<VariableWrite> plan_seen(Value category,Value id,const ReadVariable& read) const {
        const auto* item=entry(id);
        if (!item || !detail::in_category(setup_.layout,category,item->type)) return {};
        const auto balance=detail::read_variable(read,setup_.point_var);
        if (!state(*item,balance,read).detail_selectable) return {};
        const auto ids=detail::category_ids(entries_,setup_.layout,category);
        const auto found=std::find(ids.begin(),ids.end(),id);
        if (found==ids.end()) return {};
        const auto position=static_cast<std::size_t>(found-ids.begin());
        // CCommonShop passes the position in the current filtered table.
        const auto flag=detail::read_variable(read,setup_.seen_vars[position/32U]);
        if (!detail::writable(flag)) return {};
        const auto bits=static_cast<Word>(flag->value);
        const auto next=bits|(Word{1}<<(position%32U));
        if (next==bits) return {};
        return VariableWrite{flag->id,flag->value,detail::signed_bits(next)};
    }

    std::optional<Purchase> plan_purchase(Value category,Value id,const ReadVariable& read) const {
        const auto* item=entry(id);
        if (!item || !detail::in_category(setup_.layout,category,item->type)) return {};
        const auto balance=detail::read_variable(read,setup_.point_var);
        const auto status=state(*item,balance,read);
        if (!status.balance_after) return {};
        const auto& target=*status.target;
        if (target.id==balance->id) return Purchase{id,{{target.id,target.value,*status.balance_after}}};
        return Purchase{id,{{target.id,target.value,target.value+1},{balance->id,balance->value,*status.balance_after}}};
    }

private:
    struct State {
        Value appearance=0;
        std::optional<Variable> target;
        bool sold_out=false,detail_selectable=false;
        std::optional<Value> balance_after;
    };
    State state(const ShopEntry& item,const std::optional<Variable>& balance,const ReadVariable& read) const {
        State result;
        result.appearance=detail::variable_value(detail::read_variable(read,item.appear_var,true));
        result.target=detail::read_variable(read,item.target_var,true);
        const auto owned=detail::variable_value(result.target);
        result.sold_out=result.target && item.limit>0 && owned>=item.limit;
        const bool affordable=item.price && detail::variable_value(balance)>=*item.price;
        result.detail_selectable=result.appearance>0 && (!result.sold_out || setup_.can_select_limit) &&
            (!item.price || affordable || setup_.can_select_no_point);
        if (result.appearance>0 && affordable && !result.sold_out && detail::writable(balance) &&
            detail::writable(result.target) && owned<std::numeric_limits<Value>::max()) {
            const bool alias=balance->id==result.target->id;
            // The PC reloads the balance after incrementing the item quantity.
            if (!alias || balance->value==owned) {
                const auto points=std::int64_t{balance->value}+(alias ? 1 : 0)-*item.price;
                if (points<=std::numeric_limits<Value>::max()) result.balance_after=static_cast<Value>(points);
            }
        }
        return result;
    }

    ShopSetup setup_;
    std::vector<ShopEntry> entries_;
};

} // namespace nnn::inventory
