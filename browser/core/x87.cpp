#include "core/x87.h"

#include <cmath>

namespace tmweb
{
namespace
{

using f80::F80;

enum Status : std::uint16_t
{
    kExceptionMask = 0x003F,
    kInvalid       = 0x0001,
    kStackFault    = 0x0040,
    kErrorSummary  = 0x0080,
    kC0            = 0x0100,
    kC1            = 0x0200,
    kC2            = 0x0400,
    kTopMask       = 0x3800,
    kC3            = 0x4000,
    kBusy          = 0x8000,
    kConditionMask = kC0 | kC1 | kC2 | kC3,
};

enum : std::uint16_t
{
    kSelectorCode = 0x1B,
    kSelectorData = 0x23,
};

class Fpu final
{
public:
    Fpu(Memory& memory, Context& context)
    : m_(memory)
    , c_(context)
    , f_(context.fpu)
    , env_(f80::envFromControl(context.fpu.control))
    {
    }

    auto execute(const std::uint32_t opcode, const X87Operand& operand, const std::uint32_t at) -> bool
    {
        const auto low = opcode & 7;
        if (!isControl(low, operand))
        {
            f_.lastInstruction = at;
            f_.lastOpcode      = static_cast<std::uint16_t>((low << 8) | operand.modrm);
            if (operand.memory)
            {
                f_.lastOperand = operand.address;
            }
        }
        if (operand.memory)
        {
            return memoryForm(low, operand);
        }
        return registerForm(low, operand.reg, operand.rm);
    }

private:
    static auto isControl(const std::uint32_t low, const X87Operand& operand) -> bool
    {
        if (operand.memory)
        {
            // FLDENV FLDCW FNSTENV FNSTCW, FRSTOR FNSAVE FNSTSW
            return (low == 1 && operand.reg >= 4) || (low == 5 && operand.reg >= 4);
        }
        // FNCLEX FNINIT, FNSTSW AX
        return (low == 3 && (operand.modrm == 0xE2 || operand.modrm == 0xE3)) || (low == 7 && operand.modrm == 0xE0);
    }

    // Stack
    auto top() const -> unsigned
    {
        return f_.top();
    }

    void setTop(const unsigned top)
    {
        f_.status = static_cast<std::uint16_t>((f_.status & ~kTopMask) | ((top & 7) << 11));
    }

    auto physical(const unsigned i) const -> unsigned
    {
        return (top() + i) & 7;
    }

    auto empty(const unsigned physicalIndex) const -> bool
    {
        return ((f_.tag >> (2 * physicalIndex)) & 3) == 3;
    }

    void setEmpty(const unsigned physicalIndex, const bool isEmpty)
    {
        f_.tag = static_cast<std::uint16_t>(f_.tag & ~(3u << (2 * physicalIndex)));
        if (isEmpty)
        {
            f_.tag = static_cast<std::uint16_t>(f_.tag | (3u << (2 * physicalIndex)));
        }
    }

    // ST(i), with stack underflow: the invalid exception and the indefinite NaN
    auto get(const unsigned i) -> F80
    {
        const auto index = physical(i);
        if (empty(index))
        {
            f_.status = static_cast<std::uint16_t>((f_.status | kInvalid | kStackFault) & ~kC1);
            return f80::indefinite();
        }
        return f_.st[index];
    }

    void set(const unsigned i, const F80 value)
    {
        const auto index = physical(i);
        f_.st[index]     = value;
        setEmpty(index, false);
    }

    void push(F80 value)
    {
        const auto index = (top() - 1) & 7;
        if (!empty(index))
        {
            f_.status = static_cast<std::uint16_t>(f_.status | kInvalid | kStackFault | kC1);
            value     = f80::indefinite();
        }
        setTop(index);
        f_.st[index] = value;
        setEmpty(index, false);
    }

    void pop()
    {
        setEmpty(physical(0), true);
        setTop(top() + 1);
    }

    // Fold this instruction's exceptions into the status word; C1 is "rounded up" for an inexact result
    void commit(const bool roundingSetsC1)
    {
        f_.status = static_cast<std::uint16_t>(f_.status | (env_.flags & kExceptionMask));
        if (roundingSetsC1)
        {
            f_.status = static_cast<std::uint16_t>(f_.status & ~kC1);
            if (env_.flags & f80::kRoundedUp)
            {
                f_.status = static_cast<std::uint16_t>(f_.status | kC1);
            }
        }
        if (f_.status & ~f_.control & kExceptionMask)
        {
            f_.status = static_cast<std::uint16_t>(f_.status | kErrorSummary | kBusy);
        }
    }

