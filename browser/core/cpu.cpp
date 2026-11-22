#include "core/cpu.h"

#include "core/x87.h"

#include <array>
#include <bit>

namespace tmweb
{
namespace
{

constexpr std::uint32_t kMask[5] = { 0, 0xFF, 0xFFFF, 0, 0xFFFFFFFF };
constexpr std::uint32_t kSign[5] = { 0, 0x80, 0x8000, 0, 0x80000000 };
// EFLAGS bits user code may change through POPF: CF PF AF ZF SF TF DF OF NT AC ID
constexpr std::uint32_t kPopfMask        = 0x00244DD5;
constexpr std::uint32_t kArithmeticFlags = kCF | kPF | kAF | kZF | kSF | kOF;

// Windows user-mode selectors, for MOV r/m, Sreg and PUSH Sreg
enum Selector : std::uint16_t
{
    kSelectorCode = 0x1B,
    kSelectorData = 0x23,
    kSelectorTeb  = 0x3B,
};

// CPUID: GenuineIntel, family 6 model 3 stepping 3; FPU VME DE PSE TSC MSR CX8 CMOV, no MMX, no SSE
constexpr std::uint32_t kCpuidSignature = 0x00000633;
constexpr std::uint32_t kCpuidFeatures  = 0x0000813F;

enum class Rep : std::uint8_t
{
    None,
    Repe,
    Repne,
};

auto parity(const std::uint32_t value) -> bool
{
    return (std::popcount(value & 0xFF) & 1) == 0;
}

// PF for each low byte
constexpr auto kParity = []()
{
    auto table = std::array<std::uint32_t, 256>{};
    for (auto value = 0u; value < 256; ++value)
    {
        if ((std::popcount(value) & 1) == 0)
        {
            table[value] = kPF;
        }
    }
    return table;
}();

// The prefix bytes, so an instruction without one skips the prefix loop
constexpr auto kPrefix = []()
{
    auto table = std::array<bool, 256>{};
    for (const auto prefix : { 0x66, 0x67, 0xF2, 0xF3, 0x64, 0x65, 0xF0, 0x26, 0x2E, 0x36, 0x3E })
    {
        table[prefix] = true;
    }
    return table;
}();

// Thrown to end an instruction early with a CPU event: a divide error, an invalid opcode, an interrupt
struct Interruption
{
    std::uint32_t vector = 0;
    bool          trap   = false; // INT n, INT3, INTO: eip moves past the instruction
};

class Interpreter final
{
public:
    Interpreter(Memory& memory, Context& context)
    : m_(memory)
    , c_(context)
    {
    }

    // One instruction, or one iteration of a REP string instruction
    void execute()
    {
        start_     = c_.eip;
        pc_        = c_.eip;
        fastBytes_ = 0;
        if (m_.pageMapped(start_))
        {
            fastBytes_ = kPageSize - (start_ & (kPageSize - 1));
        }
        opsize_     = 4;
        addr16_     = false;
        segment_    = 0;
        rep_        = Rep::None;
        auto opcode = fetch8();
        for (auto prefixes = 0; prefixes < 15 && kPrefix[opcode]; ++prefixes)
        {
            if (opcode == 0x66)
            {
                opsize_ = 2;
            }
            else if (opcode == 0x67)
            {
                addr16_ = true;
            }
            else if (opcode == 0xF2)
            {
                rep_ = Rep::Repne;
            }
            else if (opcode == 0xF3)
            {
                rep_ = Rep::Repe;
            }
            else if (opcode == 0x64)
            {
                segment_ = c_.fsBase;
            }
            else if (opcode == 0x65)
            {
                segment_ = c_.gsBase;
            }
            else if (opcode == 0xF0 || opcode == 0x26 || opcode == 0x2E || opcode == 0x36 || opcode == 0x3E)
            {
                // LOCK and the flat segments change nothing here
            }
            else
            {
                break;
            }
            opcode = fetch8();
        }
        if (opcode == 0x0F)
        {
            twoByte(fetch8());
        }
        else
        {
            oneByte(opcode);
        }
    }

    auto start() const -> std::uint32_t
    {
        return start_;
    }

private:
    // Fetch
    auto fetch8() -> std::uint32_t
    {
        // Bytes up to the end of the instruction's first page were checked once in execute()
        if (pc_ - start_ >= fastBytes_)
        {
            m_.check(pc_, 1, Access::Execute);
        }
        const auto value = *m_.host(pc_);
        ++pc_;
        return value;
    }

    auto fetch16() -> std::uint32_t
    {
        const auto low = fetch8();
        return low | (fetch8() << 8);
    }

    auto fetch32() -> std::uint32_t
    {
        const auto low = fetch16();
        return low | (fetch16() << 16);
    }

    auto fetchImm(const unsigned size) -> std::uint32_t
    {
        if (size == 1)
        {
            return fetch8();
        }
        if (size == 2)
        {
            return fetch16();
        }
        return fetch32();
    }

    // Memory
    auto load(const std::uint32_t address, const unsigned size) const -> std::uint32_t
    {
        if (size == 1)
        {
            return m_.read<std::uint8_t>(address);
        }
        if (size == 2)
        {
            return m_.read<std::uint16_t>(address);
        }
        return m_.read<std::uint32_t>(address);
    }

    void store(const std::uint32_t address, const unsigned size, const std::uint32_t value)
    {
        if (size == 1)
        {
            m_.write<std::uint8_t>(address, static_cast<std::uint8_t>(value));
        }
        else if (size == 2)
        {
            m_.write<std::uint16_t>(address, static_cast<std::uint16_t>(value));
        }
        else
        {
            m_.write<std::uint32_t>(address, value);
        }
    }

    void push(const std::uint32_t value, const unsigned size = 4)
    {
        const auto esp = c_.r[kEsp] - size;
        store(esp, size, value);
        c_.r[kEsp] = esp;
    }

    auto pop(const unsigned size = 4) -> std::uint32_t
    {
        const auto value = load(c_.r[kEsp], size);
        c_.r[kEsp] += size;
        return value;
    }

    // Registers
    auto reg(const unsigned index, const unsigned size) const -> std::uint32_t
    {
        if (size == 4)
        {
            return c_.r[index];
        }
        if (size == 2)
        {
            return c_.r[index] & 0xFFFF;
        }
        if (index < 4)
        {
            return c_.r[index] & 0xFF;
        }
        return (c_.r[index - 4] >> 8) & 0xFF;
    }

    void setReg(const unsigned index, const unsigned size, const std::uint32_t value)
    {
        if (size == 4)
        {
            c_.r[index] = value;
        }
        else if (size == 2)
        {
            c_.r[index] = (c_.r[index] & 0xFFFF0000) | (value & 0xFFFF);
        }
        else if (index < 4)
        {
            c_.r[index] = (c_.r[index] & 0xFFFFFF00) | (value & 0xFF);
        }
        else
        {
            c_.r[index - 4] = (c_.r[index - 4] & 0xFFFF00FF) | ((value & 0xFF) << 8);
        }
    }

    // ModRM
    void modrm()
    {
        const auto byte = fetch8();
        modrmByte_      = static_cast<std::uint8_t>(byte);
        mod_            = byte >> 6;
        reg_            = (byte >> 3) & 7;
        rm_             = byte & 7;
        if (mod_ == 3)
        {
            isMem_ = false;
            return;
        }
        isMem_ = true;
        if (addr16_)
        {
            ea_ = address16();
        }
        else
        {
            ea_ = address32();
        }
        ea_ += segment_;
    }

