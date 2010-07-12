// Copyright (C) 2010  Internet Systems Consortium, Inc. ("ISC")
//
// Permission to use, copy, modify, and/or distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH
// REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
// AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT,
// INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
// LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
// OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
// PERFORMANCE OF THIS SOFTWARE.

// $Id$

#include <stdint.h>
#include <cassert>
#include <iterator>
#include <string>
#include <vector>

#include <boost/archive/iterators/base64_from_binary.hpp>
#include <boost/archive/iterators/binary_from_base64.hpp>
#include <boost/archive/iterators/transform_width.hpp>
#include <boost/math/common_factor.hpp>

#include <dns/util/base32hex_from_binary.h>
#include <dns/util/binary_from_base32hex.h>

#include <dns/util/base16_from_binary.h>
#include <dns/util/binary_from_base16.h>

#include <exceptions/exceptions.h>

#include <dns/util/base32hex.h>
#include <dns/util/base64.h>

using namespace std;
using namespace boost::archive::iterators;

namespace isc {
namespace dns {

// In the following anonymous namespace, we provide a generic framework
// to encode/decode baseN format.  We use the following tools:
// - boost base64_from_binary/binary_from_base64: provide mapping table for
//   base64
// - base32hex_from_binary/binary_from_base32hex: provide mapping table for
//   base32hex.  A straightforward variation of their base64 counterparts.
// - EncodeNormalizer/DecodeNormalizer: supplemental filter handling baseN
//   padding characters (=)
// - boost transform_width: an iterator framework for handling data stream
//   per bit-group
//
// A conceptual description of how the encoding and decoding work is as
// follows:
// Encoding:
//   input binary data => Normalizer (append sufficient number of 0 bits)
//                     => transform_width (extract bit groups from the original
//                                         stream)
//                     => baseXX_from_binary (convert each bit group to an
//                                            encoded byte using the mapping)
// Decoding:
//   input baseXX text => Normalizer (convert '=' to 0 bits)
//                     => binary_from_baseXX (convert each encoded byte into
//                                            the original group bit)
//                     => transform_width (build original byte stream by
//                                         concatenating the decoded bit
//                                         stream)
//
// Below, we define a set of templated classes to handle different parameters
// for different encoding algorithms.
namespace {
// Common constants used for all baseN encoding.
const char BASE_PADDING_CHAR = '=';
const uint8_t BINARY_ZERO_CODE = 0;
  
class EncodeNormalizer : public iterator<input_iterator_tag, uint8_t> {
public:
    EncodeNormalizer(const vector<uint8_t>::const_iterator& base,
                     const vector<uint8_t>::const_iterator& base_end) :
        base_(base), base_end_(base_end), in_pad_(false)
    {}
    EncodeNormalizer& operator++() {
        if (!in_pad_) {
            ++base_;
        }
        if (base_ == base_end_) {
            in_pad_ = true;
        }
        return (*this);
    }
    const uint8_t& operator*() const {
        if (in_pad_) {
            return (BINARY_ZERO_CODE);
        } else {
            return (*base_);
        }
    }
    bool operator==(const EncodeNormalizer& other) const {
        return (base_ == other.base_);
    }
private:
    vector<uint8_t>::const_iterator base_;
    const vector<uint8_t>::const_iterator base_end_;
    bool in_pad_;
};

class DecodeNormalizer : public iterator<input_iterator_tag, char> {
public:
    DecodeNormalizer(const char base_zero_code,
                     const string::const_iterator& base,
                     const string::const_iterator& base_beginpad,
                     const string::const_iterator& base_end) :
        base_zero_code_(base_zero_code),
        base_(base), base_beginpad_(base_beginpad), base_end_(base_end),
        in_pad_(false)
    {}
    DecodeNormalizer& operator++() {
        ++base_;
        while (base_ != base_end_ && isspace(*base_)) {
            ++base_;
        }
        if (base_ == base_beginpad_) {
            in_pad_ = true;
        }
        return (*this);
    }
    const char& operator*() const {
        if (in_pad_ && *base_ == BASE_PADDING_CHAR) {
            return (base_zero_code_);
        } else {
            return (*base_);
        }
    }
    bool operator==(const DecodeNormalizer& other) const {
        return (base_ == other.base_);
    }
private:
    const char base_zero_code_;
    string::const_iterator base_;
    const string::const_iterator base_beginpad_;
    const string::const_iterator base_end_;
    bool in_pad_;
};

// BitsPerChunk: number of bits to be converted using the baseN mapping table.
//               e.g. 6 for base64.
// BaseZeroCode: the byte character that represents a value of 0 in
//               the corresponding encoding.  e.g. 'A' for base64.
// Encoder: baseX_from_binary<transform_width<EncodeNormalizer,
//                                            BitsPerChunk, 8> >
// Decoder: transform_width<binary_from_baseX<DecodeNormalizer<BaseZeroCode>,
//                                           char>, 8, BitsPerChunk, char>
template <int BitsPerChunk, char BaseZeroCode,
          typename Encoder, typename Decoder>
struct BaseNTransformer {
    static string encode(const vector<uint8_t>& binary);
    static void decode(const char* algorithm,
                       const string& base64, vector<uint8_t>& result);

    // BITS_PER_GROUP is the number of bits for the smallest possible (non
    // empty) bit string that can be converted to a valid baseN encoded text
    // without padding.  It's the least common multiple of 8 and BitsPerChunk,
    // e.g. 24 for base64.
    static const int BITS_PER_GROUP =
        boost::math::static_lcm<BitsPerChunk, 8>::value;