    void conditions(const std::uint16_t codes)
    {
        f_.status = static_cast<std::uint16_t>((f_.status & ~kConditionMask) | (codes & kConditionMask));
    }

    // Memory operands
    auto loadFloat(const std::uint32_t address, const unsigned bytes) -> F80
    {
        if (bytes == 4)
        {
            return f80::fromFloat(m_.read<std::uint32_t>(address), env_);
        }
        return f80::fromDouble(m_.read<std::uint64_t>(address), env_);
    }

    auto loadInt(const std::uint32_t address, const unsigned bytes) const -> F80
    {
        if (bytes == 2)
        {
            return f80::fromInt64(m_.read<std::int16_t>(address));
        }
        if (bytes == 4)
        {
            return f80::fromInt64(m_.read<std::int32_t>(address));
        }
        return f80::fromInt64(m_.read<std::int64_t>(address));
    }

    auto loadExtended(const std::uint32_t address) const -> F80
    {
        return F80{ m_.read<std::uint64_t>(address), m_.read<std::uint16_t>(address + 8) };
    }

    void storeExtended(const std::uint32_t address, const F80 value)
    {
        m_.check(address, 10, Access::Write);
        m_.write<std::uint64_t>(address, value.significand);
        m_.write<std::uint16_t>(address + 8, value.signExponent);
    }

    // The arithmetic operations by ModRM reg: FADD FMUL FCOM FCOMP FSUB FSUBR FDIV FDIVR, x = destination
    auto arithmetic(const unsigned op, const F80 x, const F80 y) -> F80
    {
        switch (op)
        {
            case 0:
                return f80::add(x, y, env_);
            case 1:
                return f80::mul(x, y, env_);
            case 4:
                return f80::sub(x, y, env_);
            case 5:
                return f80::sub(y, x, env_);
            case 6:
                return f80::div(x, y, env_);
            default:
                return f80::div(y, x, env_);
        }
    }

    void compareCodes(const f80::Order order)
    {
        switch (order)
        {
            case f80::Order::Greater:
                conditions(0);
                break;
            case f80::Order::Less:
                conditions(kC0);
                break;
            case f80::Order::Equal:
                conditions(kC3);
                break;
            default:
                conditions(kC0 | kC2 | kC3);
                break;
        }
    }

    void compareFlags(const f80::Order order)
    {
        c_.eflags &= ~(kZF | kPF | kCF | kOF | kSF | kAF);
        if (order == f80::Order::Less)
        {
            c_.eflags |= kCF;
        }
        else if (order == f80::Order::Equal)
        {
            c_.eflags |= kZF;
        }
        else if (order == f80::Order::Unordered)
        {
            c_.eflags |= kZF | kPF | kCF;
        }
        f_.status = static_cast<std::uint16_t>(f_.status & ~kC1);
    }