    auto address32() -> std::uint32_t
    {
        auto address = std::uint32_t{};
        if (rm_ == 4)
        {
            const auto sib   = fetch8();
            const auto scale = sib >> 6;
            const auto index = (sib >> 3) & 7;
            const auto base  = sib & 7;
            if (index != 4)
            {
                address = c_.r[index] << scale;
            }
            if (base == 5 && mod_ == 0)
            {
                address += fetch32();
            }
            else
            {
                address += c_.r[base];
            }
        }
        else if (rm_ == 5 && mod_ == 0)
        {
            address = fetch32();
        }
        else
        {
            address = c_.r[rm_];
        }
        if (mod_ == 1)
        {
            address += static_cast<std::uint32_t>(static_cast<std::int8_t>(fetch8()));
        }
        else if (mod_ == 2)
        {
            address += fetch32();
        }
        return address;
    }

    auto address16() -> std::uint32_t
    {
        const auto bx      = c_.r[kEbx] & 0xFFFF;
        const auto bp      = c_.r[kEbp] & 0xFFFF;
        const auto si      = c_.r[kEsi] & 0xFFFF;
        const auto di      = c_.r[kEdi] & 0xFFFF;
        auto       address = std::uint32_t{};
        switch (rm_)
        {
            case 0:
                address = bx + si;
                break;
            case 1:
                address = bx + di;
                break;
            case 2:
                address = bp + si;
                break;
            case 3:
                address = bp + di;
                break;
            case 4:
                address = si;
                break;
            case 5:
                address = di;
                break;
            case 6:
                if (mod_ == 0)
                {
                    address = fetch16();
                }
                else
                {
                    address = bp;
                }
                break;
            default:
                address = bx;
                break;
        }
        if (mod_ == 1)
        {
            address += static_cast<std::uint32_t>(static_cast<std::int8_t>(fetch8()));
        }
        else if (mod_ == 2)
        {
            address += fetch16();
        }
        return address & 0xFFFF;
    }

    auto readRm(const unsigned size) const -> std::uint32_t
    {
        if (isMem_)
        {
            return load(ea_, size);
        }
        return reg(rm_, size);
    }

    void writeRm(const unsigned size, const std::uint32_t value)
    {
        if (isMem_)
        {
            store(ea_, size, value);
        }
        else
        {
            setReg(rm_, size, value);
        }
    }

    // Flags
    void setFlag(const std::uint32_t flag, const bool on)
    {
        if (on)
        {
            c_.eflags |= flag;
        }
        else
        {
            c_.eflags &= ~flag;
        }
    }

    auto flag(const std::uint32_t flag) const -> bool
    {
        return (c_.eflags & flag) != 0;
    }

    void setSzp(const std::uint32_t result, const unsigned size)
    {
        c_.eflags = (c_.eflags & ~(kZF | kSF | kPF)) | szp(result, size);
    }

    // ZF SF PF of a result, as bits
    static auto szp(const std::uint32_t result, const unsigned size) -> std::uint32_t
    {
        return (static_cast<std::uint32_t>((result & kMask[size]) == 0) * kZF) | (static_cast<std::uint32_t>((result & kSign[size]) != 0) * kSF) | kParity[result & 0xFF];
    }

    auto add(const std::uint32_t a, const std::uint32_t b, const std::uint32_t carry, const unsigned size)
        -> std::uint32_t
    {
        const auto wide   = static_cast<std::uint64_t>(a) + b + carry;
        const auto result = static_cast<std::uint32_t>(wide) & kMask[size];
        const auto flags  = (static_cast<std::uint32_t>(wide > kMask[size]) * kCF) | (static_cast<std::uint32_t>(((a ^ result) & (b ^ result) & kSign[size]) != 0) * kOF) | ((a ^ b ^ result) & kAF) | szp(result, size);
        c_.eflags         = (c_.eflags & ~kArithmeticFlags) | flags;
        return result;
    }

    auto sub(const std::uint32_t a, const std::uint32_t b, const std::uint32_t borrow, const unsigned size)
        -> std::uint32_t
    {
        const auto result = (a - b - borrow) & kMask[size];
        const auto flags  = (static_cast<std::uint32_t>(static_cast<std::uint64_t>(a) < static_cast<std::uint64_t>(b) + borrow) * kCF) | (static_cast<std::uint32_t>(((a ^ b) & (a ^ result) & kSign[size]) != 0) * kOF) | ((a ^ b ^ result) & kAF) | szp(result, size);
        c_.eflags         = (c_.eflags & ~kArithmeticFlags) | flags;
        return result;
    }

    auto logic(const std::uint32_t result, const unsigned size) -> std::uint32_t
    {
        const auto masked = result & kMask[size];
        c_.eflags         = (c_.eflags & ~kArithmeticFlags) | szp(masked, size);
        return masked;
    }

    // ADD OR ADC SBB AND SUB XOR CMP; returns the result, which CMP discards
    auto alu(const unsigned op, const std::uint32_t a, const std::uint32_t b, const unsigned size) -> std::uint32_t
    {
        switch (op)
        {
            case 0:
                return add(a, b, 0, size);
            case 1:
                return logic(a | b, size);
            case 2:
                return add(a, b, flag(kCF), size);
            case 3:
                return sub(a, b, flag(kCF), size);
            case 4:
                return logic(a & b, size);
            case 5:
            case 7:
                return sub(a, b, 0, size);
            default:
                return logic(a ^ b, size);
        }
    }

    auto condition(const unsigned code) const -> bool
    {
        auto result = false;
        switch (code >> 1)
        {
            case 0:
                result = flag(kOF);
                break;
            case 1:
                result = flag(kCF);
                break;
            case 2:
                result = flag(kZF);
                break;
            case 3:
                result = flag(kCF) || flag(kZF);
                break;
            case 4:
                result = flag(kSF);
                break;
            case 5:
                result = flag(kPF);
                break;
            case 6:
                result = flag(kSF) != flag(kOF);
                break;
            default:
                result = flag(kZF) || flag(kSF) != flag(kOF);
                break;
        }
        if (code & 1)
        {
            return !result;
        }
        return result;
    }

    auto signExtend(const std::uint32_t value, const unsigned size) const -> std::uint32_t
    {
        if (size == 1)
        {
            return static_cast<std::uint32_t>(static_cast<std::int8_t>(value));
        }
        if (size == 2)
        {
            return static_cast<std::uint32_t>(static_cast<std::int16_t>(value));
        }
        return value;
    }

    void jump(const std::uint32_t target)
    {
        if (opsize_ == 2)
        {
            pc_ = target & 0xFFFF;
        }
        else
        {
            pc_ = target;
        }
    }

    void finish()
    {
        c_.eip = pc_;
    }

    [[noreturn]] void invalid()
    {
        throw Interruption{ 6, false };
    }

    [[noreturn]] void privileged()
    {
        throw Interruption{ 13, false };
    }

