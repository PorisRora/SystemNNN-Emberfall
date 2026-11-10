#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace nnn::snapshot {
inline constexpr std::size_t max_bytes=48U*1024U*1024U;
using Bytes=std::vector<std::uint8_t>;
[[noreturn]] inline void invalid(const char* message) { throw std::runtime_error(message); }

template<class T> using Bits=std::conditional_t<sizeof(T)==8,std::uint64_t,
    std::conditional_t<sizeof(T)==4,std::uint32_t,std::conditional_t<sizeof(T)==2,std::uint16_t,std::uint8_t>>>;

class Writer {
public:
    template<class... T> void operator()(const T&... fields) { (value(fields),...); }
    void raw(const void* data,std::size_t length) {
        if (length>max_bytes-bytes.size()) invalid("Snapshot exceeds capacity");
        if (!length) return;
        const auto* start=static_cast<const std::uint8_t*>(data);
        bytes.insert(bytes.end(),start,start+length);
    }
    void size(std::size_t n,std::size_t maximum) {
        if (n>maximum) invalid("Snapshot field exceeds capacity");
        value(static_cast<std::uint32_t>(n));
    }
    template<class T,class F> void records(const std::vector<T>& list,std::size_t maximum,F entry) {
        size(list.size(),maximum);
        for (const auto& item:list) entry(*this,item);
    }
    Bytes bytes;
private:
    void value(bool v) { value(static_cast<std::uint8_t>(v)); }
    template<class T,std::enable_if_t<std::is_arithmetic_v<T> && !std::is_same_v<T,bool>,int> =0>
    void value(T v) {
        Bits<T> bits{};
        std::memcpy(&bits,&v,sizeof(v));
        std::uint8_t encoded[sizeof(T)];
        for (std::size_t i=0;i<sizeof(T);++i) encoded[i]=static_cast<std::uint8_t>(bits>>(8*i));
        raw(encoded,sizeof(T));
    }
    void value(const std::string& s) { size(s.size(),1024U*1024U); raw(s.data(),s.size()); }
    void value(const Bytes& b) { size(b.size(),max_bytes); raw(b.data(),b.size()); }
    template<class T,std::size_t N> void value(const std::array<T,N>& a) { for (const auto& item:a) value(item); }
    template<class T> void value(const std::vector<T>& v) {
        records(v,65536,[](Writer& out,const T& item){out.value(item);});
    }
};

class Reader {
public:
    explicit Reader(const Bytes& bytes):Reader(bytes.data(),bytes.size()) {}
    Reader(const std::uint8_t* data,std::size_t length):data_(data),length_(length) {
        if (length>max_bytes || (!data && length)) invalid("Invalid snapshot buffer");
    }
    template<class... T> void operator()(T&... fields) { (value(fields),...); }
    const std::uint8_t* raw(std::size_t length) {
        if (length>remaining()) invalid("Truncated snapshot");
        const auto* start=position_ ? data_+position_ : data_;position_+=length;return start;
    }
    std::size_t remaining() const { return length_-position_; }
    std::size_t size(std::size_t maximum) {
        std::uint32_t n=0;value(n);
        if (n>maximum || n>remaining()) invalid("Invalid snapshot field length");
        return n;
    }
    template<class T,class F> void records(std::vector<T>& list,std::size_t maximum,F entry) {
        list.resize(size(maximum));
        for (auto& item:list) entry(*this,item);
    }
private:
    void value(bool& v) {
        std::uint8_t flag=0;value(flag);
        if (flag>1) invalid("Invalid snapshot flag");
        v=flag!=0;
    }
    template<class T,std::enable_if_t<std::is_arithmetic_v<T> && !std::is_same_v<T,bool>,int> =0>
    void value(T& v) {
        const auto* encoded=raw(sizeof(T));
        std::uint64_t wide=0;
        for (std::size_t i=0;i<sizeof(T);++i) wide|=std::uint64_t{encoded[i]}<<(8*i);
        const auto bits=static_cast<Bits<T>>(wide);
        std::memcpy(&v,&bits,sizeof(v));
        if constexpr (std::is_floating_point_v<T>) if (!std::isfinite(v)) invalid("Invalid snapshot number");
    }
    void value(std::string& s) { const auto n=size(1024U*1024U);s.assign(reinterpret_cast<const char*>(raw(n)),n); }
    void value(Bytes& b) { const auto n=size(max_bytes);const auto* start=raw(n);b.assign(start,start+n); }
    template<class T,std::size_t N> void value(std::array<T,N>& a) { for (auto& item:a) value(item); }
    template<class T> void value(std::vector<T>& v) {
        records(v,65536,[](Reader& in,T& item){in.value(item);});
    }
    const std::uint8_t* data_;
    std::size_t length_;
    std::size_t position_=0;
};
} // namespace nnn::snapshot
