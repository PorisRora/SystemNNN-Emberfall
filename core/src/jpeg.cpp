#include "nnn/image.hpp"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <stdexcept>
#include <utility>

namespace {

// Covers decoder work and its temporary RGB buffer. The returned vector
// has its own pixel bound and is allocated after the decoder frees its work.
constexpr std::size_t max_jpeg_work_bytes=128U*1024U*1024U;

struct JpegBudget {
    std::size_t used=0;
    bool exhausted=false;
};

thread_local JpegBudget* jpeg_budget=nullptr;

class JpegAllocationScope {
public:
    JpegAllocationScope(): previous_(jpeg_budget) { jpeg_budget=&budget; }
    ~JpegAllocationScope() { jpeg_budget=previous_; }
    JpegAllocationScope(const JpegAllocationScope&)=delete;
    JpegAllocationScope& operator=(const JpegAllocationScope&)=delete;
    JpegBudget budget;
private:
    JpegBudget* previous_;
};

union AllocationHeader {
    std::size_t charge;
    std::max_align_t alignment;
};

void* jpeg_reallocate(void* pointer,std::size_t size) noexcept {
    auto* old=pointer?static_cast<AllocationHeader*>(pointer)-1:nullptr;
    const auto old_charge=old?old->charge:0U;
    auto& budget=*jpeg_budget;
    const auto current=budget.used-old_charge;
    const auto capacity=max_jpeg_work_bytes-sizeof(AllocationHeader);
    if (current>capacity || size>capacity-current) {
        budget.exhausted=true;
        return nullptr;
    }
    const auto charge=size+sizeof(AllocationHeader);
    auto* header=static_cast<AllocationHeader*>(std::realloc(old,charge));
    if (!header) {
        budget.exhausted=true;
        return nullptr;
    }
    header->charge=charge;
    budget.used=current+charge;
    return header+1;
}

void jpeg_release(void* pointer) noexcept {
    if (!pointer) return;
    auto* header=static_cast<AllocationHeader*>(pointer)-1;
    jpeg_budget->used-=header->charge;
    std::free(header);
}

} // namespace

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_SIMD
#define STBI_MAX_DIMENSIONS 8192
#define STBI_MALLOC(size) jpeg_reallocate(nullptr,size)
#define STBI_REALLOC_SIZED(pointer,old_size,new_size) jpeg_reallocate(pointer,new_size)
#define STBI_FREE(pointer) jpeg_release(pointer)
#include <stb_image.h>