    auto memoryForm(const std::uint32_t low, const X87Operand& operand) -> bool
    {
        const auto address = operand.address;
        const auto op      = operand.reg;
        switch (low)
        {
            case 0: // D8: m32fp arithmetic
            case 4: // DC: m64fp arithmetic
            case 2: // DA: m32int arithmetic
            case 6: // DE: m16int arithmetic
            {
                auto y = F80{};
                if (low == 0)
                {
                    y = loadFloat(address, 4);
                }
                else if (low == 4)
                {
                    y = loadFloat(address, 8);
                }
                else if (low == 2)
                {
                    y = loadInt(address, 4);
                }
                else
                {
                    y = loadInt(address, 2);
                }
                const auto x = get(0);
                if (op == 2 || op == 3)
                {
                    compareCodes(f80::compare(x, y, false, env_));
                    commit(false);
                    if (op == 3)
                    {
                        pop();
                    }
                    return true;
                }
                set(0, arithmetic(op, x, y));
                commit(true);
                return true;
            }
            case 1: // D9
                switch (op)
                {
                    case 0:
                        push(loadFloat(address, 4));
                        commit(false);
                        return true;
                    case 2:
                    case 3:
                    {
                        const auto bits = f80::toFloat(get(0), env_);
                        m_.write<std::uint32_t>(address, bits);
                        commit(true);
                        if (op == 3)
                        {
                            pop();
                        }
                        return true;
                    }
                    case 4:
                        loadEnvironment(address);
                        return true;
                    case 5:
                        f_.control = m_.read<std::uint16_t>(address);
                        return true;
                    case 6:
                        storeEnvironment(address);
                        f_.control = static_cast<std::uint16_t>(f_.control | kExceptionMask);
                        return true;
                    case 7:
                        m_.write<std::uint16_t>(address, f_.control);
                        return true;
                    default:
                        return false;
                }
            case 3: // DB
                switch (op)
                {
                    case 0:
                        push(loadInt(address, 4));
                        return true;
                    case 2:
                    case 3:
                    {
                        const auto value = f80::toInt32(get(0), env_);
                        m_.write<std::int32_t>(address, value);
                        commit(true);
                        if (op == 3)
                        {
                            pop();
                        }
                        return true;
                    }
                    case 5:
                        push(loadExtended(address));
                        return true;
                    case 7:
                        storeExtended(address, get(0));
                        pop();
                        return true;
                    default:
                        return false; // FISTTP is SSE3
                }
            case 5: // DD
                switch (op)
                {
                    case 0:
                        push(loadFloat(address, 8));
                        commit(false);
                        return true;
                    case 2:
                    case 3:
                    {
                        const auto bits = f80::toDouble(get(0), env_);
                        m_.write<std::uint64_t>(address, bits);
                        commit(true);
                        if (op == 3)
                        {
                            pop();
                        }
                        return true;
                    }
                    case 4:
                        restore(address);
                        return true;
                    case 6:
                        save(address);
                        return true;
                    case 7:
                        m_.write<std::uint16_t>(address, f_.status);
                        return true;
                    default:
                        return false;
                }
            default: // DF
                switch (op)
                {
                    case 0:
                        push(loadInt(address, 2));
                        return true;
                    case 2:
                    case 3:
                    {
                        const auto value = f80::toInt16(get(0), env_);
                        m_.write<std::int16_t>(address, value);
                        commit(true);
                        if (op == 3)
                        {
                            pop();
                        }
                        return true;
                    }
                    case 4:
                        push(loadBcd(address));
                        return true;
                    case 5:
                        push(loadInt(address, 8));
                        return true;
                    case 6:
                        storeBcd(address, get(0));
                        commit(true);
                        pop();
                        return true;
                    case 7:
                    {
                        const auto value = f80::toInt64(get(0), env_);
                        m_.write<std::int64_t>(address, value);
                        commit(true);
                        pop();
                        return true;
                    }
                    default:
                        return false;
                }
        }
    }