    // Shifts and rotates: ROL ROR RCL RCR SHL SHR SAL SAR. Flags follow QEMU where the manual leaves them undefined:
    // AF is cleared, OF is computed for every count.
    auto shift(const unsigned op, const std::uint32_t value, const std::uint32_t rawCount, const unsigned size)
        -> std::uint32_t
    {
        const auto bits  = size * 8;
        const auto mask  = kMask[size];
        const auto sign  = kSign[size];
        auto       count = rawCount & 31;
        if (count == 0)
        {
            return value;
        }
        auto result = value;
        switch (op)
        {
            case 0: // ROL
            {
                const auto n = count % bits;
                result       = ((value << n) | (value >> ((bits - n) % bits))) & mask;
                if (n == 0)
                {
                    result = value;
                }
                setFlag(kCF, (result & 1) != 0);
                setFlag(kOF, (((result & sign) != 0) != flag(kCF)));
                return result;
            }
            case 1: // ROR
            {
                const auto n = count % bits;
                result       = ((value >> n) | (value << ((bits - n) % bits))) & mask;
                if (n == 0)
                {
                    result = value;
                }
                setFlag(kCF, (result & sign) != 0);
                setFlag(kOF, (((result ^ (result << 1)) & sign) != 0));
                return result;
            }
            case 2: // RCL
            case 3: // RCR
            {
                auto n = count;
                if (size < 4)
                {
                    n = count % (bits + 1);
                }
                if (n == 0)
                {
                    return value;
                }
                auto       carry = static_cast<std::uint64_t>(flag(kCF));
                auto       wide  = (carry << bits) | value; // bits+1 wide: carry above the value
                const auto width = bits + 1;
                const auto full  = (std::uint64_t{ 1 } << width) - 1;
                if (op == 2)
                {
                    wide = ((wide << n) | (wide >> (width - n))) & full;
                }
                else
                {
                    wide = ((wide >> n) | (wide << (width - n))) & full;
                }
                result = static_cast<std::uint32_t>(wide) & mask;
                carry  = (wide >> bits) & 1;
                setFlag(kCF, carry != 0);
                if (op == 2)
                {
                    setFlag(kOF, ((result & sign) != 0) != (carry != 0));
                }
                else
                {
                    setFlag(kOF, (((result ^ (result << 1)) & sign) != 0));
                }
                return result;
            }
            case 4: // SHL
            case 6: // SAL
            {
                result         = static_cast<std::uint32_t>(static_cast<std::uint64_t>(value) << count) & mask;
                const auto out = (static_cast<std::uint64_t>(value) << (count - 1)) & mask;
                setFlag(kCF, (out & sign) != 0);
                setFlag(kOF, ((out ^ result) & sign) != 0);
                break;
            }
            case 5: // SHR
            {
                result         = static_cast<std::uint32_t>(static_cast<std::uint64_t>(value) >> count);
                const auto out = static_cast<std::uint32_t>(static_cast<std::uint64_t>(value) >> (count - 1));
                setFlag(kCF, (out & 1) != 0);
                setFlag(kOF, ((out ^ result) & sign) != 0);
                break;
            }
            default: // SAR
            {
                const auto signed64 = static_cast<std::int64_t>(static_cast<std::int32_t>(signExtend(value, size)));
                result              = static_cast<std::uint32_t>(signed64 >> count) & mask;
                const auto out      = static_cast<std::uint32_t>(signed64 >> (count - 1)) & mask;
                setFlag(kCF, (out & 1) != 0);
                setFlag(kOF, ((out ^ result) & sign) != 0);
                break;
            }
        }
        setFlag(kAF, false);
        setSzp(result, size);
        return result;
    }

    // SHLD/SHRD
    auto doubleShift(const bool left, const std::uint32_t dst, const std::uint32_t src, const std::uint32_t rawCount, const unsigned size) -> std::uint32_t
    {
        const auto count = rawCount & 31;
        if (count == 0)
        {
            return dst;
        }
        const auto bits   = size * 8;
        auto       result = std::uint32_t{};
        auto       out    = std::uint32_t{};
        if (size == 4)
        {
            if (left)
            {
                const auto wide = (static_cast<std::uint64_t>(dst) << 32) | src;
                result          = static_cast<std::uint32_t>((wide << count) >> 32);
                out             = static_cast<std::uint32_t>((wide << (count - 1)) >> 32);
                setFlag(kCF, (out & 0x80000000) != 0);
            }
            else
            {
                const auto wide = (static_cast<std::uint64_t>(src) << 32) | dst;
                result          = static_cast<std::uint32_t>(wide >> count);
                out             = static_cast<std::uint32_t>(wide >> (count - 1));
                setFlag(kCF, (out & 1) != 0);
            }
        }
        else
        {
            // 16-bit: QEMU shifts through a 48-bit value (dst:src:dst); counts above 16 are undefined
            auto wide = std::uint64_t{};
            if (left)
            {
                wide   = (static_cast<std::uint64_t>(dst) << 32) | (static_cast<std::uint64_t>(src) << 16) | dst;
                result = static_cast<std::uint32_t>((wide << count) >> 32) & 0xFFFF;
                out    = static_cast<std::uint32_t>((wide << (count - 1)) >> 32) & 0xFFFF;
                setFlag(kCF, (out & 0x8000) != 0);
            }
            else
            {
                wide   = (static_cast<std::uint64_t>(dst) << 32) | (static_cast<std::uint64_t>(src) << 16) | dst;
                wide   = (static_cast<std::uint64_t>(src) << 16) | dst | (static_cast<std::uint64_t>(dst) << 32);
                result = static_cast<std::uint32_t>(wide >> count) & 0xFFFF;
                out    = static_cast<std::uint32_t>(wide >> (count - 1)) & 0xFFFF;
                setFlag(kCF, (out & 1) != 0);
            }
        }
        static_cast<void>(bits);
        setFlag(kOF, ((out ^ result) & kSign[size]) != 0);
        setFlag(kAF, false);
        setSzp(result, size);
        return result;
    }

    // MUL IMUL DIV IDIV on the accumulator
    void multiply(const bool isSigned, const std::uint32_t operand, const unsigned size)
    {
        auto low  = std::uint32_t{};
        auto high = std::uint32_t{};
        auto over = false;
        if (size == 1)
        {
            const auto a = c_.r[kEax] & 0xFF;
            auto       r = std::uint32_t{};
            if (isSigned)
            {
                const auto product = static_cast<std::int16_t>(static_cast<std::int8_t>(a) * static_cast<std::int8_t>(operand));
                r                  = static_cast<std::uint16_t>(product);
                over               = product != static_cast<std::int8_t>(product);
            }
            else
            {
                r    = a * operand;
                over = (r & 0xFF00) != 0;
            }
            setReg(kEax, 2, r);
            low = r & 0xFF;
        }
        else if (size == 2)
        {
            const auto a = c_.r[kEax] & 0xFFFF;
            auto       r = std::uint32_t{};
            if (isSigned)
            {
                const auto product = static_cast<std::int32_t>(static_cast<std::int16_t>(a)) * static_cast<std::int16_t>(operand);
                r                  = static_cast<std::uint32_t>(product);
                over               = product != static_cast<std::int16_t>(product);
            }
            else
            {
                r    = a * operand;
                over = (r & 0xFFFF0000) != 0;
            }
            low  = r & 0xFFFF;
            high = r >> 16;
            setReg(kEax, 2, low);
            setReg(kEdx, 2, high);
        }
        else
        {
            if (isSigned)
            {
                const auto product = static_cast<std::int64_t>(static_cast<std::int32_t>(c_.r[kEax])) * static_cast<std::int32_t>(operand);
                low                = static_cast<std::uint32_t>(product);
                high               = static_cast<std::uint32_t>(static_cast<std::uint64_t>(product) >> 32);
                over               = product != static_cast<std::int32_t>(product);
            }
            else
            {
                const auto product = static_cast<std::uint64_t>(c_.r[kEax]) * operand;
                low                = static_cast<std::uint32_t>(product);
                high               = static_cast<std::uint32_t>(product >> 32);
                over               = high != 0;
            }
            c_.r[kEax] = low;
            c_.r[kEdx] = high;
        }
        mulFlags(over, low, size);
    }

    // QEMU's MUL flags: CF = OF = overflow; SF ZF PF from the low result; AF cleared
    void mulFlags(const bool overflow, const std::uint32_t low, const unsigned size)
    {
        setFlag(kCF, overflow);
        setFlag(kOF, overflow);
        setFlag(kAF, false);
        setSzp(low, size);
    }