    // MAX_PADDING_CHARS is the maximum number of padding characters
    // that can appear in a valid baseN encoded text.
    // It's group_len - chars_for_byte, where group_len is the number of
    // encoded characters to represent BITS_PER_GROUP bits, and
    // chars_for_byte is the number of encoded character that is needed to
    // represent a single byte, which is ceil(8 / BitsPerChunk).
    // For example, for base64 we need two encoded characters to represent a
    // byte, and each group consists of 4 encoded characters, so
    // MAX_PADDING_CHARS is 4 - 2 = 2.
    static const int MAX_PADDING_CHARS =
        BITS_PER_GROUP / BitsPerChunk -
        (8 / BitsPerChunk + ((8 % BitsPerChunk) == 0 ? 0 : 1));
}; 

template <int BitsPerChunk, char BaseZeroCode,
          typename Encoder, typename Decoder>
string
BaseNTransformer<BitsPerChunk, BaseZeroCode, Encoder, Decoder>::encode(
    const vector<uint8_t>& binary)
{
    // calculate the resulting length.
    size_t bits = binary.size() * 8;
    if (bits % BITS_PER_GROUP > 0) {
        bits += (BITS_PER_GROUP - (bits % BITS_PER_GROUP));
    }
    const size_t len = bits / BitsPerChunk;

    string result;
    result.reserve(len);
    result.assign(Encoder(EncodeNormalizer(binary.begin(), binary.end())),
                  Encoder(EncodeNormalizer(binary.end(), binary.end())));
    assert(len >= result.length());
    result.append(len - result.length(), BASE_PADDING_CHAR);
    return (result);
}

template <int BitsPerChunk, char BaseZeroCode,
          typename Encoder, typename Decoder>
void
BaseNTransformer<BitsPerChunk, BaseZeroCode, Encoder, Decoder>::decode(
    const char* const algorithm,
    const string& input,
    vector<uint8_t>& result)
{
    // enumerate the number of trailing padding characters (=), ignoring
    // white spaces.  since baseN_from_binary doesn't accept padding,
    // we handle it explicitly.
    size_t padchars = 0;
    string::const_reverse_iterator srit = input.rbegin();
    string::const_reverse_iterator srit_end = input.rend();
    while (srit != srit_end) {
        char ch = *srit;
        if (ch == BASE_PADDING_CHAR) {
            if (++padchars > MAX_PADDING_CHARS) {
                isc_throw(BadValue, "Too many " << algorithm
                          << " padding characters: " << input);
            }
        } else if (!isspace(ch)) {
            break;
        }
        ++srit;
    }
    const size_t padbits = (padchars * BitsPerChunk + 7) & ~7;
    if (padbits > BitsPerChunk * (padchars + 1)) {
        isc_throw(BadValue, "Invalid " << algorithm << "padding: " << input);
    }
    const size_t padbytes = padbits / 8;

    try {
        result.assign(Decoder(DecodeNormalizer(BaseZeroCode, input.begin(),
                                               srit.base(), input.end())),
                      Decoder(DecodeNormalizer(BaseZeroCode, input.end(),
                                               input.end(), input.end())));
    } catch (const dataflow_exception& ex) {
        // convert any boost exceptions into our local one.
        isc_throw(BadValue, ex.what());
    }

    // Confirm the original BaseX text is the canonical encoding of the
    // data.
    assert(result.size() >= padbytes);
    vector<uint8_t>::const_reverse_iterator rit = result.rbegin();
    for (int i = 0; i < padbytes; ++i, ++rit) {
        if (*rit != 0) {
            isc_throw(BadValue, "Non 0 bits included in " << algorithm
                      << " padding: " << input);
        }
    }

    // strip the padded zero-bit fields
    result.resize(result.size() - padbytes);
}

//
// Instantiation for BASE-64
//
typedef
base64_from_binary<transform_width<EncodeNormalizer, 6, 8> > base64_encoder;
typedef
transform_width<binary_from_base64<DecodeNormalizer, char>, 8, 6, char>
base64_decoder;
typedef BaseNTransformer<6, 'A', base64_encoder, base64_decoder>
Base64Transformer;

//
// Instantiation for BASE-32HEX
//
typedef
base32hex_from_binary<transform_width<EncodeNormalizer, 5, 8> >
base32hex_encoder;
typedef
transform_width<binary_from_base32hex<DecodeNormalizer, char>, 8, 5, char>
base32hex_decoder;
typedef BaseNTransformer<5, '0', base32hex_encoder, base32hex_decoder>
Base32HexTransformer;

//
// Instantiation for BASE-16 (HEX)
//
typedef
base16_from_binary<transform_width<EncodeNormalizer, 4, 8> > base16_encoder;
typedef
transform_width<binary_from_base16<DecodeNormalizer, char>, 8, 4, char>
base16_decoder;
typedef BaseNTransformer<4, '0', base16_encoder, base16_decoder>
Base16Transformer;
}

string
encodeBase64(const vector<uint8_t>& binary) {
    return (Base64Transformer::encode(binary));
}

void
decodeBase64(const string& input, vector<uint8_t>& result) {
    Base64Transformer::decode("base64", input, result);
}

string
encodeBase32Hex(const vector<uint8_t>& binary) {
    return (Base32HexTransformer::encode(binary));
}

void
decodeBase32Hex(const string& input, vector<uint8_t>& result) {
    Base32HexTransformer::decode("base32hex", input, result);
}

string
encodeHex(const vector<uint8_t>& binary) {
    return (Base16Transformer::encode(binary));
}

void
decodeHex(const string& input, vector<uint8_t>& result) {
    Base16Transformer::decode("base16", input, result);
}

}
}