    auto registerForm(const std::uint32_t low, const unsigned op, const unsigned i) -> bool
    {
        switch (low)
        {
            case 0: // D8: ST0 = ST0 op ST(i)
            {
                const auto x = get(0);
                const auto y = get(i);
                if (op == 2 || op == 3)
                {
                    compareCodes(f80::compare(x, y, false, env_));
                    commit(false);
                    if (op == 3)
                    {
                        pop();
                    }
                    return true;
                }
                set(0, arithmetic(op, x, y));
                commit(true);
                return true;
            }
            case 4: // DC: ST(i) = ST(i) op ST0, with the SUB/SUBR and DIV/DIVR encodings swapped
            case 6: // DE: the same, then pop
            {
                if (low == 6 && op == 3)
                {
                    if (i != 1)
                    {
                        return false;
                    }
                    // FCOMPP
                    compareCodes(f80::compare(get(0), get(1), false, env_));
                    commit(false);
                    pop();
                    pop();
                    return true;
                }
                if (op == 2 || op == 3)
                {
                    // FCOM2/FCOMP3/FCOMP5 aliases
                    compareCodes(f80::compare(get(0), get(i), false, env_));
                    commit(false);
                    if (op == 3 || low == 6)
                    {
                        pop();
                    }
                    return true;
                }
                auto swapped = op;
                if (op >= 4)
                {
                    swapped = op ^ 1;
                }
                set(i, arithmetic(swapped, get(i), get(0)));
                commit(true);
                if (low == 6)
                {
                    pop();
                }
                return true;
            }
            case 1:
                return d9(op, i);
            case 2: // DA: FCMOVcc, FUCOMPP
                if (op < 4)
                {
                    if (fcmovCondition(op))
                    {
                        set(0, get(i));
                    }
                    return true;
                }
                if (op == 5 && i == 1)
                {
                    compareCodes(f80::compare(get(0), get(1), true, env_));
                    commit(false);
                    pop();
                    pop();
                    return true;
                }
                return false;
            case 3: // DB
                if (op < 4)
                {
                    if (!fcmovCondition(op))
                    {
                        set(0, get(i));
                    }
                    return true;
                }
                if (op == 4)
                {
                    if (i == 2)
                    {
                        f_.status = static_cast<std::uint16_t>(f_.status & ~(kExceptionMask | kStackFault | kErrorSummary | kBusy));
                        return true;
                    }
                    if (i == 3)
                    {
                        initialize();
                        return true;
                    }
                    // FENI, FDISI, FSETPM are no-ops on a 387 and later
                    return i <= 4;
                }
                if (op == 5 || op == 6)
                {
                    compareFlags(f80::compare(get(0), get(i), op == 5, env_));
                    commit(false);
                    return true;
                }
                return false;
            case 5: // DD
                switch (op)
                {
                    case 0:
                        setEmpty(physical(i), true);
                        return true;
                    case 1: // FXCH4 alias
                        exchange(i);
                        return true;
                    case 2:
                    case 3:
                        set(i, get(0));
                        if (op == 3)
                        {
                            pop();
                        }
                        return true;
                    case 4:
                    case 5:
                        compareCodes(f80::compare(get(0), get(i), true, env_));
                        commit(false);
                        if (op == 5)
                        {
                            pop();
                        }
                        return true;
                    default:
                        return false;
                }
            default: // DF
                if (op == 0)
                {
                    // FFREEP
                    setEmpty(physical(i), true);
                    pop();
                    return true;
                }
                if (op == 4 && i == 0)
                {
                    c_.r[kEax] = (c_.r[kEax] & 0xFFFF0000) | f_.status;
                    return true;
                }
                if (op == 5 || op == 6)
                {
                    compareFlags(f80::compare(get(0), get(i), op == 5, env_));
                    commit(false);
                    pop();
                    return true;
                }
                if (op == 1)
                {
                    exchange(i);
                    return true;
                }
                if (op == 2 || op == 3)
                {
                    set(i, get(0));
                    pop();
                    return true;
                }
                return false;
        }
    }

    auto fcmovCondition(const unsigned op) const -> bool
    {
        switch (op)
        {
            case 0:
                return (c_.eflags & kCF) != 0;
            case 1:
                return (c_.eflags & kZF) != 0;
            case 2:
                return (c_.eflags & (kCF | kZF)) != 0;
            default:
                return (c_.eflags & kPF) != 0;
        }
    }

    void exchange(const unsigned i)
    {
        const auto a = get(0);
        const auto b = get(i);
        set(0, b);
        set(i, a);
        f_.status = static_cast<std::uint16_t>(f_.status & ~kC1);
    }