    void divide(const bool isSigned, const std::uint32_t divisor, const unsigned size)
    {
        if (divisor == 0)
        {
            throw Interruption{ 0, false };
        }
        if (size == 1)
        {
            const auto dividend = c_.r[kEax] & 0xFFFF;
            auto       q        = std::uint32_t{};
            auto       r        = std::uint32_t{};
            if (isSigned)
            {
                const auto n  = static_cast<std::int16_t>(dividend);
                const auto d  = static_cast<std::int8_t>(divisor);
                const auto qs = n / d;
                if (qs > 127 || qs < -128)
                {
                    throw Interruption{ 0, false };
                }
                q = static_cast<std::uint8_t>(qs);
                r = static_cast<std::uint8_t>(n % d);
            }
            else
            {
                q = dividend / divisor;
                if (q > 0xFF)
                {
                    throw Interruption{ 0, false };
                }
                r = dividend % divisor;
            }
            setReg(kEax, 2, (r << 8) | (q & 0xFF));
        }
        else if (size == 2)
        {
            const auto dividend = ((c_.r[kEdx] & 0xFFFF) << 16) | (c_.r[kEax] & 0xFFFF);
            auto       q        = std::uint32_t{};
            auto       r        = std::uint32_t{};
            if (isSigned)
            {
                const auto n  = static_cast<std::int32_t>(dividend);
                const auto d  = static_cast<std::int16_t>(divisor);
                const auto qs = n / d;
                if (qs > 32767 || qs < -32768)
                {
                    throw Interruption{ 0, false };
                }
                q = static_cast<std::uint16_t>(qs);
                r = static_cast<std::uint16_t>(n % d);
            }
            else
            {
                q = dividend / divisor;
                if (q > 0xFFFF)
                {
                    throw Interruption{ 0, false };
                }
                r = dividend % divisor;
            }
            setReg(kEax, 2, q);
            setReg(kEdx, 2, r);
        }
        else
        {
            const auto dividend = (static_cast<std::uint64_t>(c_.r[kEdx]) << 32) | c_.r[kEax];
            if (isSigned)
            {
                const auto n = static_cast<std::int64_t>(dividend);
                const auto d = static_cast<std::int64_t>(static_cast<std::int32_t>(divisor));
                if (n == INT64_MIN && d == -1)
                {
                    throw Interruption{ 0, false };
                }
                const auto qs = n / d;
                if (qs > INT32_MAX || qs < INT32_MIN)
                {
                    throw Interruption{ 0, false };
                }
                c_.r[kEax] = static_cast<std::uint32_t>(qs);
                c_.r[kEdx] = static_cast<std::uint32_t>(n % d);
            }
            else
            {
                const auto q = dividend / divisor;
                if (q > 0xFFFFFFFF)
                {
                    throw Interruption{ 0, false };
                }
                c_.r[kEax] = static_cast<std::uint32_t>(q);
                c_.r[kEdx] = static_cast<std::uint32_t>(dividend % divisor);
            }
        }
        // QEMU leaves the flags as they were
    }

    // String instructions: one iteration per call; a REP that has more to do re-enters the instruction
    auto counter() const -> std::uint32_t
    {
        if (addr16_)
        {
            return c_.r[kEcx] & 0xFFFF;
        }
        return c_.r[kEcx];
    }

    void decrementCounter()
    {
        if (addr16_)
        {
            setReg(kEcx, 2, c_.r[kEcx] - 1);
        }
        else
        {
            --c_.r[kEcx];
        }
    }

    void advance(const unsigned index, const unsigned size)
    {
        auto delta = size;
        if (flag(kDF))
        {
            delta = 0u - size;
        }
        if (addr16_)
        {
            setReg(index, 2, c_.r[index] + delta);
        }
        else
        {
            c_.r[index] += delta;
        }
    }

    auto pointer(const unsigned index) const -> std::uint32_t
    {
        if (addr16_)
        {
            return c_.r[index] & 0xFFFF;
        }
        return c_.r[index];
    }

    void string(const std::uint32_t opcode)
    {
        auto size = opsize_;
        if ((opcode & 1) == 0)
        {
            size = 1;
        }
        if (rep_ != Rep::None && counter() == 0)
        {
            finish();
            return;
        }
        const auto source   = segment_ + pointer(kEsi);
        const auto dest     = pointer(kEdi);
        auto       compares = false;
        switch (opcode)
        {
            case 0xA4:
            case 0xA5:
                store(dest, size, load(source, size));
                advance(kEsi, size);
                advance(kEdi, size);
                break;
            case 0xA6:
            case 0xA7:
                sub(load(source, size), load(dest, size), 0, size);
                advance(kEsi, size);
                advance(kEdi, size);
                compares = true;
                break;
            case 0xAA:
            case 0xAB:
                store(dest, size, reg(kEax, size));
                advance(kEdi, size);
                break;
            case 0xAC:
            case 0xAD:
                setReg(kEax, size, load(source, size));
                advance(kEsi, size);
                break;
            default: // SCAS
                sub(reg(kEax, size), load(dest, size), 0, size);
                advance(kEdi, size);
                compares = true;
                break;
        }
        if (rep_ == Rep::None)
        {
            finish();
            return;
        }
        decrementCounter();
        auto again = counter() != 0;
        if (compares && rep_ == Rep::Repe && !flag(kZF))
        {
            again = false;
        }
        if (compares && rep_ == Rep::Repne && flag(kZF))
        {
            again = false;
        }
        if (again)
        {
            c_.eip = start_;
        }
        else
        {
            finish();
        }
    }

    auto segmentSelector(const unsigned index) const -> std::uint32_t
    {
        switch (index)
        {
            case 1:
                return kSelectorCode;
            case 4:
                return kSelectorTeb;
            case 5:
                return 0;
            default:
                return kSelectorData;
        }
    }

    void bitTest(const unsigned op, const std::uint32_t offset, const bool immediate, const unsigned size)
    {
        // op: 0 BT, 1 BTS, 2 BTR, 3 BTC
        auto bit = std::uint32_t{};
        if (isMem_ && !immediate)
        {
            // A register bit offset reaches outside the operand, signed
            const auto signedOffset = static_cast<std::int32_t>(signExtend(offset, size));
            const auto bitsPerUnit  = static_cast<std::int32_t>(size * 8);
            auto       units        = signedOffset / bitsPerUnit;
            auto       within       = signedOffset % bitsPerUnit;
            if (within < 0)
            {
                within += bitsPerUnit;
                --units;
            }
            ea_ += static_cast<std::uint32_t>(units * static_cast<std::int32_t>(size));
            bit = static_cast<std::uint32_t>(within);
        }
        else
        {
            bit = offset & (size * 8 - 1);
        }
        const auto value = readRm(size);
        setFlag(kCF, ((value >> bit) & 1) != 0);
        auto result = value;
        if (op == 1)
        {
            result = value | (1u << bit);
        }
        else if (op == 2)
        {
            result = value & ~(1u << bit);
        }
        else if (op == 3)
        {
            result = value ^ (1u << bit);
        }
        if (op != 0)
        {
            writeRm(size, result);
        }
    }

