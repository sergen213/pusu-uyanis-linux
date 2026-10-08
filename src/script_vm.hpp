#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace pusu::script {

class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Recovered Pusu jump-table order; operands are raw little-endian words, NOT floats.
// Binary operations consume the top operand FIRST: top - next, top / next, etc.
enum class Opcode : std::uint32_t {
    PushWord, PushFloat, PushLocal, PushIndirect, PushAddress, PushString,
    StoreWord, StoreFloat, AddInt, MultiplyInt, SubtractInt, DivideInt,
    AddFloat, MultiplyFloat, SubtractFloat, DivideFloat,
    LessInt, LessEqualInt, EqualInt, NotEqualInt, GreaterEqualInt, GreaterInt,
    LessFloat, LessEqualFloat, EqualFloat, NotEqualFloat, GreaterEqualFloat, GreaterFloat,
    JumpIfZero, JumpIfNonzero, Jump, IntToFloat, FloatToInt,
    ResolveWord, ResolveFloat, Call, And, Or, NegateInt, NegateFloat
};

struct Instruction {
    Opcode opcode{};
    std::uint32_t argument{};
    std::uint32_t word_offset{};
    std::size_t jump_target{}; // Decoded instruction index, including end-of-function.
    std::string text;         // Original name bytes; no locale/Unicode conversion.
};

struct Function {
    std::string name;
    std::vector<std::uint32_t> initial_data;
    std::vector<Instruction> instructions;
    std::vector<std::uint32_t> code_words; // Preserves inline padding and shared-IP call resumption.
    std::uint32_t code_word_count{};
};

struct Record {
    // PCS tag 0: function (name, data count/data, code count/code).
    // PCS tag 1: include (name only). There is no header or terminator.
    bool include{};
    std::string name;
    Function function;
};

struct Program {
    std::string source;
    std::vector<Record> records; // Order matters; Pusu resolves the most recently registered duplicate.
    static Program decode(const std::vector<std::uint8_t>& bytes, std::string source = {});
    static Program read(std::istream& input, std::string source = {});
    static Program read_file(const std::filesystem::path& path);
};

const char* opcode_name(Opcode opcode);
class Runtime {
public:
    using Host = std::function<void(Runtime&)>;
    using IncludeLoader = std::function<Program(const std::string&)>;
    enum class Result { Missing, Completed, Stopped };
    // Pusu VM stores 20 words between its stack base and stop byte: 10 pairs.
    static constexpr std::size_t stack_capacity = 10;

    Runtime();
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    // Commit an already loaded runtime for the same native game owner.
    // Both must be quiescent; active invocations throw before any state changes.
    // Borrowed bindings transfer as spans: caller also transfers/unbinds native storage.
    void swap_state(Runtime& other);
    // Internal bytecode CALL lookup failure only; optional invoke/events stay silent.
    // The game sink emits the recovered nonfatal warning to console (30s) and file.
    void set_missing_call_diagnostic(std::function<void(std::string_view)> diagnostic);

    // ASCII case-insensitive identity; newest duplicate wins. Removal unbinds all duplicates.
    void register_host(std::string name, Host host);
    void load(const Program& program, const IncludeLoader& includes = {});
    // Includes always resolve against this directory, not against the including file.
    // Pusu constructs level/pcs/<name>.pcs; pass that directory explicitly.
    void load_directory(const std::filesystem::path& directory, const std::string& name);
    bool contains(std::string_view name) const;
    // Original callable list is newest-first, including duplicate registrations.
    // Removing a name erases its newest list entry, retaining older owned entries unbound.
    // Views are invalidated by registry mutation.
    std::span<const std::string_view> ordered_callable_names() const noexcept { return callable_order_; }
    // Calls share the operand stack. Reentry resets the same callable's shared IP;
    // resumed accesses are checked, never replaced by independent recursive frames.
    Result invoke(std::string_view name);
    Result invoke_event(std::string_view base, std::string_view suffix);
    void remove_callable(std::string_view name);
    void clear_callables(); // Removes scripts AND hosts, independently of the stop byte.
    void stop_running_script() noexcept; // Stops and unregisters the next script completing Call.
    void reset_stack();     // Explicit portable reset, also acknowledges the stop byte.

    // Host callbacks accept tag-0 values only; these checked methods consume
    // one payload/tag pair. Word values preserve bits; no int/float numeric conversion.
    std::uint32_t pop_word();
    std::int32_t pop_int();
    float pop_float();
    std::string pop_string(); // Copies to native ownership and releases the VM allocation.
    void push_word(std::uint32_t value);
    void push_int(std::int32_t value);
    void push_float(float value);
    std::uint32_t push_string(std::string_view value);
    std::size_t stack_size() const noexcept;

    // Opcode PushString duplicates a C string. Handles are uniquely owned allocations;
    // raw word assignment transfers no ownership and does not retain or free strings.
    // pop_string is the consuming copy-and-free convenience for native callbacks.
    std::string string_at(std::uint32_t address) const;
    std::uint32_t allocate_string(std::string_view value);
    std::uint32_t duplicate_string(std::uint32_t address);
    void release_string(std::uint32_t address);

    // Portable checked 32-bit VM addresses, never truncated native pointers.
    // Alias nonempty caller-owned live storage, length a multiple of four bytes.
    // Storage must not move; unbind its exact base before destroying the native fields.
    // Registry clearing/GC retains external bindings. Destruction never frees caller bytes.
    std::uint32_t bind_words(std::span<std::byte> native_storage);
    void unbind_words(std::uint32_t address);
    std::uint32_t read_word(std::uint32_t address) const;
    void write_word(std::uint32_t address, std::uint32_t value);
    std::uint32_t data_address(std::string_view function) const;
    std::uint32_t data_word(std::string_view function, std::size_t index) const;

private:
    enum class RegionKind { Script, String, External };
    struct Region;
    struct Callable;
    struct StackValue { std::uint32_t payload; std::uint32_t tag; };
    struct CallableNameLess {
        using is_transparent = void;
        bool operator()(std::string_view left, std::string_view right) const noexcept;
    };
    std::map<std::string, std::shared_ptr<Callable>, CallableNameLess> callables_;
    std::vector<std::shared_ptr<Callable>> ordered_callables_;
    std::vector<std::string_view> callable_order_;
    std::map<std::uint32_t, std::shared_ptr<Region>> regions_;
    std::array<StackValue, stack_capacity> stack_{};
    std::size_t stack_size_{};
    std::uint32_t next_address_{0x10000};
    bool stop_requested_{};
    std::size_t invocation_depth_{};
    bool cleanup_requested_{};
    std::function<void(std::string_view)> missing_call_diagnostic_;

    std::shared_ptr<Region> allocate_region(std::size_t byte_count, RegionKind kind,
                                          std::span<const std::uint32_t> initial_words = {},
                                          std::span<std::byte> external_storage = {});
    const Region& region_at(std::uint32_t address, std::size_t bytes) const;
    Region& region_at(std::uint32_t address, std::size_t bytes);
    void push(std::uint32_t payload, std::uint32_t tag);
    StackValue pop();
    std::uint32_t resolve(const Callable& function);
    Result execute(const std::shared_ptr<Callable>& function);
    void load_records(const Program& program, const IncludeLoader& includes,
                      std::vector<std::string>& include_stack);
    void add_callable(std::shared_ptr<Callable> callable);
    void collect_data_regions();
    void reclaim_address_tail();
};

} // namespace pusu::script
