#include "script_vm.hpp"

#include <cassert>
#include <bit>
#include <filesystem>
#include <initializer_list>
#include <iostream>
#include <string_view>

using namespace pusu::script;
namespace {
void word(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) bytes.push_back(value >> shift);
}
Program function(std::string_view name, std::initializer_list<std::uint32_t> data,
                 std::initializer_list<std::uint32_t> code) {
    std::vector<std::uint8_t> bytes;
    word(bytes, 0); word(bytes, name.size());
    bytes.insert(bytes.end(), name.begin(), name.end());
    word(bytes, data.size()); for (auto value : data) word(bytes, value);
    word(bytes, code.size()); for (auto value : code) word(bytes, value);
    return Program::decode(bytes);
}
template<class F> void rejects(F operation) {
    bool rejected = false;
    try { operation(); } catch (const Error&) { rejected = true; }
    assert(rejected);
}
}

int main(int argc, char** argv) {
    Runtime vm;
    vm.load(function("arithmetic", {}, {0,3,0,11,10, 0,2,0,13,11,
                                       1,0x40000000,1,0x41200000,14,
                                       1,0x40000000,1,0x41400000,15}));
    assert(vm.invoke("arithmetic") == Runtime::Result::Completed);
    assert(vm.pop_float() == 6.0f); assert(vm.pop_float() == 8.0f);
    assert(vm.pop_int() == 6); assert(vm.pop_int() == 8);

    // Tag 1 is a local index, tag 2 dereferences its stored VM address.
    vm.load(function("pointer", {0,0}, {4,1,2,0,6, 0,41,2,1,6,
                                      0,99,3,0,6, 3,0,33, 4,1}));
    assert(vm.invoke("pointer") == Runtime::Result::Completed);
    const auto address = vm.pop_word();
    assert(vm.pop_word() == 99); assert(vm.read_word(address) == 99);
    assert(vm.data_word("pointer", 0) == address);
    rejects([&] { vm.read_word(address + 1); });
    rejects([&] { vm.write_word(address + 4, 1); });
    vm.load(function("tag", {7}, {2,0})); vm.invoke("tag");
    rejects([&] { vm.pop_word(); });
    vm.reset_stack();
    for (std::size_t i = 0; i < Runtime::stack_capacity; ++i) vm.push_word(i);
    rejects([&] { vm.push_word(10); });
    vm.reset_stack(); rejects([&] { vm.pop_word(); });
    const auto string = vm.push_string("owned");
    assert(vm.pop_string() == "owned"); rejects([&] { vm.string_at(string); });
    // Entity position pointers alias live native fields in both directions, not snapshots.
    {
        struct Position { float x, y, z; } position{1.0f, 2.0f, 3.0f};
        {
            Runtime live;
            auto bytes = std::as_writable_bytes(std::span(&position, 1));
            rejects([&] { live.bind_words({}); });
            rejects([&] { live.bind_words(bytes.first(3)); });
            const auto bound = live.bind_words(bytes);
            assert(position.x == 1.0f && position.y == 2.0f && position.z == 3.0f);
            live.load(function("position_store", {bound}, {1,0x41180000,3,0,7}));
            live.invoke("position_store"); assert(position.x == 9.5f);
            position.y = 42.25f;
            assert(live.read_word(bound + 4) == std::bit_cast<std::uint32_t>(position.y));
            live.write_word(bound + 8, std::bit_cast<std::uint32_t>(-7.0f));
            assert(position.z == -7.0f);
            rejects([&] { live.read_word(bound + 9); });
            rejects([&] { live.write_word(bound + 11, 0); });
            rejects([&] { live.string_at(bound); });
            rejects([&] { live.duplicate_string(bound); });
            rejects([&] { live.release_string(bound); });
            rejects([&] { live.unbind_words(bound + 4); });
            rejects([&] { live.unbind_words(live.data_address("position_store")); });
            live.clear_callables();
            assert(live.read_word(bound) == std::bit_cast<std::uint32_t>(9.5f));
            live.unbind_words(bound);
            rejects([&] { live.read_word(bound); });
            rejects([&] { live.write_word(bound, 0); });
            rejects([&] { live.unbind_words(bound); });
            assert(position.x == 9.5f && position.y == 42.25f && position.z == -7.0f);
            live.bind_words(bytes); // Destruction releases metadata, never caller storage.
        }
        assert(position.x == 9.5f && position.y == 42.25f && position.z == -7.0f);
    }

    vm.load(function("DuPlIcAtE", {1}, {2,0,33}));
    const auto older_data = vm.data_address("duplicate");
    vm.load(function("DUPLICATE", {2}, {2,0,33}));
    const auto newer_data = vm.data_address("duPLICate");
    assert(vm.ordered_callable_names().front() == "DUPLICATE");
    assert(vm.data_word("dUpLiCaTe", 0) == 2 && vm.contains("duplicate"));
    vm.invoke("duplicate"); assert(vm.pop_word() == 2);
    vm.remove_callable("duPLICate");
    assert(vm.invoke("DUPLICATE") == Runtime::Result::Missing && !vm.contains("duplicate"));
    assert(vm.ordered_callable_names().front() == "DuPlIcAtE");
    assert(vm.read_word(older_data) == 1); // Original older list entry still owns its data.
    rejects([&] { vm.read_word(newer_data); });
    rejects([&] { vm.data_address("duplicate"); });
    Program included;
    Record include; include.include = true; include.name = "child";
    included.records.push_back(include);
    vm.load(included, [](const std::string&) { return function("child", {23}, {2,0,33}); });
    vm.invoke("child"); assert(vm.pop_word() == 23);
    const auto old_data = vm.data_address("child");
    vm.clear_callables(); rejects([&] { vm.read_word(old_data); });
    rejects([&] { vm.read_word(older_data); }); // Clearing also releases retained unbound entries.
    assert(vm.invoke("child") == Runtime::Result::Missing);
    vm.reset_stack();
    // Native control callbacks exercise real registry/lifetime/stop behavior, not game mocks.
    vm.register_host("clear", [](Runtime& runtime) { runtime.clear_callables(); });
    vm.load(function("survive_clear", {29}, {35,2,0x61656c63,0x72, 2,0,33}));
    assert(vm.invoke("survive_clear") == Runtime::Result::Completed);
    assert(vm.pop_word() == 29); assert(!vm.contains("survive_clear"));
    vm.register_host("Stop", [](Runtime& runtime) { runtime.stop_running_script(); });
    vm.load(function("Stopped", {}, {0,444}));
    vm.load(function("stopped", {}, {35,2,0x706f7473,0, 0,123}));
    assert(vm.invoke("STOPPED") == Runtime::Result::Stopped);
    assert(!vm.contains("stopped")); assert(vm.stack_size() == 0);
    assert(vm.invoke("Stopped") == Runtime::Result::Missing); // Stopping cannot revive the older duplicate.
    unsigned reentries = 0;
    vm.register_host("reenter", [&](Runtime& runtime) {
        if (++reentries == 1) runtime.invoke("reentrant");
    });
    vm.load(function("reentrant", {}, {35,2,0x6e656572,0x00726574, 0,7}));
    rejects([&] { vm.invoke("reentrant"); }); // Inner completes; outer shared-IP read is past EOF.
    assert(reentries == 2 && vm.pop_word() == 7); // Reentry actually ran, it was not rejected.
    // A nested stop leaves the shared IP on Call's span, so the outer call resumes there.
    reentries = 0;
    vm.register_host("reenter", [&](Runtime& runtime) {
        if (++reentries == 1) runtime.invoke("reentrant");
        else runtime.stop_running_script();
    });
    assert(vm.invoke("reentrant") == Runtime::Result::Completed);
    assert(reentries == 2 && vm.pop_word() == 7 && !vm.contains("reentrant"));
    vm.clear_callables(); vm.reset_stack();

    // PushFloat uses MOVSS: do not quiet a signaling NaN until a float load resolves it.
    vm.load(function("float_bits", {}, {1,0x7f800001}));
    vm.invoke("float_bits"); assert(vm.pop_word() == 0x7f800001);
    rejects([&] { Program::decode({0,0,0}); });
    rejects([&] { function("bad_pointer", {0}, {4,1}); });
    rejects([&] { function("bad_jump", {}, {30,1}); });
    // Internal missing CALL warns without consuming operands, returning a value or aborting.
    {
        Runtime calls;
        std::size_t diagnostics = 0;
        calls.set_missing_call_diagnostic([&](std::string_view) { ++diagnostics; });
        calls.load(function("continuation", {0},
                            {0,11,35,2,0x65736261,0x0000746e,0,7,8,2,0,6}));
        calls.push_int(99);
        assert(calls.invoke("continuation") == Runtime::Result::Completed);
        assert(diagnostics == 1 && calls.data_word("continuation", 0) == 18);
        assert(calls.stack_size() == 1 && calls.pop_int() == 99);
        assert(calls.contains("continuation"));
        assert(calls.invoke("absent") == Runtime::Result::Missing);
        assert(calls.invoke_event("absent", "_on_use") == Runtime::Result::Missing);
        assert(diagnostics == 1 && calls.stack_size() == 0);
        calls.load(function("leaf", {}, {0,3}));
        calls.load(function("defined_call", {}, {35,2,0x6661656c,0,0,2,8}));
        assert(calls.invoke("defined_call") == Runtime::Result::Completed);
        assert(calls.pop_word() == 5 && diagnostics == 1);
    }
    // Quiescent scene commit transfers real storage/handles, not bytecode snapshots.
    {
        float old_position = 3.0f, new_position = 4.0f;
        Runtime old_state, new_state;
        std::size_t old_diagnostics = 0, new_diagnostics = 0;
        old_state.set_missing_call_diagnostic([&](std::string_view) { ++old_diagnostics; });
        new_state.set_missing_call_diagnostic([&](std::string_view) { ++new_diagnostics; });
        old_state.load(function("leaf", {1}, {2,0,33}));
        old_state.load(function("stopper", {}, {35,2,0x6661656c,0}));
        new_state.load(function("new_scene", {2,9}, {2,0,33}));
        const auto old_alias = old_state.bind_words(std::as_writable_bytes(std::span(&old_position,1)));
        const auto new_alias = new_state.bind_words(std::as_writable_bytes(std::span(&new_position,1)));
        old_state.push_string("old-owned");
        new_state.push_int(17); new_state.push_string("new-owned");
        old_state.stop_running_script();
        old_state.swap_state(new_state);
        assert(old_state.contains("new_scene") && !old_state.contains("leaf"));
        assert(new_state.contains("leaf") && !new_state.contains("new_scene"));
        assert(old_state.ordered_callable_names().front() == "new_scene");
        old_state.register_host("attempt_commit", [&](Runtime& active) {
            rejects([&] { active.swap_state(new_state); });
            rejects([&] { new_state.swap_state(active); });
            assert(active.contains("new_scene") && !active.contains("leaf"));
            assert(new_state.contains("leaf") && !new_state.contains("new_scene"));
        });
        old_state.push_int(5); new_state.push_int(6);
        assert(old_state.invoke("attempt_commit") == Runtime::Result::Completed);
        assert(old_state.pop_int() == 5 && new_state.pop_int() == 6);
        assert(old_state.pop_string() == "new-owned" && old_state.pop_int() == 17);
        assert(new_state.invoke("stopper") == Runtime::Result::Stopped);
        assert(new_state.pop_word() == 1 && new_state.pop_string() == "old-owned");
        assert(old_state.read_word(new_alias) == std::bit_cast<std::uint32_t>(4.0f));
        new_state.write_word(old_alias, std::bit_cast<std::uint32_t>(9.0f));
        assert(old_position == 9.0f);
        new_state.clear_callables();
        assert(new_state.read_word(old_alias) == std::bit_cast<std::uint32_t>(9.0f));
        old_state.unbind_words(new_alias); new_state.unbind_words(old_alias);
        old_state.load(function("diagnostic_commit", {0}, {35,2,0x65736261,0x0000746e,0,23,2,0,6}));
        assert(old_state.invoke("diagnostic_commit") == Runtime::Result::Completed);
        assert(new_diagnostics == 1 && old_diagnostics == 0);
        assert(old_state.data_word("diagnostic_commit", 0) == 23);
    }

    // Optional immutable media coverage, independent of any game-host implementation.
    if (argc == 2) {
        std::size_t files = 0, functions = 0;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(argv[1])) {
            if (!entry.is_regular_file() || entry.path().extension() != ".pcs") continue;
            const auto program = Program::read_file(entry.path());
            ++files;
            for (const auto& record : program.records) functions += !record.include;
        }
        assert(files == 30 && functions == 4435);
        std::cout << "Decoded " << files << " PCS files, " << functions
                  << " functions. No game-native callbacks invoked.\n";
    }
}