    void oneByte(const std::uint32_t opcode)
    {
        const auto size8 = 1u;
        const auto sizev = opsize_;
        if (opcode < 0x40 && (opcode & 7) < 6)
        {
            const auto op   = opcode >> 3;
            const auto form = opcode & 7;
            if (form == 4 || form == 5)
            {
                auto size = sizev;
                if (form == 4)
                {
                    size = size8;
                }
                const auto imm    = fetchImm(size);
                const auto result = alu(op, reg(kEax, size), imm, size);
                if (op != 7)
                {
                    setReg(kEax, size, result);
                }
                finish();
                return;
            }
            modrm();
            auto size = sizev;
            if ((form & 1) == 0)
            {
                size = size8;
            }
            if (form < 2)
            {
                const auto result = alu(op, readRm(size), reg(reg_, size), size);
                if (op != 7)
                {
                    writeRm(size, result);
                }
            }
            else
            {
                const auto result = alu(op, reg(reg_, size), readRm(size), size);
                if (op != 7)
                {
                    setReg(reg_, size, result);
                }
            }
            finish();
            return;
        }
        switch (opcode)
        {
            case 0x06:
            case 0x0E:
            case 0x16:
            case 0x1E:
                push(segmentSelector((opcode >> 3) & 3), opsize_);
                break;
            case 0x07:
            case 0x17:
            case 0x1F:
                pop(opsize_);
                break;
            case 0x27:
                daa();
                break;
            case 0x2F:
                das();
                break;
            case 0x37:
                aaa(true);
                break;
            case 0x3F:
                aaa(false);
                break;
            case 0x40:
            case 0x41:
            case 0x42:
            case 0x43:
            case 0x44:
            case 0x45:
            case 0x46:
            case 0x47:
                setReg(opcode & 7, sizev, incDec(reg(opcode & 7, sizev), true, sizev));
                break;
            case 0x48:
            case 0x49:
            case 0x4A:
            case 0x4B:
            case 0x4C:
            case 0x4D:
            case 0x4E:
            case 0x4F:
                setReg(opcode & 7, sizev, incDec(reg(opcode & 7, sizev), false, sizev));
                break;
            case 0x50:
            case 0x51:
            case 0x52:
            case 0x53:
            case 0x54:
            case 0x55:
            case 0x56:
            case 0x57:
                push(reg(opcode & 7, sizev), sizev);
                break;
            case 0x58:
            case 0x59:
            case 0x5A:
            case 0x5B:
            case 0x5C:
            case 0x5D:
            case 0x5E:
            case 0x5F:
            {
                const auto value = pop(sizev);
                setReg(opcode & 7, sizev, value);
                break;
            }
            case 0x60:
            {
                const auto esp = c_.r[kEsp];
                for (auto index = 0u; index < 8; ++index)
                {
                    auto value = c_.r[index];
                    if (index == kEsp)
                    {
                        value = esp;
                    }
                    store(esp - sizev * (index + 1), sizev, value);
                }
                c_.r[kEsp] = esp - 8 * sizev;
                break;
            }
            case 0x61:
            {
                const auto esp = c_.r[kEsp];
                for (auto index = 0u; index < 8; ++index)
                {
                    const auto value = load(esp + sizev * (7 - index), sizev);
                    if (index != kEsp)
                    {
                        setReg(index, sizev, value);
                    }
                }
                c_.r[kEsp] = esp + 8 * sizev;
                break;
            }
            case 0x62:
                bound();
                break;
            case 0x68:
                push(fetchImm(sizev), sizev);
                break;
            case 0x6A:
                push(signExtend(fetch8(), 1), sizev);
                break;
            case 0x69:
            case 0x6B:
            {
                modrm();
                const auto a = readRm(sizev);
                auto       b = std::uint32_t{};
                if (opcode == 0x69)
                {
                    b = fetchImm(sizev);
                }
                else
                {
                    b = signExtend(fetch8(), 1);
                }
                setReg(reg_, sizev, imul(a, b, sizev));
                break;
            }
            case 0x6C:
            case 0x6D:
            case 0x6E:
            case 0x6F:
            case 0xE4:
            case 0xE5:
            case 0xE6:
            case 0xE7:
            case 0xEC:
            case 0xED:
            case 0xEE:
            case 0xEF:
            case 0xF4:
            case 0xFA:
            case 0xFB:
                privileged();
            case 0x70:
            case 0x71:
            case 0x72:
            case 0x73:
            case 0x74:
            case 0x75:
            case 0x76:
            case 0x77:
            case 0x78:
            case 0x79:
            case 0x7A:
            case 0x7B:
            case 0x7C:
            case 0x7D:
            case 0x7E:
            case 0x7F:
            {
                const auto displacement = signExtend(fetch8(), 1);
                if (condition(opcode & 15))
                {
                    jump(pc_ + displacement);
                }
                break;
            }
            case 0x80:
            case 0x81:
            case 0x82:
            case 0x83:
            {
                modrm();
                auto size = sizev;
                if (opcode == 0x80 || opcode == 0x82)
                {
                    size = size8;
                }
                const auto a = readRm(size);
                auto       b = std::uint32_t{};
                if (opcode == 0x81)
                {
                    b = fetchImm(size);
                }
                else
                {
                    b = signExtend(fetch8(), 1) & kMask[size];
                }
                const auto result = alu(reg_, a, b, size);
                if (reg_ != 7)
                {
                    writeRm(size, result);
                }
                break;
            }
            case 0x84:
            case 0x85:
            {
                modrm();
                auto size = sizev;
                if (opcode == 0x84)
                {
                    size = size8;
                }
                logic(readRm(size) & reg(reg_, size), size);
                break;
            }
            case 0x86:
            case 0x87:
            {
                modrm();
                auto size = sizev;
                if (opcode == 0x86)
                {
                    size = size8;
                }
                const auto a = readRm(size);
                writeRm(size, reg(reg_, size));
                setReg(reg_, size, a);
                break;
            }
            case 0x88:
            case 0x89:
            {
                modrm();
                auto size = sizev;
                if (opcode == 0x88)
                {
                    size = size8;
                }
                writeRm(size, reg(reg_, size));
                break;
            }
            case 0x8A:
            case 0x8B:
            {
                modrm();
                auto size = sizev;
                if (opcode == 0x8A)
                {
                    size = size8;
                }
                setReg(reg_, size, readRm(size));
                break;
            }
            case 0x8C:
            {
                modrm();
                auto size = 2u;
                if (!isMem_)
                {
                    size = sizev;
                }
                writeRm(size, segmentSelector(reg_));
                break;
            }
            case 0x8D:
            {
                const auto saved = segment_;
                segment_         = 0;
                modrm();
                segment_ = saved;
                if (!isMem_)
                {
                    invalid();
                }
                setReg(reg_, sizev, ea_);
                break;
            }
            case 0x8E:
                modrm();
                readRm(2);
                break;
            case 0x8F:
            {
                // The address uses ESP after the pop
                const auto value = load(c_.r[kEsp], sizev);
                c_.r[kEsp] += sizev;
                modrm();
                writeRm(sizev, value);
                break;
            }
            case 0x90:
                break;
            case 0x91:
            case 0x92:
            case 0x93:
            case 0x94:
            case 0x95:
            case 0x96:
            case 0x97:
            {
                const auto a = reg(kEax, sizev);
                setReg(kEax, sizev, reg(opcode & 7, sizev));
                setReg(opcode & 7, sizev, a);
                break;
            }
            case 0x98:
                if (sizev == 2)
                {
                    setReg(kEax, 2, signExtend(c_.r[kEax] & 0xFF, 1));
                }
                else
                {
                    c_.r[kEax] = signExtend(c_.r[kEax] & 0xFFFF, 2);
                }
                break;
            case 0x99:
                if (sizev == 2)
                {
                    setReg(kEdx, 2, ((c_.r[kEax] & 0x8000) != 0) * 0xFFFFu);
                }
                else
                {
                    c_.r[kEdx] = ((c_.r[kEax] & 0x80000000) != 0) * 0xFFFFFFFFu;
                }
                break;
            case 0x9B:
                break;
            case 0x9C:
                push((c_.eflags & ~0x30000u) | 2, sizev);
                break;
            case 0x9D:
            {
                const auto value = pop(sizev);
                auto       mask  = kPopfMask;
                if (sizev == 2)
                {
                    mask &= 0xFFFF;
                }
                c_.eflags = (c_.eflags & ~mask) | (value & mask) | 2;
                break;
            }
            case 0x9E:
            {
                const auto ah = (c_.r[kEax] >> 8) & (kCF | kPF | kAF | kZF | kSF);
                c_.eflags     = (c_.eflags & ~0xFFu) | ah | 2;
                break;
            }
            case 0x9F:
                setReg(4, 1, (c_.eflags & (kCF | kPF | kAF | kZF | kSF)) | 2);
                break;
            case 0xA0:
            case 0xA1:
            case 0xA2:
            case 0xA3:
            {
                auto size = sizev;
                if ((opcode & 1) == 0)
                {
                    size = size8;
                }
                auto address = std::uint32_t{};
                if (addr16_)
                {
                    address = fetch16();
                }
                else
                {
                    address = fetch32();
                }
                address += segment_;
                if (opcode < 0xA2)
                {
                    setReg(kEax, size, load(address, size));
                }
                else
                {
                    store(address, size, reg(kEax, size));
                }
                break;
            }
            case 0xA4:
            case 0xA5:
            case 0xA6:
            case 0xA7:
            case 0xAA:
            case 0xAB:
            case 0xAC:
            case 0xAD:
            case 0xAE:
            case 0xAF:
                string(opcode);
                return;
            case 0xA8:
                logic((c_.r[kEax] & fetch8()), 1);
                break;
            case 0xA9:
                logic(reg(kEax, sizev) & fetchImm(sizev), sizev);
                break;
            case 0xB0:
            case 0xB1:
            case 0xB2:
            case 0xB3:
            case 0xB4:
            case 0xB5:
            case 0xB6:
            case 0xB7:
                setReg(opcode & 7, 1, fetch8());
                break;
            case 0xB8:
            case 0xB9:
            case 0xBA:
            case 0xBB:
            case 0xBC:
            case 0xBD:
            case 0xBE:
            case 0xBF:
                setReg(opcode & 7, sizev, fetchImm(sizev));
                break;
            case 0xC0:
            case 0xC1:
            case 0xD0:
            case 0xD1:
            case 0xD2:
            case 0xD3:
            {
                modrm();
                auto size = sizev;
                if ((opcode & 1) == 0)
                {
                    size = size8;
                }
                const auto value = readRm(size);
                auto       count = 1u;
                if (opcode < 0xD0)
                {
                    count = fetch8();
                }
                else if (opcode >= 0xD2)
                {
                    count = c_.r[kEcx] & 0xFF;
                }
                const auto result = shift(reg_, value, count, size);
                if ((count & 31) != 0)
                {
                    writeRm(size, result);
                }
                break;
            }
            case 0xC2:
            {
                const auto bytes  = fetch16();
                const auto target = pop(sizev);
                c_.r[kEsp] += bytes;
                jump(target);
                break;
            }
            case 0xC3:
                jump(pop(sizev));
                break;
            case 0xC6:
            case 0xC7:
            {
                modrm();
                auto size = sizev;
                if (opcode == 0xC6)
                {
                    size = size8;
                }
                writeRm(size, fetchImm(size));
                break;
            }
            case 0xC8:
                enter();
                break;
            case 0xC9:
                c_.r[kEsp] = c_.r[kEbp];
                setReg(kEbp, sizev, pop(sizev));
                break;
            case 0xCA:
            case 0xCB:
            {
                auto bytes = 0u;
                if (opcode == 0xCA)
                {
                    bytes = fetch16();
                }
                const auto target = pop(sizev);
                pop(sizev);
                c_.r[kEsp] += bytes;
                jump(target);
                break;
            }
            case 0xCC:
                finish();
                throw Interruption{ 3, true };
            case 0xCD:
            {
                const auto vector = fetch8();
                finish();
                throw Interruption{ vector, true };
            }
            case 0xCE:
                if (flag(kOF))
                {
                    finish();
                    throw Interruption{ 4, true };
                }
                break;
            case 0xD4:
            {
                const auto base = fetch8();
                if (base == 0)
                {
                    throw Interruption{ 0, false };
                }
                const auto al = c_.r[kEax] & 0xFF;
                setReg(kEax, 2, ((al / base) << 8) | (al % base));
                setFlag(kAF, false);
                setFlag(kCF, false);
                setFlag(kOF, false);
                setSzp(al % base, 1);
                break;
            }
            case 0xD5:
            {
                const auto base = fetch8();
                const auto al   = ((c_.r[kEax] & 0xFF) + ((c_.r[kEax] >> 8) & 0xFF) * base) & 0xFF;
                setReg(kEax, 2, al);
                setFlag(kAF, false);
                setFlag(kCF, false);
                setFlag(kOF, false);
                setSzp(al, 1);
                break;
            }
            case 0xD6:
                setReg(kEax, 1, flag(kCF) * 0xFFu);
                break;
            case 0xD7:
            {
                auto address = c_.r[kEbx] + (c_.r[kEax] & 0xFF);
                if (addr16_)
                {
                    address &= 0xFFFF;
                }
                setReg(kEax, 1, load(segment_ + address, 1));
                break;
            }
            case 0xD8:
            case 0xD9:
            case 0xDA:
            case 0xDB:
            case 0xDC:
            case 0xDD:
            case 0xDE:
            case 0xDF:
            {
                modrm();
                const auto operand = X87Operand{ isMem_, ea_, rm_, reg_, modrmByte_ };
                if (!x87Execute(m_, c_, opcode, operand, start_))
                {
                    invalid();
                }
                break;
            }
            case 0xE0:
            case 0xE1:
            case 0xE2:
            case 0xE3:
            {
                const auto displacement = signExtend(fetch8(), 1);
                auto       taken        = false;
                if (opcode == 0xE3)
                {
                    taken = counter() == 0;
                }
                else
                {
                    decrementCounter();
                    taken = counter() != 0;
                    if (opcode == 0xE0)
                    {
                        taken = taken && !flag(kZF);
                    }
                    else if (opcode == 0xE1)
                    {
                        taken = taken && flag(kZF);
                    }
                }
                if (taken)
                {
                    jump(pc_ + displacement);
                }
                break;
            }
            case 0xE8:
            {
                const auto displacement = fetchImm(sizev);
                push(pc_, sizev);
                jump(pc_ + signExtend(displacement, sizev));
                break;
            }
            case 0xE9:
            {
                const auto displacement = fetchImm(sizev);
                jump(pc_ + signExtend(displacement, sizev));
                break;
            }
            case 0xEB:
            {
                const auto displacement = signExtend(fetch8(), 1);
                jump(pc_ + displacement);
                break;
            }
            case 0xF1:
                finish();
                throw Interruption{ 1, true };
            case 0xF5:
                c_.eflags ^= kCF;
                break;
            case 0xF6:
            case 0xF7:
                group3(opcode);
                break;
            case 0xF8:
                setFlag(kCF, false);
                break;
            case 0xF9:
                setFlag(kCF, true);
                break;
            case 0xFC:
                setFlag(kDF, false);
                break;
            case 0xFD:
                setFlag(kDF, true);
                break;
            case 0xFE:
                modrm();
                if (reg_ > 1)
                {
                    invalid();
                }
                writeRm(1, incDec(readRm(1), reg_ == 0, 1));
                break;
            case 0xFF:
                group5();
                break;
            default:
                invalid();
        }
        finish();
    }