namespace nnn::image {
namespace {

constexpr std::size_t max_markers=65536U;
constexpr std::size_t max_scans=128U;
static_assert(max_input_bytes<=static_cast<std::size_t>(std::numeric_limits<int>::max()));
static_assert(max_dimension==STBI_MAX_DIMENSIONS);

struct JpegFrame {
    std::uint32_t width=0,height=0;
    Byte components=0;
    std::size_t first_scan=no_byte;
};

unsigned word(const Byte* data) {
    return (static_cast<unsigned>(data[0])<<8U)|data[1];
}

Status check_stream(const Byte* bytes,std::size_t length,JpegFrame& frame) {
    if (length>max_input_bytes) return {ErrorCode::LimitExceeded,no_byte,"JPEG payload exceeds byte limit"};
    if (!bytes || length<2U) return {ErrorCode::InvalidFormat,length,"Truncated JPEG signature"};
    if (bytes[0]!=0xffU || bytes[1]!=0xd8U) return {ErrorCode::UnsupportedFormat,0U,"Missing JPEG SOI signature"};

    std::size_t position=2U,markers=0U,scans=0U,scan_start=0U;
    bool scanning=false;
    while (position<length) {
        if (scanning) {
            const auto* next=static_cast<const Byte*>(std::memchr(bytes+position,0xff,length-position));
            if (!next) break;
            position=static_cast<std::size_t>(next-bytes);
        }
        const auto offset=position;
        if (bytes[position++]!=0xffU) return {ErrorCode::InvalidFormat,offset,"Expected JPEG marker"};
        while (position<length && bytes[position]==0xffU) ++position;
        if (position==length) break;
        const auto marker=bytes[position++];
        const bool restart=marker>=0xd0U && marker<=0xd7U;
        if (scanning && (marker==0U || restart)) continue;
        if (scanning && offset==scan_start) return {ErrorCode::InvalidFormat,offset,"Empty JPEG scan"};
        scanning=false;
        if (++markers>max_markers) return {ErrorCode::LimitExceeded,offset,"JPEG marker count exceeds limit"};
        if (marker==0xd9U) {
            if (!scans || position!=length) return {ErrorCode::InvalidFormat,offset,"EOI must follow JPEG scans and end the payload"};
            return {};
        }
        if (marker==0U || marker==0x01U || marker==0xd8U || restart)
            return {ErrorCode::InvalidFormat,offset,"Unexpected standalone JPEG marker"};
        if (length-position<2U) return {ErrorCode::InvalidFormat,position,"Truncated JPEG segment length"};
        const auto segment=word(bytes+position);
        if (segment<2U || segment>length-position)
            return {ErrorCode::InvalidFormat,position,"JPEG segment exceeds its bounded payload"};
        const auto data=position+2U;
        position+=segment;
        const bool frame_marker=marker>=0xc0U && marker<=0xcfU &&
                                marker!=0xc4U && marker!=0xc8U && marker!=0xccU;
        if (frame_marker) {
            if (marker>0xc2U) return {ErrorCode::UnsupportedFormat,offset,"Unsupported JPEG coding process"};
            if (frame.width || segment<8U) return {ErrorCode::InvalidFormat,offset,"Invalid or duplicate JPEG frame header"};
            if (bytes[data]!=8U) return {ErrorCode::UnsupportedFormat,data,"JPEG sample precision must be eight bits"};
            frame.height=word(bytes+data+1U);
            frame.width=word(bytes+data+3U);
            frame.components=bytes[data+5U];
            if (!frame.width || !frame.height || segment!=8U+3U*frame.components)
                return {ErrorCode::InvalidFormat,data,"Invalid JPEG frame dimensions or component layout"};
            if (frame.components!=1U && frame.components!=3U && frame.components!=4U)
                return {ErrorCode::UnsupportedFormat,data+5U,"Unsupported JPEG component count"};
            if (frame.width>max_dimension || frame.height>max_dimension ||
                static_cast<std::size_t>(frame.width)*frame.height>max_pixels)
                return {ErrorCode::LimitExceeded,data,"JPEG canvas exceeds resource limit"};
        } else if (marker==0xdaU) {
            if (!frame.width || segment<6U || bytes[data]==0U || bytes[data]>frame.components ||
                segment!=6U+2U*bytes[data]) return {ErrorCode::InvalidFormat,offset,"Invalid JPEG scan header"};
            if (++scans>max_scans) return {ErrorCode::LimitExceeded,offset,"JPEG scan count exceeds limit"};
            if (scans==1U) frame.first_scan=position;
            scanning=true;
            scan_start=position;
        }
    }
    return {ErrorCode::InvalidFormat,length,"JPEG payload has no complete EOI marker"};
}

} // namespace

ImageResult decode_jpeg(const Byte* bytes,std::size_t length) {
    try {
        JpegFrame frame;
        const auto status=check_stream(bytes,length,frame);
        if (!status) return {status,{}};

        JpegAllocationScope allocations;
        int width=0,height=0;
        const std::unique_ptr<stbi_uc,decltype(&stbi_image_free)> decoded(
            stbi_load_from_memory(bytes,static_cast<int>(length),&width,&height,nullptr,STBI_rgb),stbi_image_free);
        if (!decoded) {
            if (allocations.budget.exhausted)
                return {{ErrorCode::LimitExceeded,frame.first_scan,"JPEG decoder exceeds work allocation limit"},{}};
            const auto* reason=stbi_failure_reason();
            return {{ErrorCode::InvalidFormat,frame.first_scan,reason?reason:"JPEG decode failed"},{}};
        }
        if (width!=static_cast<int>(frame.width) || height!=static_cast<int>(frame.height))
            return {{ErrorCode::InvalidFormat,frame.first_scan,"JPEG decoder disagrees with checked frame dimensions"},{}};

        Image image;
        image.width=frame.width;
        image.height=frame.height;
        image.source_bit_depth=static_cast<std::uint16_t>(frame.components*8U);
        const auto count=static_cast<std::size_t>(image.width)*image.height*3U;
        image.rgb.assign(decoded.get(),decoded.get()+count);
        return {{},std::move(image)};
    } catch (const std::bad_alloc&) {
        return {{ErrorCode::LimitExceeded,no_byte,"Cannot allocate bounded JPEG output"},{}};
    } catch (const std::length_error&) {
        return {{ErrorCode::LimitExceeded,no_byte,"JPEG output exceeds storage limit"},{}};
    }
}

} // namespace nnn::image