    auto d9(const unsigned op, const unsigned i) -> bool
    {
        if (op == 0)
        {
            push(get(i));
            return true;
        }
        if (op == 1)
        {
            exchange(i);
            return true;
        }
        if (op == 2)
        {
            return i == 0; // FNOP
        }
        if (op == 3)
        {
            // FSTP1 alias
            set(i, get(0));
            pop();
            return true;
        }
        const auto code = static_cast<unsigned>(0xC0 | (op << 3) | i);
        switch (code)
        {
            case 0xE0:
                set(0, f80::negate(get(0)));
                f_.status = static_cast<std::uint16_t>(f_.status & ~kC1);
                return true;
            case 0xE1:
                set(0, f80::abs(get(0)));
                f_.status = static_cast<std::uint16_t>(f_.status & ~kC1);
                return true;
            case 0xE4:
                compareCodes(f80::compare(get(0), f80::zero(), false, env_));
                commit(false);
                return true;
            case 0xE5:
                examine();
                return true;
            case 0xE8:
            case 0xE9:
            case 0xEA:
            case 0xEB:
            case 0xEC:
            case 0xED:
            case 0xEE:
                push(f80::constant(static_cast<std::uint8_t>(code - 0xE8), env_));
                return true;
            case 0xF0: // F2XM1
                set(0, viaDouble(std::exp2(f80::toHostDouble(get(0))) - 1.0));
                return true;
            case 0xF1: // FYL2X
            {
                const auto x = f80::toHostDouble(get(0));
                const auto y = f80::toHostDouble(get(1));
                set(1, viaDouble(y * std::log2(x)));
                pop();
                return true;
            }
            case 0xF2: // FPTAN
            {
                const auto x = f80::toHostDouble(get(0));
                if (std::fabs(x) >= 9223372036854775808.0)
                {
                    f_.status = static_cast<std::uint16_t>(f_.status | kC2);
                    return true;
                }
                set(0, viaDouble(std::tan(x)));
                push(f80::one());
                f_.status = static_cast<std::uint16_t>(f_.status & ~kC2);
                return true;
            }
            case 0xF3: // FPATAN
            {
                const auto x = f80::toHostDouble(get(0));
                const auto y = f80::toHostDouble(get(1));
                set(1, viaDouble(std::atan2(y, x)));
                pop();
                return true;
            }
            case 0xF4: // FXTRACT
            {
                auto       significand = F80{};
                const auto exponent    = f80::extract(get(0), significand, env_);
                set(0, exponent);
                push(significand);
                commit(false);
                return true;
            }
            case 0xF5:
            case 0xF8:
            {
                auto       codes     = std::uint16_t{};
                const auto remainder = f80::partialRemainder(get(0), get(1), code == 0xF5, env_, codes);
                set(0, remainder);
                conditions(codes);
                commit(false);
                return true;
            }
            case 0xF6:
                setTop(top() - 1);
                f_.status = static_cast<std::uint16_t>(f_.status & ~kC1);
                return true;
            case 0xF7:
                setTop(top() + 1);
                f_.status = static_cast<std::uint16_t>(f_.status & ~kC1);
                return true;
            case 0xF9: // FYL2XP1
            {
                const auto x = f80::toHostDouble(get(0));
                const auto y = f80::toHostDouble(get(1));
                set(1, viaDouble(y * std::log2(x + 1.0)));
                pop();
                return true;
            }
            case 0xFA:
                set(0, f80::sqrt(get(0), env_));
                commit(true);
                return true;
            case 0xFB: // FSINCOS
            {
                const auto x = f80::toHostDouble(get(0));
                if (std::fabs(x) >= 9223372036854775808.0)
                {
                    f_.status = static_cast<std::uint16_t>(f_.status | kC2);
                    return true;
                }
                set(0, viaDouble(std::sin(x)));
                push(viaDouble(std::cos(x)));
                f_.status = static_cast<std::uint16_t>(f_.status & ~kC2);
                return true;
            }
            case 0xFC:
                set(0, f80::roundToInt(get(0), env_));
                commit(true);
                return true;
            case 0xFD:
                set(0, f80::scale(get(0), get(1), env_));
                commit(true);
                return true;
            case 0xFE:
            case 0xFF:
            {
                const auto x = f80::toHostDouble(get(0));
                if (std::fabs(x) >= 9223372036854775808.0)
                {
                    f_.status = static_cast<std::uint16_t>(f_.status | kC2);
                    return true;
                }
                if (code == 0xFE)
                {
                    set(0, viaDouble(std::sin(x)));
                }
                else
                {
                    set(0, viaDouble(std::cos(x)));
                }
                f_.status = static_cast<std::uint16_t>(f_.status & ~kC2);
                return true;
            }
            default:
                return false;
        }
    }

    static auto viaDouble(const double value) -> F80
    {
        return f80::fromHostDouble(value);
    }

    void examine()
    {
        auto       codes = std::uint16_t{};
        const auto index = physical(0);
        if (f80::isNegative(f_.st[index]))
        {
            codes |= kC1;
        }
        auto kind = f80::Class::Empty;
        if (!empty(index))
        {
            kind = f80::classify(f_.st[index]);
        }
        switch (kind)
        {
            case f80::Class::NaN:
                codes |= kC0;
                break;
            case f80::Class::Normal:
                codes |= kC2;
                break;
            case f80::Class::Infinity:
                codes |= kC2 | kC0;
                break;
            case f80::Class::Zero:
                codes |= kC3;
                break;
            case f80::Class::Empty:
                codes |= kC3 | kC0;
                break;
            case f80::Class::Denormal:
                codes |= kC3 | kC2;
                break;
            default:
                break;
        }
        conditions(codes);
    }

    void initialize()
    {
        f_.control         = 0x037F;
        f_.status          = 0;
        f_.tag             = 0xFFFF;
        f_.lastInstruction = 0;
        f_.lastOperand     = 0;
        f_.lastOpcode      = 0;
    }

