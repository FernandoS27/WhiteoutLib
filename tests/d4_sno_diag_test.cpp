// Diagnostic test: parse a D4 SNO file and check for null values.
// Usage: d4_sno_diag_test

#include <whiteout/sno/sno_reader.h>
#include <whiteout/sno/sno_types.h>
#include <whiteout/sno/sno_value.h>

#include <cstring>
#include <iostream>
#include <vector>

using namespace whiteout;
using namespace whiteout::sno;

static void countValues(const SnoValue& v, int& total, int& nulls, int depth = 0) {
    ++total;
    if (v.isNull()) {
        ++nulls;
        return;
    }
    if (depth > 10) return; // prevent infinite recursion
    if (v.isObject()) {
        for (auto& [k, child] : v.asObject()) {
            countValues(child, total, nulls, depth + 1);
        }
    } else if (v.isArray()) {
        for (size_t i = 0; i < v.size(); ++i) {
            auto* e = v.at(i);
            if (e) countValues(*e, total, nulls, depth + 1);
        }
    }
}

int main() {
    // Build a minimal SNO binary: 0xDEADBEEF header + known format hash + payload.
    // Use format hash 123576590 (SoundTableDefinition, 5 fields, 72 bytes).
    //
    // Header: magic(4) + formatHash(4) + pad(8) = 16 bytes
    // Payload: 72 bytes of the root struct (all zeros for simplicity)
    constexpr u32 kMagic = 0xDEADBEEF;
    constexpr u32 kFmtHash = 123576590u;

    std::vector<u8> data(16 + 72, 0);
    std::memcpy(data.data(), &kMagic, 4);
    std::memcpy(data.data() + 4, &kFmtHash, 4);

    // Put a known integer value at payload offset 4 (past snoId at offset 0)
    // This tests whether basic types are read correctly.
    i32 testInt = 42;
    std::memcpy(data.data() + 16 + 4, &testInt, 4);

    float testFloat = 3.14f;
    std::memcpy(data.data() + 16 + 8, &testFloat, 4);

    SnoReader reader;
    auto file = reader.parse(data);

    if (!file) {
        std::cerr << "FAIL: parse returned nullopt\n";
        return 1;
    }

    std::cout << "Parse OK: type=" << file->typeName << "\n";

    // Check root
    if (file->root.isNull()) {
        std::cerr << "FAIL: root is NULL\n";
        return 1;
    }

    if (!file->root.isObject()) {
        std::cerr << "FAIL: root is not an object (type index: "
                  << static_cast<int>(file->root.type()) << ")\n";
        return 1;
    }

    // Print all top-level fields and their types
    const auto& obj = file->root.asObject();
    std::cout << "Root has " << obj.size() << " fields:\n";
    for (auto& [name, val] : obj) {
        std::cout << "  " << name << " = ";
        if (val.isNull()) std::cout << "NULL";
        else if (val.isBool()) std::cout << "bool(" << val.asBool() << ")";
        else if (val.isInt()) std::cout << "int(" << val.asInt() << ")";
        else if (val.isUint()) std::cout << "uint(" << val.asUint() << ")";
        else if (val.isFloat()) std::cout << "float(" << val.asFloat() << ")";
        else if (val.isString()) std::cout << "string(\"" << val.asString() << "\")";
        else if (val.isObject()) std::cout << "object(" << val.asObject().size() << " fields)";
        else if (val.isArray()) std::cout << "array(" << val.size() << " elems)";
        else if (val.isRef()) std::cout << "ref(group=" << val.asRef().group << ",id=" << val.asRef().snoId << ")";
        else std::cout << "other(type=" << static_cast<int>(val.type()) << ")";
        std::cout << "\n";
    }

    int total = 0, nulls = 0;
    countValues(file->root, total, nulls);
    std::cout << "\nTotal values: " << total << ", Nulls: " << nulls
              << " (" << (total > 0 ? 100.0 * nulls / total : 0) << "%)\n";

    return 0;
}
