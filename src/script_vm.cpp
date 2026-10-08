#include "script_vm.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <utility>

namespace pusu::script {
namespace {
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559,
              "The recovered VM requires IEEE-754 binary32");
static_assert(std::numeric_limits<double>::digits == 53,
              "Pusu CRT selects 53-bit x87 precision");

std::uint32_t float_bits(float value) {
    std::uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}
std::uint32_t quiet_float(std::uint32_t bits) {
    if ((bits & 0x7f800000u) == 0x7f800000u && (bits & 0x007fffffu) != 0)
        bits |= 0x00400000u; // Masked x87 FLD quiets signaling NaNs.
    return bits;
}
float bits_float(std::uint32_t bits) {
    bits = quiet_float(bits);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}
std::int32_t signed_word(std::uint32_t bits) {
    const std::int64_t value = bits < 0x80000000u ? bits : std::int64_t(bits) - 0x100000000LL;
    return static_cast<std::int32_t>(value);
}
// Pusu uses the MSVC float-to-int64 truncation helper; opcode 32 keeps EAX only.
// Invalid FISTP int64 produces 80000000:00000000, hence a zero low word.
std::uint32_t truncate_word(float value) {
    const double wide = value;
    if (!std::isfinite(wide) || wide < -0x1p63 || wide >= 0x1p63) return 0;
    return static_cast<std::uint32_t>(static_cast<std::int64_t>(wide));
}
std::uint32_t float_arithmetic(Opcode opcode, std::uint32_t a, std::uint32_t b) {
    a = quiet_float(a);
    b = quiet_float(b);
    const bool nan_a = (a & 0x7fffffffu) > 0x7f800000u;
    const bool nan_b = (b & 0x7fffffffu) > 0x7f800000u;
    if (nan_a || nan_b) {
        // x87 selects the larger NaN significand, positive sign on a tie.
        // QEMU's x87 policy documents this independently of host ARM/SSE rules:
        // https://github.com/qemu/qemu/blob/v2.0.0/fpu/softfloat-specialize.h
        if (!nan_b) return a;
        if (!nan_a) return b;
        const auto magnitude_a = a & 0x7fffffffu, magnitude_b = b & 0x7fffffffu;
        if (magnitude_a != magnitude_b) return magnitude_a > magnitude_b ? a : b;
        return std::min(a, b);
    }
    const double left = bits_float(a), right = bits_float(b);
    double result;
    switch (opcode) {
    case Opcode::AddFloat: result = left + right; break;
    case Opcode::MultiplyFloat: result = left * right; break;
    case Opcode::SubtractFloat: result = left - right; break;
    case Opcode::DivideFloat: result = left / right; break;
    default: throw Error("Invalid floating arithmetic opcode");
    }
    // x87's invalid-operation indefinite is negative, even on hosts whose NaN is positive.
    return std::isnan(result) ? 0xffc00000u : float_bits(static_cast<float>(result));
}

constexpr std::array<const char*, 40> names{{
    "push_word", "push_float", "push_local", "push_indirect", "push_address", "push_string",
    "store_word", "store_float", "add_int", "multiply_int", "subtract_int", "divide_int",
    "add_float", "multiply_float", "subtract_float", "divide_float",
    "less_int", "less_equal_int", "equal_int", "not_equal_int", "greater_equal_int", "greater_int",
    "less_float", "less_equal_float", "equal_float", "not_equal_float", "greater_equal_float", "greater_float",
    "jump_if_zero", "jump_if_nonzero", "jump", "int_to_float", "float_to_int",
    "resolve_word", "resolve_float", "call", "and", "or", "negate_int", "negate_float"
}};
std::string checked_include(std::string name) {
    std::replace(name.begin(), name.end(), '\\', '/');
    if (name.empty() || name.front() == '/' || name.find(':') != std::string::npos ||
        name.find('\0') != std::string::npos) throw Error("Invalid PCS include name");
    std::size_t start = 0;
    while (start <= name.size()) {
        const auto end = name.find('/', start);
        const auto part = name.substr(start, end == std::string::npos ? end : end - start);
        if (part.empty() || part == "." || part == "..") throw Error("PCS include escapes script directory");
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return name;
}

class Reader {
public:
    Reader(const std::vector<std::uint8_t>& bytes, const std::string& source)
        : bytes_(bytes), source_(source) {}
    bool empty() const { return offset_ == bytes_.size(); }
    std::uint32_t word() {
        require(4);
        const auto* p = bytes_.data() + offset_;
        offset_ += 4;
        return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
               (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
    }
    std::string name() {
        const auto length = word();
        require(length);
        std::string value(reinterpret_cast<const char*>(bytes_.data() + offset_), length);
        offset_ += length;
        if (value.empty() || value.find('\0') != std::string::npos) fail("Invalid record name");
        return value;
    }
    std::vector<std::uint32_t> words() {
        const auto count = word();
        if (count > (bytes_.size() - offset_) / 4) fail("Truncated word block");
        std::vector<std::uint32_t> values;
        values.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) values.push_back(word());
        return values;
    }
    [[noreturn]] void fail(const std::string& reason) const {
        throw Error(source_ + ": byte " + std::to_string(offset_) + ": " + reason);
    }
private:
    void require(std::size_t count) const {
        if (count > bytes_.size() - offset_) fail("Truncated PCS record");
    }
    const std::vector<std::uint8_t>& bytes_;
    const std::string& source_;
    std::size_t offset_{};
};

void decode_function(Function& function, std::vector<std::uint32_t> code) {
    function.code_word_count = static_cast<std::uint32_t>(code.size());
    std::vector<std::size_t> boundaries(code.size() + 1, std::numeric_limits<std::size_t>::max());
    std::size_t ip = 0;
    auto fail = [&](const char* message) {
        throw Error(function.name + ": word " + std::to_string(ip) + ": " + message);
    };
    while (ip < code.size()) {
        const auto offset = ip;
        const auto op = code[ip++];
        if (op >= names.size()) fail("Invalid opcode");
        Instruction instruction;
        instruction.opcode = static_cast<Opcode>(op);
        instruction.word_offset = static_cast<std::uint32_t>(offset);
        boundaries[offset] = function.instructions.size();
        if (op <= 4 || (op >= 28 && op <= 30)) {
            if (ip == code.size()) fail("Missing instruction operand");
            instruction.argument = code[ip++];
            if (op >= 2 && op <= 4 && instruction.argument >= function.initial_data.size())
                fail("Data index outside function block");
        } else if (op == 5 || op == 35) {
            if (ip == code.size()) fail("Missing string word count");
            const auto count = code[ip++];
            if (count == 0 || count > code.size() - ip) fail("Truncated inline string");
            bool terminated = false;
            for (std::uint32_t i = 0; i < count && !terminated; ++i) {
                for (unsigned byte = 0; byte < 4; ++byte) {
                    const char character = static_cast<char>((code[ip + i] >> (byte * 8)) & 255);
                    if (character == '\0') { terminated = true; break; }
                    instruction.text.push_back(character);
                }
            }
            if (!terminated) fail("Unterminated inline string");
            // Shipped padding contains compiler/heap residue: it is NOT necessarily zero.
            ip += count;
        }
        function.instructions.push_back(std::move(instruction));
    }
    boundaries[code.size()] = function.instructions.size();
    for (auto& instruction : function.instructions) {
        const auto op = static_cast<std::uint32_t>(instruction.opcode);
        if (op >= 28 && op <= 30) {
            if (instruction.argument > code.size() ||
                boundaries[instruction.argument] == std::numeric_limits<std::size_t>::max())
                throw Error(function.name + ": jump does not target an instruction boundary");
            instruction.jump_target = boundaries[instruction.argument];
        }
    }
    function.code_words = std::move(code);
}
} // namespace

Program Program::decode(const std::vector<std::uint8_t>& bytes, std::string source) {
    Program program;
    program.source = std::move(source);
    Reader reader(bytes, program.source);
    while (!reader.empty()) {
        Record record;
        const auto type = reader.word();
        if (type > 1) reader.fail("Invalid record tag");
        record.include = type == 1;
        record.name = reader.name();
        if (record.include) {
            checked_include(record.name);
        } else {
            if (record.name.size() >= 64) reader.fail("Function name exceeds original 64-byte buffer");
            record.function.name = record.name;
            record.function.initial_data = reader.words();
            decode_function(record.function, reader.words());
        }
        program.records.push_back(std::move(record));
    }
    return program;
}
Program Program::read(std::istream& input, std::string source) {
    const auto start = input.tellg();
    if (start < 0) throw Error(source + ": PCS input must be seekable");
    input.seekg(0, std::ios::end);
    const auto end = input.tellg();
    if (end < start) throw Error(source + ": cannot determine PCS length");
    input.seekg(start);
    const auto size = static_cast<std::uint64_t>(end - start);
    if (size > std::numeric_limits<std::size_t>::max() ||
        size > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max()))
        throw Error(source + ": PCS input too large");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty() && !input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size)))
        throw Error(source + ": cannot read PCS input");
    return decode(bytes, std::move(source));
}
Program Program::read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw Error("Cannot open PCS: " + path.string());
    return read(input, path.string());
}
const char* opcode_name(Opcode opcode) {
    return names.at(static_cast<std::size_t>(opcode));
}
struct Runtime::Region {
    std::uint32_t base{};
    std::size_t byte_count{};
    RegionKind kind{};
    std::uint32_t address_extent{};
    std::vector<std::uint32_t> words;
    std::span<std::byte> external_storage;
};
struct Runtime::Callable {
    Host host;
    Function function;
    std::shared_ptr<Region> data;
    std::uint32_t cursor{};
    std::size_t instruction_index{};
};
Runtime::Runtime() = default;
Runtime::~Runtime() = default;
void Runtime::set_missing_call_diagnostic(std::function<void(std::string_view)> diagnostic) {
    missing_call_diagnostic_ = std::move(diagnostic);
}
void Runtime::swap_state(Runtime& other) {
    if (invocation_depth_ != 0 || other.invocation_depth_ != 0)
        throw Error("Runtime state exchange requires quiescent invocations");
    callables_.swap(other.callables_);
    ordered_callables_.swap(other.ordered_callables_);
    callable_order_.swap(other.callable_order_);
    regions_.swap(other.regions_);
    stack_.swap(other.stack_);
    std::swap(stack_size_, other.stack_size_);
    std::swap(next_address_, other.next_address_);
    std::swap(stop_requested_, other.stop_requested_);
    std::swap(cleanup_requested_, other.cleanup_requested_);
    missing_call_diagnostic_.swap(other.missing_call_diagnostic_);
}
std::shared_ptr<Runtime::Region> Runtime::allocate_region(std::size_t byte_count, RegionKind kind,
                                                        std::span<const std::uint32_t> initial_words,
                                                        std::span<std::byte> external_storage) {
    if (byte_count > 0xffffffffu) throw Error("VM region exceeds 32-bit address space");
    const std::uint64_t allocated = (std::uint64_t(byte_count) + 3) & ~std::uint64_t(3);
    const auto advance = std::max<std::uint64_t>(allocated, 4);
    std::uint64_t base = next_address_;
    if (advance > 0xffffffffu - base) {
        // Search holes only if the bump allocation would exhaust 32-bit addresses.
        base = 0x10000;
        for (const auto& entry : regions_) {
            if (advance <= std::uint64_t(entry.first) - base) break;
            base = std::uint64_t(entry.first) + entry.second->address_extent;
        }
        if (base + advance > 0xffffffffu) throw Error("VM address space exhausted");
    }
    auto region = std::make_shared<Region>();
    region->base = static_cast<std::uint32_t>(base);
    region->byte_count = byte_count;
    region->kind = kind;
    region->address_extent = static_cast<std::uint32_t>(advance);
    if (kind == RegionKind::External) region->external_storage = external_storage;
    else if (initial_words.empty()) region->words.resize(static_cast<std::size_t>(allocated / 4));
    else region->words.assign(initial_words.begin(), initial_words.end());
    next_address_ = std::max(next_address_, static_cast<std::uint32_t>(base + advance));
    regions_.emplace(region->base, region);
    return region;
}
const Runtime::Region& Runtime::region_at(std::uint32_t address, std::size_t bytes) const {
    auto it = regions_.upper_bound(address);
    if (it == regions_.begin()) throw Error("Invalid VM address");
    const auto& region = *std::prev(it)->second;
    const auto offset = std::uint64_t(address) - region.base;
    if (offset > region.byte_count || bytes > region.byte_count - offset)
        throw Error("VM access outside allocated region");
    return region;
}
Runtime::Region& Runtime::region_at(std::uint32_t address, std::size_t bytes) {
    return const_cast<Region&>(static_cast<const Runtime&>(*this).region_at(address, bytes));
}
std::uint32_t Runtime::bind_words(std::span<std::byte> native_storage) {
    if (native_storage.empty() || native_storage.size() % 4 != 0)
        throw Error("External VM storage must contain whole nonempty words");
    return allocate_region(native_storage.size(), RegionKind::External, {}, native_storage)->base;
}
void Runtime::unbind_words(std::uint32_t address) {
    const auto it = regions_.find(address);
    if (it == regions_.end() || it->second->kind != RegionKind::External)
        throw Error("Unbind requires the exact base of external VM storage");
    regions_.erase(it);
    reclaim_address_tail();
}
std::uint32_t Runtime::read_word(std::uint32_t address) const {
    const auto& region = region_at(address, 4);
    const auto offset = address - region.base;
    if (region.kind == RegionKind::External) {
        std::uint32_t value;
        std::memcpy(&value, region.external_storage.data() + offset, sizeof(value));
        return value;
    }
    if (offset % 4 == 0) return region.words[offset / 4];
    std::uint32_t word = 0;
    for (unsigned i = 0; i < 4; ++i) {
        const auto byte = offset + i;
        word |= ((region.words[byte / 4] >> ((byte % 4) * 8)) & 255) << (i * 8);
    }
    return word;
}
void Runtime::write_word(std::uint32_t address, std::uint32_t value) {
    auto& region = region_at(address, 4);
    const auto offset = address - region.base;
    if (region.kind == RegionKind::External) {
        std::memcpy(region.external_storage.data() + offset, &value, sizeof(value));
        return;
    }
    if (offset % 4 == 0) { region.words[offset / 4] = value; return; }
    for (unsigned i = 0; i < 4; ++i) {
        const auto byte = offset + i;
        auto& word = region.words[byte / 4];
        const auto shift = (byte % 4) * 8;
        word = (word & ~(std::uint32_t(255) << shift)) | (((value >> (i * 8)) & 255) << shift);
    }
}
std::uint32_t Runtime::allocate_string(std::string_view value) {
    const auto zero = value.find('\0');
    if (zero != std::string_view::npos) value = value.substr(0, zero);
    if (value.size() >= 0xffffffffu) throw Error("VM string too large");
    auto region = allocate_region(value.size() + 1, RegionKind::String);
    for (std::size_t i = 0; i < value.size(); ++i)
        region->words[i / 4] |= std::uint32_t(static_cast<unsigned char>(value[i])) << ((i % 4) * 8);
    return region->base;
}
std::string Runtime::string_at(std::uint32_t address) const {
    const auto& region = region_at(address, 1);
    if (region.kind == RegionKind::External) throw Error("External VM words are not a string");
    std::string value;
    for (std::size_t i = address - region.base; i < region.byte_count; ++i) {
        const char byte = static_cast<char>((region.words[i / 4] >> ((i % 4) * 8)) & 255);
        if (byte == '\0') return value;
        value.push_back(byte);
    }
    throw Error("Unterminated VM string");
}
std::uint32_t Runtime::duplicate_string(std::uint32_t address) {
    return allocate_string(string_at(address));
}
void Runtime::release_string(std::uint32_t address) {
    if (address == 0) return; // free(NULL), including missing saved strings.
    const auto it = regions_.find(address);
    if (it == regions_.end() || it->second->kind != RegionKind::String)
        throw Error("Invalid VM string release");
    regions_.erase(it);
    reclaim_address_tail();
}
void Runtime::push(std::uint32_t payload, std::uint32_t tag) {
    if (stack_size_ == stack_capacity) throw Error("Original VM operand stack capacity exceeded");
    stack_[stack_size_++] = {payload, tag};
}
Runtime::StackValue Runtime::pop() {
    if (stack_size_ == 0) throw Error("VM operand stack underflow");
    return stack_[--stack_size_];
}
std::uint32_t Runtime::pop_word() {
    const auto value = pop();
    if (value.tag != 0) throw Error("Host argument is an unresolved local reference");
    return value.payload;
}
std::int32_t Runtime::pop_int() { return signed_word(pop_word()); }
float Runtime::pop_float() { return bits_float(pop_word()); }
std::string Runtime::pop_string() {
    const auto address = pop_word();
    auto value = string_at(address);
    release_string(address);
    return value;
}
void Runtime::push_word(std::uint32_t value) { push(value, 0); }
void Runtime::push_int(std::int32_t value) { push_word(static_cast<std::uint32_t>(value)); }
void Runtime::push_float(float value) { push_word(float_bits(value)); }
std::uint32_t Runtime::push_string(std::string_view value) {
    if (stack_size_ == stack_capacity) throw Error("Original VM operand stack capacity exceeded");
    const auto address = allocate_string(value);
    push_word(address);
    return address;
}
std::size_t Runtime::stack_size() const noexcept { return stack_size_; }
void Runtime::stop_running_script() noexcept { stop_requested_ = true; }
void Runtime::reset_stack() { stack_size_ = 0; stop_requested_ = false; }
std::uint32_t Runtime::resolve(const Callable& function) {
    const auto value = pop();
    if (value.tag == 0) return value.payload;
    if (value.tag > 2 || !function.data || value.payload >= function.data->words.size())
        throw Error("Invalid local VM reference");
    const auto word = function.data->words[value.payload];
    return value.tag == 1 ? word : read_word(word);
}
bool Runtime::CallableNameLess::operator()(std::string_view left, std::string_view right) const noexcept {
    const auto fold = [](unsigned char value) {
        return value >= 'A' && value <= 'Z' ? static_cast<unsigned char>(value + ('a' - 'A')) : value;
    };
    const auto count = std::min(left.size(), right.size());
    for (std::size_t i = 0; i < count; ++i) {
        const auto a = fold(static_cast<unsigned char>(left[i]));
        const auto b = fold(static_cast<unsigned char>(right[i]));
        if (a != b) return a < b;
    }
    return left.size() < right.size();
}
void Runtime::add_callable(std::shared_ptr<Callable> callable) {
    callables_.insert_or_assign(callable->function.name, callable);
    ordered_callables_.insert(ordered_callables_.begin(), std::move(callable));
    callable_order_.insert(callable_order_.begin(), ordered_callables_.front()->function.name);
}
void Runtime::register_host(std::string name, Host host) {
    if (name.empty() || name.size() >= 64 || name.find('\0') != std::string::npos || !host)
        throw Error("Host callback requires a bounded name and implementation");
    auto callable = std::make_shared<Callable>();
    callable->function.name = std::move(name);
    callable->host = std::move(host);
    add_callable(std::move(callable));
}
void Runtime::load_records(const Program& program, const IncludeLoader& includes,
                           std::vector<std::string>& include_stack) {
    for (const auto& record : program.records) {
        if (record.include) {
            const auto name = checked_include(record.name);
            if (!includes) throw Error("PCS include requires a loader: " + name);
            if (std::find(include_stack.begin(), include_stack.end(), name) != include_stack.end())
                throw Error("Cyclic PCS include: " + name);
            include_stack.push_back(name);
            load_records(includes(name), includes, include_stack);
            include_stack.pop_back();
        } else {
            auto callable = std::make_shared<Callable>();
            callable->function.name = record.name;
            callable->function.instructions = record.function.instructions;
            callable->function.code_word_count = record.function.code_word_count;
            callable->function.code_words = record.function.code_words;
            callable->data = allocate_region(record.function.initial_data.size() * 4, RegionKind::Script,
                                             record.function.initial_data);
            add_callable(std::move(callable));
        }
    }
}
void Runtime::load(const Program& program, const IncludeLoader& includes) {
    std::vector<std::string> include_stack;
    load_records(program, includes, include_stack);
}
void Runtime::load_directory(const std::filesystem::path& directory, const std::string& name) {
    const auto root = std::filesystem::weakly_canonical(directory);
    IncludeLoader loader = [root](const std::string& script) {
        const auto relative = checked_include(script) + ".pcs";
        const auto path = std::filesystem::weakly_canonical(root / std::filesystem::path(relative));
        const auto contained = path.lexically_relative(root);
        if (contained.empty() || *contained.begin() == "..") throw Error("PCS path escapes script directory");
        return Program::read_file(path);
    };
    const auto script = checked_include(name);
    std::vector<std::string> include_stack{script};
    load_records(loader(script), loader, include_stack);
}
bool Runtime::contains(std::string_view name) const { return callables_.find(name) != callables_.end(); }
Runtime::Result Runtime::invoke_event(std::string_view base, std::string_view suffix) {
    if (suffix.empty()) return invoke(base);
    std::string name(base);
    name.append(suffix.data(), suffix.size()); // Event names concatenate without a separator.
    return invoke(name);
}
void Runtime::remove_callable(std::string_view name) {
    const auto it = callables_.find(name);
    if (it != callables_.end()) {
        const auto ordered = std::find(ordered_callables_.begin(), ordered_callables_.end(), it->second);
        const auto index = static_cast<std::size_t>(ordered - ordered_callables_.begin());
        callable_order_.erase(callable_order_.begin() + index);
        ordered_callables_.erase(ordered);
        callables_.erase(it); // Original hash deletion removes every case-insensitive duplicate binding.
        cleanup_requested_ = true;
        if (invocation_depth_ == 0) {
            collect_data_regions();
            cleanup_requested_ = false;
        }
    }
}
void Runtime::clear_callables() {
    callable_order_.clear();
    callables_.clear();
    ordered_callables_.clear();
    cleanup_requested_ = true;
    if (invocation_depth_ == 0) {
        collect_data_regions();
        cleanup_requested_ = false;
    }
}
void Runtime::collect_data_regions() {
    for (auto it = regions_.begin(); it != regions_.end();) {
        if (it->second->kind == RegionKind::Script && it->second.use_count() == 1) it = regions_.erase(it);
        else ++it;
    }
    reclaim_address_tail();
}
void Runtime::reclaim_address_tail() {
    next_address_ = regions_.empty() ? 0x10000u :
        regions_.rbegin()->first + regions_.rbegin()->second->address_extent;
}
std::uint32_t Runtime::data_address(std::string_view name) const {
    const auto it = callables_.find(name);
    if (it == callables_.end() || !it->second->data) throw Error("No script data block for callable");
    return it->second->data->base;
}
std::uint32_t Runtime::data_word(std::string_view name, std::size_t index) const {
    const auto it = callables_.find(name);
    if (it == callables_.end() || !it->second->data || index >= it->second->data->words.size())
        throw Error("Invalid script data index");
    return it->second->data->words[index];
}
Runtime::Result Runtime::invoke(std::string_view name) {
    const auto it = callables_.find(name);
    if (it == callables_.end()) return Result::Missing; // Exact original missing-event behavior.
    struct InvocationScope {
        Runtime& runtime;
        ~InvocationScope() {
            if (--runtime.invocation_depth_ == 0 && runtime.cleanup_requested_) {
                runtime.collect_data_regions();
                runtime.cleanup_requested_ = false;
            }
        }
    } scope{*this};
    ++invocation_depth_;
    // Retain native storage if a callback destroys the registry while this frame executes.
    auto callable = it->second;
    auto result = Result::Completed;
    if (callable->host) callable->host(*this); // Original native callback has no VM return value.
    else result = execute(callable);
    if (result == Result::Stopped) remove_callable(callable->function.name);
    return result;
}
Runtime::Result Runtime::execute(const std::shared_ptr<Callable>& function) {
    const auto& instructions = function->function.instructions;
    const auto& code = function->function.code_words;
    function->cursor = 0; // Pusu keeps this IP in the callable, including across reentry.
    function->instruction_index = 0;
    auto get = [&] { return resolve(*function); };
    auto get_float = [&] { return bits_float(get()); };
    while (function->cursor < code.size()) {
        if (function->instruction_index >= instructions.size() ||
            instructions[function->instruction_index].word_offset != function->cursor)
            throw Error("Shared VM instruction pointer is not an instruction boundary");
        const auto& instruction = instructions[function->instruction_index++];
        const auto op = instruction.opcode;
        function->cursor = op == Opcode::Call ? instruction.word_offset + 1 :
            function->instruction_index < instructions.size() ?
                instructions[function->instruction_index].word_offset : function->function.code_word_count;
        switch (op) {
        case Opcode::PushWord: push_word(instruction.argument); break;
        case Opcode::PushFloat: push_word(instruction.argument); break;
        case Opcode::PushLocal: push(instruction.argument, 1); break;
        case Opcode::PushIndirect: push(instruction.argument, 2); break;
        case Opcode::PushAddress: push_word(function->data->base + instruction.argument * 4); break;
        case Opcode::PushString: push_string(instruction.text); break;
        case Opcode::StoreWord: case Opcode::StoreFloat: {
            const auto destination = pop();
            if (destination.tag != 1 && destination.tag != 2)
                throw Error("Store destination is not a local/indirect reference");
            if (destination.payload >= function->data->words.size()) throw Error("Store data index out of bounds");
            auto value = get(); // Word and float are separate loads of the same raw bits.
            if (op == Opcode::StoreFloat) value = quiet_float(value);
            auto& word = function->data->words[destination.payload];
            if (destination.tag == 1) word = value;
            else write_word(word, value);
            break;
        }
        case Opcode::AddInt: case Opcode::MultiplyInt: case Opcode::SubtractInt:
        case Opcode::DivideInt: case Opcode::LessInt: case Opcode::LessEqualInt:
        case Opcode::EqualInt: case Opcode::NotEqualInt: case Opcode::GreaterEqualInt:
        case Opcode::GreaterInt: case Opcode::And: case Opcode::Or: {
            const auto a = get();
            const auto b = get();
            const auto sa = signed_word(a), sb = signed_word(b);
            std::uint32_t value = 0;
            switch (op) {
            case Opcode::AddInt: value = a + b; break;
            case Opcode::MultiplyInt: value = a * b; break;
            case Opcode::SubtractInt: value = a - b; break;
            case Opcode::DivideInt:
                if (sb == 0 || (sa == std::numeric_limits<std::int32_t>::min() && sb == -1))
                    throw Error("Original VM signed IDIV fault");
                value = static_cast<std::uint32_t>(sa / sb); break;
            case Opcode::LessInt: value = sa < sb; break;
            case Opcode::LessEqualInt: value = sa <= sb; break;
            case Opcode::EqualInt: value = a == b; break;
            case Opcode::NotEqualInt: value = a != b; break;
            case Opcode::GreaterEqualInt: value = sa >= sb; break;
            case Opcode::GreaterInt: value = sa > sb; break;
            case Opcode::And: value = a & b; break;
            case Opcode::Or: value = a | b; break;
            default: throw Error("Invalid integer instruction group");
            }
            push_word(value);
            break;
        }
        case Opcode::AddFloat: case Opcode::MultiplyFloat: case Opcode::SubtractFloat:
        case Opcode::DivideFloat: {
            const auto a = get();
            const auto b = get();
            push_word(float_arithmetic(op, a, b));
            break;
        }
        case Opcode::LessFloat: case Opcode::LessEqualFloat:
        case Opcode::EqualFloat: case Opcode::NotEqualFloat: case Opcode::GreaterEqualFloat:
        case Opcode::GreaterFloat: {
            const double a = get_float();
            const double b = get_float();
            // CRT sets x87 to 53 significand bits; each result is then stored binary32.
            // FUCOM status-word branches match ordered C++ comparisons, including NaNs.
            switch (op) {
            case Opcode::LessFloat: push_word(a < b); break;
            case Opcode::LessEqualFloat: push_word(a <= b); break;
            case Opcode::EqualFloat: push_word(a == b); break;
            case Opcode::NotEqualFloat: push_word(a != b); break;
            case Opcode::GreaterEqualFloat: push_word(a >= b); break;
            case Opcode::GreaterFloat: push_word(a > b); break;
            default: throw Error("Invalid floating instruction group");
            }
            break;
        }
        case Opcode::JumpIfZero: case Opcode::JumpIfNonzero: {
            const bool zero = get() == 0;
            if (zero == (op == Opcode::JumpIfZero)) {
                function->cursor = instruction.argument;
                function->instruction_index = instruction.jump_target;
            }
            break;
        }
        case Opcode::Jump:
            function->cursor = instruction.argument;
            function->instruction_index = instruction.jump_target;
            break;
        case Opcode::IntToFloat: push_float(static_cast<float>(signed_word(get()))); break;
        case Opcode::FloatToInt: push_word(truncate_word(get_float())); break;
        case Opcode::ResolveWord: push_word(get()); break;
        case Opcode::ResolveFloat: push_word(quiet_float(get())); break;
        case Opcode::Call:
            if (invoke(instruction.text) == Result::Missing && missing_call_diagnostic_)
                missing_call_diagnostic_(instruction.text);
            if (stop_requested_) {
                stop_requested_ = false; // Only the immediate script consumes it.
                return Result::Stopped;
            }
            // The original reads the string span from the CURRENT shared IP after return.
            // Reentry can change that IP; preserving raw words avoids inventing call frames.
            if (function->cursor >= code.size()) throw Error("Reentrant Call reads beyond bytecode");
            {
                const auto next = std::uint64_t(function->cursor) + code[function->cursor] + 1;
                if (next > code.size()) throw Error("Reentrant Call span leaves bytecode");
                function->cursor = static_cast<std::uint32_t>(next);
                const auto expected = function->instruction_index < instructions.size() ?
                    instructions[function->instruction_index].word_offset : function->function.code_word_count;
                if (function->cursor != expected) {
                    const auto target = std::lower_bound(instructions.begin(), instructions.end(), function->cursor,
                        [](const Instruction& value, std::uint32_t offset) { return value.word_offset < offset; });
                    function->instruction_index = static_cast<std::size_t>(target - instructions.begin());
                }
            }
            break;
        case Opcode::NegateInt: push_word(0u - get()); break;
        case Opcode::NegateFloat: push_word(quiet_float(get()) ^ 0x80000000u); break;
        default: throw Error("Invalid VM instruction");
        }
    }
    return Result::Completed;
}

} // namespace pusu::script
