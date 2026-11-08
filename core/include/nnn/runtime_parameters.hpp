#pragma once

#include "nnn/script.hpp"

#include <array>
#include <limits>

namespace nnn {

// Numeric identities from nnnLib/commonSystemParamName.h. A declared identity
// does not imply that the upstream callback or this store implements it.
inline constexpr Word NNNPARAM_BGMMODE = 0;
inline constexpr Word NNNPARAM_SKIPMODE = 2;
inline constexpr Word NNNPARAM_SCREENMODE = 4;
inline constexpr Word NNNPARAM_AUTOMODE = 6;
inline constexpr Word NNNPARAM_USERDEFINEMODE = 50;
inline constexpr Word NNNPARAM_MUSICVOLUME = 100;
inline constexpr Word NNNPARAM_SOUNDVOLUME = 101;
inline constexpr Word NNNPARAM_VOICEVOLUME = 102;
inline constexpr Word NNNPARAM_MOVIEVOLUME = 103;
inline constexpr Word NNNPARAM_SOUNDVOICEVOLUME = 104;
inline constexpr Word NNNPARAM_MESSAGESPEED = 105;
inline constexpr Word NNNPARAM_WINDOWPERCENT = 106;
inline constexpr Word NNNPARAM_AUTOSPEEDSLIDER = 107;
inline constexpr Word NNNPARAM_TOTALVOLUME = 108;
inline constexpr Word NNNPARAM_SCRIPTSEVOLUME = 109;
inline constexpr Word NNNPARAM_USERDEFINEVOLUME = 150;
inline constexpr Word NNNPARAM_MUSICSWITCH = 200;
inline constexpr Word NNNPARAM_SOUNDSWITCH = 201;
inline constexpr Word NNNPARAM_VOICESWITCH = 202;
inline constexpr Word NNNPARAM_MOVIESWITCH = 203;
inline constexpr Word NNNPARAM_SOUNDVOICESWITCH = 204;
inline constexpr Word NNNPARAM_TOTALSWITCH = 208;
inline constexpr Word NNNPARAM_SCRIPTSESWITCH = 209;
inline constexpr Word NNNPARAM_AUTOCONTINUESWITCH = 210;
inline constexpr Word NNNPARAM_AUTOSKIPSWITCH = 211;
inline constexpr Word NNNPARAM_CHANGEREADMESSAGECOLORSWITCH = 212;
inline constexpr Word NNNPARAM_CHANGESELECTMESSAGECOLORSWITCH = 213;
inline constexpr Word NNNPARAM_CONTINUEVOICESWITCH = 214;
inline constexpr Word NNNPARAM_USERDEFINESWITCH = 250;
inline constexpr Word NNNPARAM_EXPMODE = 300;
inline constexpr Word NNNPARAM_EXPCHECK = 400;
inline constexpr Word NNNPARAM_CHARAVOICEVOLUME = 500;
inline constexpr Word runtime_extended_parameter_count = 16;
inline constexpr std::size_t runtime_character_volume_count = 12U * 6U;

inline constexpr Value NNNSKIPMODE_NORMAL = 0;
inline constexpr Value NNNSKIPMODE_ALL = 1;
inline constexpr Value NNNSCREENMODE_NORMAL = 0;
inline constexpr Value NNNSCREENMODE_FULL = 1;
inline constexpr Value NNNAUTOMODE_OFF = 0;
inline constexpr Value NNNAUTOMODE_ON = 1;

struct RuntimeParameterDescriptor {
    Word id = 0;
    Value source_default = 0;
    Value storage_minimum = std::numeric_limits<Value>::min();
    Value storage_maximum = std::numeric_limits<Value>::max();
    Value ui_minimum = 0;
    Value ui_maximum = 1;
    bool ui_maximum_from_config = false;
};

// A value store only: it performs no audio, rendering, persistence or platform
// side effects. Game/session code owns those effects and one shared instance.
// Defaults below are exactly CCommonSystemFile::CreateInitData. In particular,
// autoSpeedSlider starts at 50 and the zeroed SYSTEMFLAG2 script-SE switch is 0.
// A shipped sav/sysfile*.org or a user system save can override these defaults;
// they are not equivalent to a particular game's first-launch settings.
//
// Save/read flags, voice-off groups and the system voice theme have separate owners.
class RuntimeParameters {
public:
    RuntimeParameters() noexcept { reset(); }
    bool operator==(const RuntimeParameters& other) const noexcept { return values_==other.values_; }

