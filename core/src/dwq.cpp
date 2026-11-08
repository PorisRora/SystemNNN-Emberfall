#include "nnn/dwq.hpp"
#include "nnn/png.hpp"

#include <algorithm>
#include <new>
#include <optional>
#include <stdexcept>
#include <utility>

namespace nnn::dwq {
namespace {

using image::Byte;
using image::ErrorCode;
using Bytes=std::vector<Byte>;
constexpr std::size_t dwq_header=64;
constexpr std::size_t bmp_header=54;

struct Failure {
    ErrorCode code;
    std::size_t offset;
    const char* detail;
};

[[noreturn]] void invalid(std::size_t offset,const char* detail) {
    throw Failure{ErrorCode::InvalidFormat,offset,detail};
}

std::uint16_t le16(const Bytes& bytes,std::size_t at) {
    return static_cast<std::uint16_t>(bytes[at] | (static_cast<unsigned>(bytes[at+1])<<8U));
}

std::uint32_t le32(const Bytes& bytes,std::size_t at) {
    return le16(bytes,at) | (static_cast<std::uint32_t>(le16(bytes,at+2))<<16U);
}

std::uint32_t magnitude(std::uint32_t n) {
    return n&0x80000000U ? 0U-n : n;
}

struct Extent {
    std::uint32_t width,height;
};

Extent dimensions(const Bytes& bytes) {
    const auto width=le32(bytes,36);
    const auto height=magnitude(le32(bytes,40));
    if(width==0 || width>0x7fffffffU || height==0)invalid(36,"Invalid DWQ dimensions");
    if(width>image::max_dimension || height>image::max_dimension ||
        std::uint64_t{width}*height>image::max_pixels)
        throw Failure{ErrorCode::LimitExceeded,36,"DWQ dimensions exceed resource limits"};
    return {width,height};
}

Margins stored_margins(const Bytes& bytes,Extent size) {
    const Margins cut{le32(bytes,16),le32(bytes,20),le32(bytes,24),le32(bytes,28)};
    if(std::uint64_t{cut.left}+cut.right>=size.width || std::uint64_t{cut.top}+cut.bottom>=size.height)
        invalid(16,"DWQ margins leave no visible canvas");
    return cut;
}

Margins mask_margins(const Image& image) {
    std::uint32_t first_x=image.width,first_y=image.height,last_x=0;
    for(std::uint32_t y=0;y<image.height;++y) {
        const auto* row=image.rgba.data()+std::size_t{y}*image.width*4U;
        for(std::uint32_t x=0;x<image.width;++x) {
            if(row[std::size_t{x}*4U+3U]==0)continue;
            first_x=std::min(first_x,x);last_x=std::max(last_x,x);first_y=std::min(first_y,y);
        }
    }
    if(first_x==image.width)return {};
    // CutDataByMask retains a transparent guard around the top and sides.
    // It leaves the bottom edge at the full bitmap height.
    auto end=std::min(image.width,last_x+2U);
    if(end<image.width-1U)++end;
    return {first_y>0 ? first_y-1U : 0U,first_x>0 ? first_x-1U : 0U,image.width-end,0U};
}

struct Bitmap {
    std::size_t palette,pixels,colors,row_stride;
    std::uint16_t depth;
};

Bitmap bitmap(const Bytes& bytes,std::size_t start,std::size_t end,Extent size,bool packed) {
    if(end-start<bmp_header)invalid(end,"Truncated embedded DWQ bitmap header");
    if(bytes[start]!='B' || bytes[start+1]!='M')invalid(start,"Invalid embedded DWQ bitmap signature");
    if(le32(bytes,start+14)!=40 || le16(bytes,start+26)!=1)
        invalid(start+14,"Unsupported embedded DWQ bitmap header");
    if(le32(bytes,start+18)!=size.width || magnitude(le32(bytes,start+22))!=size.height)
        invalid(start+18,"Embedded bitmap dimensions disagree with the DWQ canvas");
    const auto depth=le16(bytes,start+28);
    if(depth!=8 && (packed || depth!=24))invalid(start+28,"Invalid embedded DWQ bitmap bit depth");
    const auto compression=le32(bytes,start+30);
    if(compression!=0 && (!packed || compression!=1))
        invalid(start+30,"Unsupported compression in embedded DWQ bitmap");
    const auto used=le32(bytes,start+46),important=le32(bytes,start+50);
    std::size_t colors=0;
    if(depth==8) {
        if(used>256U)invalid(start+46,"Embedded DWQ palette exceeds 256 entries");
        colors=used==0 ? 256U : used;
        if(important>colors)invalid(start+50,"Important colors exceed the DWQ palette");
    } else if(used!=0 || important!=0) {
        invalid(start+46,"Truecolor DWQ bitmap declares a palette");
    }
    const auto offset=bmp_header+4U*colors;
    const auto declared_offset=le32(bytes,start+10);
    if(declared_offset!=offset && (!packed || declared_offset!=0))
        invalid(start+10,"Embedded DWQ pixels do not follow the contiguous palette");
    if(offset>end-start)invalid(end,"Truncated embedded DWQ palette");
    const auto stride=(std::size_t{size.width}*(depth/8U)+3U)&~std::size_t{3U};
    // The original reader derives uncompressed lengths from the padded rows.
    // Packed color and masks are bounded by their DWQ segment and row records.
    if(!packed) {
        const auto payload=stride*size.height;
        if(payload>end-start-offset)invalid(end,"Truncated uncompressed DWQ color rows");
    }
    return {start+bmp_header,start+offset,colors,stride,depth};
}

void put_rgb(Byte* dest,const Byte* rgb) {
    dest[0]=rgb[0];dest[1]=rgb[1];dest[2]=rgb[2];
}

void put_palette(Byte* dest,const Byte* color,bool mask) {
    if(mask)dest[3]=color[2];
    else {dest[0]=color[2];dest[1]=color[1];dest[2]=color[0];}
}

void uncompressed_color(const Bytes& bytes,Bitmap layout,Image& out) {
    for(std::size_t y=0;y<out.height;++y) {
        const auto start=layout.pixels+y*layout.row_stride;
        auto* dest=out.rgba.data()+y*out.width*4U;
        if(layout.depth==24) {
            // DWQ has already reordered BMP data into physical RGB/top-down rows.
            for(std::size_t x=0;x<out.width;++x)
                put_rgb(dest+x*4U,bytes.data()+start+x*3U);
        } else {
            for(std::size_t x=0;x<out.width;++x) {
                const auto index=bytes[start+x];
                if(index>=layout.colors)invalid(start+x,"DWQ color index exceeds its palette");
                put_palette(dest+x*4U,bytes.data()+layout.palette+std::size_t{index}*4U,false);
            }
        }
    }
}

void packed_bitmap(const Bytes& bytes,Bitmap layout,std::size_t end,Image& out,bool mask) {
    auto cursor=layout.pixels;
    std::vector<Byte> previous(out.width);
    for(std::size_t y=0;y<out.height;++y) {
        for(std::size_t x=0;x<out.width;) {
            if(cursor==end)invalid(cursor,"Truncated packed DWQ row");
            const auto token=bytes[cursor++];
            if(token==0) {
                if(cursor==end)invalid(cursor,"Truncated packed DWQ zero run");
                const auto count=bytes[cursor++];
                if(count==0 || count>out.width-x)invalid(cursor-1,"Packed DWQ zero run exceeds its row");
                // Zero XOR delta keeps the previous row's values.
                x+=count;
            } else {
                previous[x]^=token;
                if(previous[x]>=layout.colors)invalid(cursor-1,"Packed DWQ index exceeds its palette");
                ++x;
            }
        }
        auto* dest=out.rgba.data()+y*out.width*4U;
        for(std::size_t x=0;x<out.width;++x)
            put_palette(dest+x*4U,bytes.data()+layout.palette+std::size_t{previous[x]}*4U,mask);
    }
    if(cursor!=end)invalid(cursor,"Extra bytes after the packed DWQ bitmap");
}

Image rgb_canvas(const image::Image& rgb) {
    const auto pixels=std::size_t{rgb.width}*rgb.height;
    if(rgb.rgb.size()!=pixels*3U)invalid(image::no_byte,"Incomplete decoded DWQ color pixels");
    Image out{rgb.width,rgb.height,false,{},Bytes(pixels*4U,255U)};
    for(std::size_t src=0,dst=0;src<rgb.rgb.size();src+=3U,dst+=4U)
        put_rgb(out.rgba.data()+dst,rgb.rgb.data()+src);
    return out;
}

ImageResult png_canvas(const Bytes& bytes,bool cut) {
    auto decoded=png::decode_dwq_png(bytes,png::PayloadTail::LegacyDwq);
    if(!decoded.status) {
        const auto& status=decoded.status;
        ErrorCode code=ErrorCode::InvalidFormat;
        switch(status.code) {
        case png::ErrorCode::Ok: break;
        case png::ErrorCode::NotFound: code=ErrorCode::NotFound; break;
        case png::ErrorCode::ReadFailed: code=ErrorCode::ReadFailed; break;
        case png::ErrorCode::UnsupportedFormat: code=ErrorCode::UnsupportedFormat; break;
        case png::ErrorCode::InvalidFormat: break;
        case png::ErrorCode::LimitExceeded: code=ErrorCode::LimitExceeded; break;
        }
        return {{code,status.byte_offset,std::move(decoded.status.detail)},{}};
    }
    auto& src=decoded.image;
    const auto& m=src.margins;
    Image out{src.width,src.height,src.has_alpha,{m.top,m.left,m.right,m.bottom},std::move(src.rgba)};
    if(!cut && out.has_mask)out.margins=mask_margins(out);
    return {{},std::move(out)};
}

std::size_t declared_end(const Bytes& bytes) {
    const auto length=le32(bytes,32);
    if(length==0 || length>bytes.size()-dwq_header)invalid(32,"Invalid DWQ color payload length");
    return dwq_header+length;
}

ImageResult decode_container(const Bytes& bytes) {
    if(bytes.size()>image::max_input_bytes)
        throw Failure{ErrorCode::LimitExceeded,image::no_byte,"DWQ input exceeds byte limit"};
    if(bytes.size()<dwq_header)invalid(bytes.size(),"Truncated 64-byte DWQ header");
    if(!std::equal(bytes.data()+48,bytes.data()+57,"PACKTYPE="))
        throw Failure{ErrorCode::UnsupportedFormat,48,"Invalid DWQ format tag"};
    const auto type=bytes[57];
    if(type=='0') {
        auto rgb=image::decode_dwq(bytes,image::BitmapSizeFields::Advisory);
        if(!rgb.status)return {std::move(rgb.status),{}};
        return {{},rgb_canvas(rgb.image)};
    }
    const bool cut=bytes[58]=='A';
    if(bytes[58]!=' ' && bytes[58]!=0 && !cut)
        throw Failure{ErrorCode::UnsupportedFormat,58,"Unsupported DWQ format suffix"};
    if(type=='8')return png_canvas(bytes,cut);
    if(type!='2' && type!='3' && type!='5' && type!='7')
        throw Failure{ErrorCode::UnsupportedFormat,57,"Unsupported DWQ PACKTYPE"};
    if(type=='5' && cut)throw Failure{ErrorCode::UnsupportedFormat,58,"Unmasked DWQ JPEG has no cut suffix"};

    const auto size=dimensions(bytes);
    const auto margins=cut ? stored_margins(bytes,size) : Margins{};
    const bool jpeg=type=='5' || type=='7';
    const bool packed=type=='3';
    auto color_end=jpeg || packed ? declared_end(bytes) : bytes.size();
    Bitmap color_layout{};
    if(!jpeg) {
        color_layout=bitmap(bytes,dwq_header,color_end,size,packed);
        if(!packed)color_end=color_layout.pixels+color_layout.row_stride*size.height;
    }
    std::optional<Bitmap> mask;
    if(type!='5')mask=bitmap(bytes,color_end,bytes.size(),size,true);
    else if(color_end!=bytes.size())invalid(color_end,"Extra bytes after unmasked DWQ JPEG");

    Image out;
    if(jpeg) {
        auto color=image::decode_jpeg(bytes.data()+dwq_header,color_end-dwq_header);
        if(!color.status) {
            if(color.status.byte_offset!=image::no_byte)color.status.byte_offset+=dwq_header;
            return {std::move(color.status),{}};
        }
        if(color.image.width!=size.width || color.image.height!=size.height)
            invalid(36,"JPEG dimensions disagree with the DWQ canvas");
        out=rgb_canvas(color.image);
    } else {
        out={size.width,size.height,false,{},Bytes(std::size_t{size.width}*size.height*4U,255U)};
        if(packed)packed_bitmap(bytes,color_layout,color_end,out,false);
        else uncompressed_color(bytes,color_layout,out);
    }
    if(mask) {
        packed_bitmap(bytes,*mask,bytes.size(),out,true);
        out.has_mask=true;
        out.margins=cut ? margins : mask_margins(out);
    }
    return {{},std::move(out)};
}

} // namespace

ImageResult decode(const std::vector<image::Byte>& bytes) {
    try {
        return decode_container(bytes);
    } catch(const Failure& error) {
        return {{error.code,error.offset,error.detail},{}};
    } catch(const std::bad_alloc&) {
        return {{ErrorCode::LimitExceeded,image::no_byte,"Cannot allocate bounded DWQ canvas"},{}};
    } catch(const std::length_error&) {
        return {{ErrorCode::LimitExceeded,image::no_byte,"DWQ storage exceeds container limit"},{}};
    }
}

} // namespace nnn::dwq
