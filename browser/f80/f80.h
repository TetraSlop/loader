#pragma once

// The x87's 80-bit extended format in software. Values keep the exact memory layout (64-bit significand with an
// explicit integer bit, then sign and a 15-bit biased exponent), so FLD/FSTP m80 are copies. Arithmetic rounds to
// the precision and rounding mode of the control word, as the x87 does, and reports exceptions in x87 status-word
// bits. Results must match the hardware bit for bit, including for the 24-bit precision Direct3D 8 leaves set.

#include <cstdint>

namespace tmweb::f80
{

struct F80
{
    std::uint64_t significand  = 0; // bit 63 is the explicit integer bit
    std::uint16_t signExponent = 0; // bit 15 sign, bits 0..14 biased exponent (bias 16383)
};

// Control-word fields (bits 8..9 and 10..11)
enum Precision : std::uint8_t
{
    kSingle   = 0, // 24-bit significand
    kReserved = 1, // behaves as kExtended
    kDouble   = 2, // 53-bit significand
    kExtended = 3, // 64-bit significand
};

enum Rounding : std::uint8_t
{
    kNearest = 0,
    kDown    = 1,
    kUp      = 2,
    kZero    = 3,
};

// Status-word exception bits, accumulated into Env::flags. kRoundedUp is the status word's C1 after an inexact
// result: set when the significand was rounded away from zero.
enum Exception : std::uint16_t
{
    kInvalid   = 0x0001,
    kDenormal  = 0x0002,
    kZeroDiv   = 0x0004,
    kOverflow  = 0x0008,
    kUnderflow = 0x0010,
    kInexact   = 0x0020,
    kRoundedUp = 0x0200,
};

// Make a fresh Env per instruction (envFromControl): kRoundedUp is that instruction's C1, not a sticky flag. The caller
// ORs bits 0..5 into the status word and decides from the control word's masks whether an unmasked kInvalid,
// kZeroDiv or kDenormal leaves the destination unwritten.
struct Env
{
    Precision     precision = kExtended;
    Rounding      rounding  = kNearest;
    std::uint16_t flags     = 0;
};

// The control word's precision and rounding fields; exception masks are the caller's business.
auto envFromControl(std::uint16_t controlWord) -> Env;

// Constants
auto zero(bool negative = false) -> F80;
auto one() -> F80;
auto infinity(bool negative = false) -> F80;
auto indefinite() -> F80;                           // the x87's default quiet NaN
auto pi() -> F80;                                   // FLDPI and the other load-constant instructions round by Env::rounding
auto constant(std::uint8_t which, Env& env) -> F80; // FLD1=0, FLDL2T=1, FLDL2E=2, FLDPI=3, FLDLG2=4, FLDLN2=5, FLDZ=6

// Loads are exact and never round
auto fromInt64(std::int64_t value) -> F80;
auto fromFloat(std::uint32_t bits, Env& env) -> F80;  // IEEE single bits; sets kDenormal / kInvalid for SNaN
auto fromDouble(std::uint64_t bits, Env& env) -> F80; // IEEE double bits

// FADD, FSUB(R), FMUL, FDIV(R) and FCOM(P) with an m32 or m64 operand convert it inside the instruction, which is not
// the same as FLD then the register form: a signaling NaN stays signaling when the x87 picks which NaN to return, and
// a denormal source only raises kDenormal when the operation itself would. Convert with floatOperand/doubleOperand
// (exact, no flags), run the operation, then call denormalOperand if the source was denormal.
auto floatOperand(std::uint32_t bits, bool& denormal) -> F80;
auto doubleOperand(std::uint64_t bits, bool& denormal) -> F80;
void denormalOperand(F80 a, F80 operand, Env& env);

// Stores round to the destination format by Env::rounding (precision control does not apply)
auto toFloat(F80 value, Env& env) -> std::uint32_t;
auto toDouble(F80 value, Env& env) -> std::uint64_t;
// FIST: rounds by Env::rounding; out of range gives the integer indefinite (0x8000...) and kInvalid
auto toInt16(F80 value, Env& env) -> std::int16_t;
auto toInt32(F80 value, Env& env) -> std::int32_t;
auto toInt64(F80 value, Env& env) -> std::int64_t;

// Arithmetic rounds the significand to Env::precision with Env::rounding
auto add(F80 a, F80 b, Env& env) -> F80;
auto sub(F80 a, F80 b, Env& env) -> F80;
auto mul(F80 a, F80 b, Env& env) -> F80;
auto div(F80 a, F80 b, Env& env) -> F80;
auto sqrt(F80 a, Env& env) -> F80;
auto roundToInt(F80 a, Env& env) -> F80;   // FRNDINT
auto scale(F80 a, F80 b, Env& env) -> F80; // FSCALE: a * 2^trunc(b)
// FPREM (ieee false) and FPREM1. conditionCodes gets C0..C3 in status-word positions (C0 0x0100, C1 0x0200,
// C2 0x0400, C3 0x4000): C2 when the reduction is partial, else the quotient's low bits in C0, C3, C1.
auto partialRemainder(F80 a, F80 b, bool ieee, Env& env, std::uint16_t& conditionCodes) -> F80;
auto extract(F80 a, F80& significand, Env& env) -> F80; // FXTRACT: returns the exponent, writes the significand

auto negate(F80 a) -> F80;
auto abs(F80 a) -> F80;

enum class Order : std::uint8_t
{
    Less,
    Equal,
    Greater,
    Unordered,
};

// FCOM raises kInvalid for any NaN, FUCOM only for signaling NaNs
auto compare(F80 a, F80 b, bool quiet, Env& env) -> Order;

// FXAM's C3, C2, C0 classes
enum class Class : std::uint8_t
{
    Unsupported,
    NaN,
    Normal,
    Infinity,
    Zero,
    Empty, // set by the caller from the tag word, never by classify
    Denormal,
};

auto classify(F80 a) -> Class;
auto isNegative(F80 a) -> bool;

// Transcendentals, computed in SoftFloat's 128-bit format and rounded once to 64 bits, so native and Wasm builds
// agree bit for bit. FSIN, FCOS, FSINCOS and FPTAN return false when |a| >= 2^63: the x87 then sets C2 and leaves
// the operand unchanged. FPTAN's push of 1.0 is the caller's.
auto sin(F80 a, F80& result, Env& env) -> bool;
auto cos(F80 a, F80& result, Env& env) -> bool;
auto sinCos(F80 a, F80& sine, F80& cosine, Env& env) -> bool;
auto tan(F80 a, F80& result, Env& env) -> bool;
auto atan2(F80 y, F80 x, Env& env) -> F80;    // FPATAN: y = ST(1), x = ST(0)
auto exp2m1(F80 a, Env& env) -> F80;          // F2XM1
auto yLog2X(F80 x, F80 y, Env& env) -> F80;   // FYL2X: y * log2(x)
auto yLog2Xp1(F80 x, F80 y, Env& env) -> F80; // FYL2XP1: y * log2(x + 1)

// Host doubles, for debugging and host stand-ins; not used by the instructions above
auto toHostDouble(F80 a) -> double;
auto fromHostDouble(double value) -> F80;

} // namespace tmweb::f80