    static bool contains(Word id) noexcept {
        return id == NNNPARAM_SKIPMODE || id == NNNPARAM_SCREENMODE || id == NNNPARAM_AUTOMODE ||
            (id >= NNNPARAM_MUSICVOLUME && id <= NNNPARAM_SCRIPTSEVOLUME) ||
            (id >= NNNPARAM_MUSICSWITCH && id <= NNNPARAM_SOUNDVOICESWITCH) ||
            (id >= NNNPARAM_TOTALSWITCH && id <= NNNPARAM_CONTINUEVOICESWITCH) ||
            (id >= NNNPARAM_EXPMODE && id-NNNPARAM_EXPMODE < runtime_extended_parameter_count) ||
            (id >= NNNPARAM_EXPCHECK && id-NNNPARAM_EXPCHECK < runtime_extended_parameter_count) ||
            (id >= NNNPARAM_CHARAVOICEVOLUME &&
                id - NNNPARAM_CHARAVOICEVOLUME < runtime_character_volume_count);
    }

    Value get(Word id) const noexcept {
        return contains(id) ? values_[id] : 0;
    }
    bool get(Word id, Value& out) const noexcept {
        if (!contains(id)) return false;
        out = values_[id];
        return true;
    }
    // Like the source callback, ordinary fields retain the supplied signed
    // 32-bit value. UI ranges are descriptive, not a lossy storage conversion.
    // Character adjustment slots reproduce the source signed five-bit packing.
    bool set(Word id, Value value) noexcept {
        if (!contains(id)) return false;
        if (id >= NNNPARAM_CHARAVOICEVOLUME) {
            const auto bits = static_cast<Word>(value) & 31U;
            value = bits < 16U ? static_cast<Value>(bits) : static_cast<Value>(bits) - 32;
        }
        values_[id] = value;
        return true;
    }
    void reset() noexcept {
        values_.fill(0);
        for (Word id = NNNPARAM_MUSICVOLUME; id <= NNNPARAM_SOUNDVOICEVOLUME; ++id) values_[id] = 90;
        values_[NNNPARAM_TOTALVOLUME] = 90;
        values_[NNNPARAM_MESSAGESPEED] = 2;
        values_[NNNPARAM_AUTOSPEEDSLIDER] = 50;
        for (Word id = NNNPARAM_MUSICSWITCH; id <= NNNPARAM_SOUNDVOICESWITCH; ++id) values_[id] = 1;
        values_[NNNPARAM_TOTALSWITCH] = 1;
    }

    static bool describe(Word id, RuntimeParameterDescriptor& out) noexcept {
        if (!contains(id)) return false;
        RuntimeParameterDescriptor info;
        info.id = id;
        if (id >= NNNPARAM_CHARAVOICEVOLUME) {
            info.storage_minimum = -16;
            info.storage_maximum = 15;
            info.ui_minimum = -15;
            info.ui_maximum = 15;
            info.ui_maximum_from_config = true;
        } else if (id >= NNNPARAM_MUSICVOLUME && id <= NNNPARAM_SCRIPTSEVOLUME) {
            info.ui_maximum = 100;
            if (id <= NNNPARAM_SOUNDVOICEVOLUME || id == NNNPARAM_TOTALVOLUME) info.source_default = 90;
            if (id == NNNPARAM_MESSAGESPEED || id == NNNPARAM_AUTOSPEEDSLIDER) {
                info.source_default = id == NNNPARAM_MESSAGESPEED ? 2 : 50;
                info.ui_maximum = 4; // commonConfig's default *VolumeDevide.
                info.ui_maximum_from_config = true;
            }
        } else if (id >= NNNPARAM_EXPMODE && id-NNNPARAM_EXPMODE < runtime_extended_parameter_count) {
            info.ui_maximum_from_config = true;
        } else if ((id >= NNNPARAM_MUSICSWITCH && id <= NNNPARAM_SOUNDVOICESWITCH) || id == NNNPARAM_TOTALSWITCH) {
            info.source_default = 1;
        }
        out = info;
        return true;
    }

private:
    std::array<Value, NNNPARAM_CHARAVOICEVOLUME + runtime_character_volume_count> values_{};
};

} // namespace nnn