    auto incDec(const std::uint32_t value, const bool increment, const unsigned size) -> std::uint32_t
    {
        const auto carry  = flag(kCF);
        auto       result = std::uint32_t{};
        if (increment)
        {
            result = add(value, 1, 0, size);
        }
        else
        {
            result = sub(value, 1, 0, size);
        }
        setFlag(kCF, carry);
        return result;
    }

    auto imul(const std::uint32_t a, const std::uint32_t b, const unsigned size) -> std::uint32_t
    {
        if (size == 2)
        {
            const auto product = static_cast<std::int32_t>(static_cast<std::int16_t>(a)) * static_cast<std::int16_t>(b);
            const auto low     = static_cast<std::uint32_t>(product) & 0xFFFF;
            mulFlags(product != static_cast<std::int16_t>(product), low, 2);
            return low;
        }
        const auto product = static_cast<std::int64_t>(static_cast<std::int32_t>(a)) * static_cast<std::int32_t>(b);
        const auto low     = static_cast<std::uint32_t>(product);
        mulFlags(product != static_cast<std::int32_t>(product), low, 4);
        return low;
    }

    void group3(const std::uint32_t opcode)
    {
        modrm();
        auto size = opsize_;
        if (opcode == 0xF6)
        {
            size = 1;
        }
        switch (reg_)
        {
            case 0:
            case 1:
            {
                const auto value = readRm(size);
                logic(value & fetchImm(size), size);
                break;
            }
            case 2:
                writeRm(size, ~readRm(size) & kMask[size]);
                break;
            case 3:
            {
                const auto value  = readRm(size);
                const auto result = sub(0, value, 0, size);
                writeRm(size, result);
                break;
            }
            case 4:
                multiply(false, readRm(size), size);
                break;
            case 5:
                multiply(true, readRm(size), size);
                break;
            case 6:
                divide(false, readRm(size), size);
                break;
            default:
                divide(true, readRm(size), size);
                break;
        }
    }

