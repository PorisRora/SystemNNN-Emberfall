#pragma once

#include "nnn/chart_graph.hpp"
#include "nnn/snapshot.hpp"
#include "pc_save.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace emberfall::scene {

struct ChartNodeSave {
    pc::Status status;
    nnn::chart::SavedVariables vars{};
    nnn::chart::RunState run;
};

namespace chart_node_detail {

[[noreturn]] inline void invalid(const char* reason) {
    throw std::runtime_error(std::string("PC chart node: ")+reason);
}

// Each section starts with its compressed byte length. Literal widths match
// the destination array, including an unsigned one-byte move-road run count.
template<class Range>
void write_section(nnn::snapshot::Writer& out,const Range& values,typename Range::value_type marker) {
    using Count=std::make_unsigned_t<typename Range::value_type>;
    nnn::snapshot::Writer compressed;
    std::size_t count=0;
    const auto flush=[&] {
        if (count==0) return;
        compressed(marker,static_cast<Count>(count));
        count=0;
    };
    for (const auto value:values) {
        if (value==marker) {
            ++count;
            if (count==std::numeric_limits<Count>::max()) flush();
        } else {
            flush();
            compressed(value);
        }
    }
    flush();
    out(static_cast<std::uint32_t>(compressed.bytes.size()));
    out.raw(compressed.bytes.data(),compressed.bytes.size());
}

template<class Range>
void read_section(nnn::snapshot::Reader& in,Range& values,typename Range::value_type marker) {
    using T=typename Range::value_type;
    using Count=std::make_unsigned_t<T>;
    std::uint32_t length=0;
    in(length);
    if (length%sizeof(T)!=0 || length>in.remaining() || length>2U*values.size()*sizeof(T))
        invalid("invalid compressed extent");
    std::size_t remaining=length/sizeof(T),offset=0;
    while (remaining>0) {
        T value{};
        in(value);
        --remaining;
        Count count=1;
        if (value==marker) {
            if (remaining==0) invalid("missing run count");
            in(count);
            --remaining;
        }
        if (count==0 || count>values.size()-offset) invalid("invalid decoded run");
        std::fill_n(values.data()+offset,count,value);
        offset+=count;
    }
    if (offset!=values.size()) invalid("incomplete decoded section");
}

} // namespace chart_node_detail

// Original chart<N>/slot<M>.dat bytes. The graph owns the padded array sizes.
// The actor applies chart::restore_variables only after a complete parse.
inline ChartNodeSave parse_chart_node(const std::vector<nnn::Byte>& bytes,const nnn::chart::Graph& graph) {
    if (!graph.configured()) chart_node_detail::invalid("missing chart graph");
    nnn::snapshot::Reader in(bytes);
    std::uint32_t length=0;
    in(length);
    if (length<sizeof(length) || length>bytes.size()) chart_node_detail::invalid("invalid status extent");

    ChartNodeSave node;
    node.status=pc::parse_status_block(bytes.data(),length);
    in.raw(length-sizeof(length));
    node.run=graph.run_defaults();
    chart_node_detail::read_section(in,node.vars,nnn::Value{0});
    chart_node_detail::read_section(in,node.run.move_road,nnn::Byte{0});
    chart_node_detail::read_section(in,node.run.can_select,std::numeric_limits<nnn::Word>::max());
    if (in.remaining()!=0) chart_node_detail::invalid("trailing bytes");
    return node;
}

inline std::vector<nnn::Byte> write_chart_node(const ChartNodeSave& node,const nnn::chart::Graph& graph) {
    if (!graph.configured() || !graph.valid(node.run)) chart_node_detail::invalid("invalid chart state extent");
    nnn::snapshot::Writer out;
    out.bytes=pc::write_status_block(node.status);
    chart_node_detail::write_section(out,node.vars,nnn::Value{0});
    chart_node_detail::write_section(out,node.run.move_road,nnn::Byte{0});
    chart_node_detail::write_section(out,node.run.can_select,std::numeric_limits<nnn::Word>::max());
    return std::move(out.bytes);
}

} // namespace emberfall::scene
