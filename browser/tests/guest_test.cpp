// Guest code through the boundary and the SEH dispatcher, without game files
#include "tests/engine.h"

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <vector>

using namespace tmweb;

TEST_CASE("Guest code faults into its fs:[0] handler, resumes and calls stand-ins through an import slot" TMWEB_ENGINE)
{
    auto  machine = Machine{};
    auto& memory  = machine.memory();
    auto  calls   = std::map<std::string, int>{};
    machine.boundary().observe([&](const Call& call)
                               {
                                   ++calls[std::string{ call.name() }];
                               });
    machine.boundary().add(Function{ "kernel32.dll", "GetTickCount", Convention::Stdcall, 0, [](Call&) -> Outcome
                                     {
                                         return Return{ 1234 };
                                     } });
    machine.boundary().add(Function{ "tmweb", "done", Convention::Stdcall, 0, [](Call&) -> Outcome
                                     {
                                         return Halt{ "done" };
                                     } });
    machine.boundary().publish();
    auto& thread = machine.scheduler().createMain(0x10000);

    constexpr auto code    = std::uint32_t{ 0x10001000 };
    constexpr auto handler = std::uint32_t{ 0x10001100 };
    constexpr auto imports = std::uint32_t{ 0x10001200 };
    memory.map(code, 0x1000);
    memory.write<std::uint32_t>(imports, machine.boundary().resolve("kernel32.dll", "GetTickCount"));
    memory.write<std::uint32_t>(imports + 4, machine.boundary().resolve("tmweb", "done"));
    auto       program = std::vector<std::uint8_t>{};
    const auto emit    = [&](std::initializer_list<std::uint8_t> bytes)
    {
        program.insert(program.end(), bytes);
    };
    const auto emit32 = [&](const std::uint32_t value)
    {
        emit({ static_cast<std::uint8_t>(value), static_cast<std::uint8_t>(value >> 8), static_cast<std::uint8_t>(value >> 16), static_cast<std::uint8_t>(value >> 24) });
    };
    emit({ 0x68 }); // push handler
    emit32(handler);
    emit({ 0x64, 0xFF, 0x35, 0, 0, 0, 0 }); // push dword fs:[0]
    emit({ 0x64, 0x89, 0x25, 0, 0, 0, 0 }); // mov fs:[0], esp
    emit({ 0x31, 0xC0, 0x89, 0x00 });       // xor eax, eax; mov [eax], eax: faults
    emit({ 0xFF, 0x15 });                   // call [GetTickCount]
    emit32(imports);
    emit({ 0x89, 0xC3, 0xFF, 0x15 }); // mov ebx, eax; call [done]
    emit32(imports + 4);
    memory.write(code, std::span<const std::uint8_t>{ program });
    // mov eax, [esp+12]; add dword [eax+0xB8], 2; xor eax, eax; ret: skip the faulting instruction
    const auto skip = std::vector<std::uint8_t>{ 0x8B, 0x44, 0x24, 0x0C, 0x83, 0x80, 0xB8, 0, 0, 0, 2, 0x31, 0xC0, 0xC3 };
    memory.write(handler, std::span<const std::uint8_t>{ skip });

    thread.context.eip = code;
    thread.context.r[kEsp] -= 16;
    auto       engine = test::Engine{ machine };
    const auto result = engine.run(10000);
    CHECK(result.reason == "done");
    CHECK(result.exceptions == 1);
    CHECK(thread.context.r[kEbx] == 1234);
    CHECK(calls == std::map<std::string, int>{ { "kernel32.dll!GetTickCount", 1 }, { "ntdll.dll!KiUserExceptionDispatcher", 1 }, { "tmweb!done", 1 } });
}