    void group5()
    {
        modrm();
        const auto size = opsize_;
        switch (reg_)
        {
            case 0:
                writeRm(size, incDec(readRm(size), true, size));
                break;
            case 1:
                writeRm(size, incDec(readRm(size), false, size));
                break;
            case 2:
            {
                const auto target = readRm(size);
                push(pc_, size);
                jump(target);
                break;
            }
            case 4:
                jump(readRm(size));
                break;
            case 6:
                push(readRm(size), size);
                break;
            default:
                invalid();
        }
    }

    void enter()
    {
        const auto bytes = fetch16();
        const auto level = fetch8() & 31;
        push(c_.r[kEbp], opsize_);
        const auto frame = c_.r[kEsp];
        for (auto index = 1u; index < level; ++index)
        {
            c_.r[kEbp] -= opsize_;
            push(load(c_.r[kEbp], opsize_), opsize_);
        }
        if (level > 0)
        {
            push(frame, opsize_);
        }
        c_.r[kEbp] = frame;
        c_.r[kEsp] -= bytes;
    }

    void bound()
    {
        modrm();
        if (!isMem_)
        {
            invalid();
        }
        const auto value = static_cast<std::int32_t>(signExtend(reg(reg_, opsize_), opsize_));
        const auto lower = static_cast<std::int32_t>(signExtend(load(ea_, opsize_), opsize_));
        const auto upper = static_cast<std::int32_t>(signExtend(load(ea_ + opsize_, opsize_), opsize_));
        if (value < lower || value > upper)
        {
            throw Interruption{ 5, false };
        }
    }

    void daa()
    {
        auto       al    = c_.r[kEax] & 0xFF;
        const auto oldAl = al;
        const auto oldCf = flag(kCF);
        setFlag(kCF, false);
        if ((al & 0x0F) > 9 || flag(kAF))
        {
            al = (al + 6) & 0xFF;
            setFlag(kCF, oldCf || oldAl > 0xF9);
            setFlag(kAF, true);
        }
        else
        {
            setFlag(kAF, false);
        }
        if (oldAl > 0x99 || oldCf)
        {
            al = (al + 0x60) & 0xFF;
            setFlag(kCF, true);
        }
        setReg(kEax, 1, al);
        setSzp(al, 1);
    }

    void das()
    {
        auto       al    = c_.r[kEax] & 0xFF;
        const auto oldAl = al;
        const auto oldCf = flag(kCF);
        setFlag(kCF, false);
        if ((al & 0x0F) > 9 || flag(kAF))
        {
            setFlag(kCF, oldCf || al < 6);
            al = (al - 6) & 0xFF;
            setFlag(kAF, true);
        }
        else
        {
            setFlag(kAF, false);
        }
        if (oldAl > 0x99 || oldCf)
        {
            al = (al - 0x60) & 0xFF;
            setFlag(kCF, true);
        }
        setReg(kEax, 1, al);
        setSzp(al, 1);
    }

    void aaa(const bool adjustUp)
    {
        if ((c_.r[kEax] & 0x0F) > 9 || flag(kAF))
        {
            auto ax = c_.r[kEax] & 0xFFFF;
            if (adjustUp)
            {
                ax = ax + 0x106;
            }
            else
            {
                ax = ((ax - 6) & 0xFF) | ((((ax >> 8) - 1) & 0xFF) << 8);
            }
            setReg(kEax, 2, ax);
            setFlag(kAF, true);
            setFlag(kCF, true);
        }
        else
        {
            setFlag(kAF, false);
            setFlag(kCF, false);
        }
        setReg(kEax, 1, c_.r[kEax] & 0x0F);
    }