    // The full tag word: valid 0, zero 1, special 2, empty 3
    auto fullTag() const -> std::uint16_t
    {
        auto tag = std::uint16_t{};
        for (auto index = 0u; index < 8; ++index)
        {
            auto value = 3u;
            if (!empty(index))
            {
                const auto kind = f80::classify(f_.st[index]);
                if (kind == f80::Class::Zero)
                {
                    value = 1;
                }
                else if (kind == f80::Class::Normal)
                {
                    value = 0;
                }
                else
                {
                    value = 2;
                }
            }
            tag = static_cast<std::uint16_t>(tag | (value << (2 * index)));
        }
        return tag;
    }

    // FNSTENV's 28-byte protected-mode layout; the reserved upper halves read as ones, as on the hardware
    void storeEnvironment(const std::uint32_t address)
    {
        m_.check(address, 28, Access::Write);
        m_.write<std::uint32_t>(address, 0xFFFF0000u | f_.control);
        m_.write<std::uint32_t>(address + 4, 0xFFFF0000u | f_.status);
        m_.write<std::uint32_t>(address + 8, 0xFFFF0000u | fullTag());
        m_.write<std::uint32_t>(address + 12, f_.lastInstruction);
        m_.write<std::uint32_t>(address + 16, kSelectorCode | (static_cast<std::uint32_t>(f_.lastOpcode & 0x7FF) << 16));
        m_.write<std::uint32_t>(address + 20, f_.lastOperand);
        m_.write<std::uint32_t>(address + 24, 0xFFFF0000u | kSelectorData);
    }

    void loadEnvironment(const std::uint32_t address)
    {
        m_.check(address, 28, Access::Read);
        f_.control         = m_.read<std::uint16_t>(address);
        f_.status          = m_.read<std::uint16_t>(address + 4);
        const auto tag     = m_.read<std::uint16_t>(address + 8);
        f_.lastInstruction = m_.read<std::uint32_t>(address + 12);
        f_.lastOpcode      = static_cast<std::uint16_t>((m_.read<std::uint32_t>(address + 16) >> 16) & 0x7FF);
        f_.lastOperand     = m_.read<std::uint32_t>(address + 20);
        // Only empty or not is kept; the class is recomputed when stored
        f_.tag = 0;
        for (auto index = 0u; index < 8; ++index)
        {
            setEmpty(index, ((tag >> (2 * index)) & 3) == 3);
        }
    }

    void save(const std::uint32_t address)
    {
        m_.check(address, 108, Access::Write);
        storeEnvironment(address);
        for (auto i = 0u; i < 8; ++i)
        {
            storeExtended(address + 28 + 10 * i, f_.st[physical(i)]);
        }
        initialize();
    }

    void restore(const std::uint32_t address)
    {
        m_.check(address, 108, Access::Read);
        loadEnvironment(address);
        for (auto i = 0u; i < 8; ++i)
        {
            f_.st[physical(i)] = loadExtended(address + 28 + 10 * i);
        }
    }

    // Packed BCD: 18 digits and a sign byte
    auto loadBcd(const std::uint32_t address) const -> F80
    {
        auto value = std::int64_t{};
        for (auto index = 8; index >= 0; --index)
        {
            const auto byte = m_.read<std::uint8_t>(address + static_cast<std::uint32_t>(index));
            value           = value * 100 + (byte >> 4) * 10 + (byte & 15);
        }
        if (m_.read<std::uint8_t>(address + 9) & 0x80)
        {
            value = -value;
        }
        return f80::fromInt64(value);
    }

    void storeBcd(const std::uint32_t address, const F80 value)
    {
        auto       integer  = f80::toInt64(value, env_);
        const auto negative = integer < 0;
        if (negative)
        {
            integer = -integer;
        }
        m_.check(address, 10, Access::Write);
        for (auto index = 0u; index < 9; ++index)
        {
            const auto low = static_cast<std::uint8_t>(integer % 10);
            integer /= 10;
            const auto high = static_cast<std::uint8_t>(integer % 10);
            integer /= 10;
            m_.write<std::uint8_t>(address + index, static_cast<std::uint8_t>((high << 4) | low));
        }
        auto sign = std::uint8_t{};
        if (negative)
        {
            sign = 0x80;
        }
        m_.write<std::uint8_t>(address + 9, sign);
    }

    Memory&  m_;
    Context& c_;
    X87&     f_;
    f80::Env env_;
};

} // namespace

auto x87Execute(Memory& memory, Context& context, const std::uint32_t opcode, const X87Operand& operand, const std::uint32_t instructionAddress) -> bool
{
    auto fpu = Fpu{ memory, context };
    return fpu.execute(opcode, operand, instructionAddress);
}

} // namespace tmweb
