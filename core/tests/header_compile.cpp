#include "nnn/script.hpp"

static_assert(sizeof(nnn::Word) == 4);
static_assert(sizeof(nnn::Value) == 4);
static_assert(sizeof(nnn::WordIndex) == 4);
static_assert(sizeof(void*) >= 4);

int public_header_contract() {
    const nnn::DecodeParameters parameters;
    const nnn::Status status;
    return parameters.xor_key == UINT32_MAX && static_cast<bool>(status) ? 0 : 1;
}