    void twoByte(const std::uint32_t opcode)
    {
        const auto sizev = opsize_;
        if (opcode >= 0x80 && opcode <= 0x8F)
        {
            const auto displacement = fetchImm(sizev);
            if (condition(opcode & 15))
            {
                jump(pc_ + signExtend(displacement, sizev));
            }
            finish();
            return;
        }
        if (opcode >= 0x40 && opcode <= 0x4F)
        {
            modrm();
            const auto value = readRm(sizev);
            if (condition(opcode & 15))
            {
                setReg(reg_, sizev, value);
            }
            else if (sizev == 4)
            {
                // A 32-bit CMOV writes its destination either way
                setReg(reg_, 4, c_.r[reg_]);
            }
            finish();
            return;
        }
        if (opcode >= 0x90 && opcode <= 0x9F)
        {
            modrm();
            writeRm(1, condition(opcode & 15));
            finish();
            return;
        }
        if (opcode >= 0xC8 && opcode <= 0xCF)
        {
            c_.r[opcode & 7] = std::byteswap(c_.r[opcode & 7]);
            finish();
            return;
        }
        switch (opcode)
        {
            case 0x00:
                modrm();
                if (reg_ == 0 || reg_ == 1)
                {
                    // SLDT, STR
                    auto value = 0u;
                    if (reg_ == 1)
                    {
                        value = 0x28;
                    }
                    writeRm(2, value);
                }
                else
                {
                    privileged();
                }
                break;
            case 0x01:
                modrm();
                if (isMem_ && (reg_ == 0 || reg_ == 1))
                {
                    // SGDT, SIDT: what Windows XP shows user mode
                    auto limit = 0x3FFu;
                    auto base  = 0x8003F000u;
                    if (reg_ == 1)
                    {
                        limit = 0x7FF;
                        base  = 0x8003F400u;
                    }
                    store(ea_, 2, limit);
                    store(ea_ + 2, 4, base);
                }
                else if (reg_ == 4)
                {
                    writeRm(2, 0x003B); // SMSW: PE MP ET NE TS
                }
                else
                {
                    privileged();
                }
                break;
            case 0x0B:
                invalid();
            case 0x18:
            case 0x19:
            case 0x1A:
            case 0x1B:
            case 0x1C:
            case 0x1D:
            case 0x1E:
            case 0x1F:
                modrm(); // hint NOPs
                break;
            case 0x06:
            case 0x08:
            case 0x09:
            case 0x20:
            case 0x21:
            case 0x22:
            case 0x23:
            case 0x30:
            case 0x32:
                privileged();
            case 0x31:
            {
                // RDTSC counts instructions: deterministic, and monotonic
                const auto counter = rdtsc_;
                c_.r[kEax]         = static_cast<std::uint32_t>(counter);
                c_.r[kEdx]         = static_cast<std::uint32_t>(counter >> 32);
                break;
            }
            case 0xA0:
            case 0xA8:
                push(segmentSelector(4 + ((opcode >> 3) & 1)), sizev);
                break;
            case 0xA1:
            case 0xA9:
                pop(sizev);
                break;
            case 0xA2:
                cpuid();
                break;
            case 0xA3:
            case 0xAB:
            case 0xB3:
            case 0xBB:
            {
                modrm();
                const auto op = (opcode >> 3) & 3;
                bitTest(op, reg(reg_, sizev), false, sizev);
                break;
            }
            case 0xBA:
            {
                modrm();
                if (reg_ < 4)
                {
                    invalid();
                }
                const auto bit = fetch8();
                bitTest(reg_ - 4, bit, true, sizev);
                break;
            }
            case 0xA4:
            case 0xA5:
            case 0xAC:
            case 0xAD:
            {
                modrm();
                auto count = std::uint32_t{};
                if ((opcode & 1) == 0)
                {
                    count = fetch8();
                }
                else
                {
                    count = c_.r[kEcx] & 0xFF;
                }
                const auto dst    = readRm(sizev);
                const auto result = doubleShift(opcode < 0xA8, dst, reg(reg_, sizev), count, sizev);
                if ((count & 31) != 0)
                {
                    writeRm(sizev, result);
                }
                break;
            }
            case 0xAF:
                modrm();
                setReg(reg_, sizev, imul(reg(reg_, sizev), readRm(sizev), sizev));
                break;
            case 0xB0:
            case 0xB1:
            {
                modrm();
                auto size = sizev;
                if (opcode == 0xB0)
                {
                    size = 1;
                }
                const auto value = readRm(size);
                const auto acc   = reg(kEax, size);
                sub(acc, value, 0, size);
                if (acc == value)
                {
                    writeRm(size, reg(reg_, size));
                }
                else
                {
                    // The destination is written back either way, as the hardware does
                    writeRm(size, value);
                    setReg(kEax, size, value);
                }
                break;
            }
            case 0xB6:
            case 0xB7:
            case 0xBE:
            case 0xBF:
            {
                modrm();
                auto size = 1u;
                if (opcode & 1)
                {
                    size = 2;
                }
                auto value = readRm(size);
                if (opcode >= 0xBE)
                {
                    value = signExtend(value, size);
                }
                setReg(reg_, sizev, value);
                break;
            }
            case 0xBC:
            case 0xBD:
            {
                modrm();
                const auto value = readRm(sizev);
                if (value == 0)
                {
                    setFlag(kZF, true);
                    break;
                }
                setFlag(kZF, false);
                auto index = std::uint32_t{};
                if (opcode == 0xBC)
                {
                    index = static_cast<std::uint32_t>(std::countr_zero(value));
                }
                else
                {
                    index = 31 - static_cast<std::uint32_t>(std::countl_zero(value));
                }
                setReg(reg_, sizev, index);
                break;
            }
            case 0xC0:
            case 0xC1:
            {
                modrm();
                auto size = sizev;
                if (opcode == 0xC0)
                {
                    size = 1;
                }
                const auto dst    = readRm(size);
                const auto src    = reg(reg_, size);
                const auto result = add(dst, src, 0, size);
                setReg(reg_, size, dst);
                writeRm(size, result);
                break;
            }
            case 0xC7:
            {
                modrm();
                if (reg_ != 1 || !isMem_)
                {
                    invalid();
                }
                const auto low  = load(ea_, 4);
                const auto high = load(ea_ + 4, 4);
                if (low == c_.r[kEax] && high == c_.r[kEdx])
                {
                    store(ea_, 4, c_.r[kEbx]);
                    store(ea_ + 4, 4, c_.r[kEcx]);
                    setFlag(kZF, true);
                }
                else
                {
                    store(ea_, 4, low);
                    store(ea_ + 4, 4, high);
                    c_.r[kEax] = low;
                    c_.r[kEdx] = high;
                    setFlag(kZF, false);
                }
                break;
            }
            default:
                invalid();
        }
        finish();
    }

    void cpuid()
    {
        const auto leaf = c_.r[kEax];
        auto       a    = 0u;
        auto       b    = 0u;
        auto       c    = 0u;
        auto       d    = 0u;
        if (leaf == 0)
        {
            a = 1;
            b = 0x756E6547; // "Genu"
            d = 0x49656E69; // "ineI"
            c = 0x6C65746E; // "ntel"
        }
        else if (leaf == 1)
        {
            a = kCpuidSignature;
            d = kCpuidFeatures;
        }
        else if (leaf == 0x80000000)
        {
            a = 0x80000000;
        }
        c_.r[kEax] = a;
        c_.r[kEbx] = b;
        c_.r[kEcx] = c;
        c_.r[kEdx] = d;
    }

public:
    std::uint64_t rdtsc_ = 0;

private:
    Memory&       m_;
    Context&      c_;
    std::uint32_t start_     = 0;
    std::uint32_t pc_        = 0;
    std::uint32_t fastBytes_ = 0; // instruction bytes known to be mapped, from start_
    unsigned      opsize_    = 4;
    bool          addr16_    = false;
    std::uint32_t segment_   = 0;
    Rep           rep_       = Rep::None;
    std::uint8_t  modrmByte_ = 0;
    unsigned      mod_       = 0;
    unsigned      reg_       = 0;
    unsigned      rm_        = 0;
    bool          isMem_     = false;
    std::uint32_t ea_        = 0;
};

} // namespace

Cpu::Cpu(Memory& memory)
: memory_(memory)
{
}

auto Cpu::run(Context& context, const std::uint64_t budget) -> Event
{
    auto interpreter = Interpreter{ memory_, context };
    // The state before the current instruction, for precise faults. The try block wraps the whole loop: a try per
    // instruction costs the Wasm build its loop optimizations.
    auto registers = context.r;
    auto eflags    = context.eflags;
    auto eip       = context.eip;
    auto count     = std::uint64_t{};
    try
    {
        for (; count < budget; ++count)
        {
            if (traps_.contains(context.eip))
            {
                return Event{ Stop::Trap, context.eip };
            }
            registers          = context.r;
            eflags             = context.eflags;
            eip                = context.eip;
            interpreter.rdtsc_ = retired_;
            interpreter.execute();
            ++retired_;
            if (eflags & kTF)
            {
                // Single-step trap after the instruction; the hardware clears TF on entry to the handler
                return Event{ Stop::Interrupt, eip, 1 };
            }
        }
    }
    catch (const GuestFault& fault)
    {
        context.r      = registers;
        context.eflags = eflags;
        context.eip    = eip;
        return Event{ Stop::Fault, eip, 0, fault };
    }
    catch (const Interruption& interruption)
    {
        if (interruption.trap)
        {
            ++retired_;
            return Event{ Stop::Interrupt, interpreter.start(), interruption.vector };
        }
        context.r      = registers;
        context.eflags = eflags;
        context.eip    = eip;
        return Event{ Stop::Interrupt, eip, interruption.vector };
    }
    return Event{ Stop::Budget, context.eip };
}

auto Cpu::step(Context& context) -> Event
{
    return run(context, 1);
}

void Cpu::setTraps(const TrapRange traps)
{
    traps_ = traps;
}

auto Cpu::retired() const -> std::uint64_t
{
    return retired_;
}

} // namespace tmweb
