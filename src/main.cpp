#include "sdk/sdk.hpp"

#include <nlohmann/json.hpp>

#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>

void generate_header_for_type_scope(const sdk::CSchemaSystemTypeScope* type_scope) {
    if (type_scope == nullptr)
        return;

    const std::string output_file_name = std::format("generated/{}.hpp", type_scope->get_module_name());

    std::fstream output(output_file_name, std::ios::out);

    output << "#pragma once\n\n#include <cstddef>\n\n";

    for (const sdk::CSchemaType_DeclaredClass* declared_class : type_scope->get_declared_classes()) {
        if (declared_class == nullptr)
            continue;

        const sdk::CSchemaClassInfo* class_info = type_scope->find_declared_class(declared_class->get_class_name());

        if (class_info == nullptr)
            continue;

        output << "namespace " << declared_class->get_class_name() << " {\n";

        for (const sdk::SchemaClassFieldData_t* field : class_info->get_fields()) {
            if (field == nullptr)
                continue;

            output << "    constexpr std::ptrdiff_t " << field->get_name() << " = 0x" << std::hex << field->get_offset() << ";\n";
        }

        output << "}\n\n";

        spdlog::info("    > generated header file for {}", declared_class->get_class_name().c_str());
    }
}

void generate_json_for_type_scope(const sdk::CSchemaSystemTypeScope* type_scope) {
    if (type_scope == nullptr)
        return;

    const std::string output_file_name = std::format("generated/{}.json", type_scope->get_module_name());

    std::fstream output(output_file_name, std::ios::out);

    nlohmann::json json;

    for (const sdk::CSchemaType_DeclaredClass* declared_class : type_scope->get_declared_classes()) {
        if (declared_class == nullptr)
            continue;

        const sdk::CSchemaClassInfo* class_info = type_scope->find_declared_class(declared_class->get_class_name());

        if (class_info == nullptr)
            continue;

        for (const sdk::SchemaClassFieldData_t* field : class_info->get_fields()) {
            if (field == nullptr)
                continue;

            json[declared_class->get_class_name()][field->get_name()] = field->get_offset();
        }

        spdlog::info("    > generated json file for {}", declared_class->get_class_name().c_str());
    }

    output << json.dump(4);
}

std::uint64_t get_entity_list() noexcept {
    std::optional<std::uint64_t> address = process::find_pattern("client.dll", "48 8B 0D ? ? ? ? 48 89 7C 24 ? 8B FA C1 EB");

    if (!address.has_value())
        return 0;

    return process::resolve_rip_relative_address(address.value()).value_or(0);
}

std::uint64_t get_local_player() noexcept {
    std::optional<std::uint64_t> address = process::find_pattern("client.dll", "48 8B 0D ? ? ? ? F2 0F 11 44 24 ? F2 41 0F 10 00");

    if (!address.has_value())
        return 0;

    address = process::resolve_rip_relative_address(address.value());

    if (!address.has_value())
        return 0;

    return process::read_memory<std::uint64_t>(address.value()) + 0x50;
}

std::uint64_t get_view_matrix() noexcept {
    std::optional<std::uint64_t> address = process::find_pattern("client.dll", "48 8D 0D ? ? ? ? 48 C1 E0 06");

    if (!address.has_value())
        return 0;

    return process::resolve_rip_relative_address(address.value()).value_or(0);
}

void fetch_offsets() noexcept {
    const std::optional<std::uint64_t> client_base = process::get_module_base("client.dll");

    if (!client_base.has_value()) {
        spdlog::error("failed to get client.dll base.");

        return;
    }

    spdlog::info("entity list: {:#x}", get_entity_list() - client_base.value());
    spdlog::info("local player controller: {:#x}", get_local_player() - client_base.value());
    spdlog::info("view matrix: {:#x}", get_view_matrix() - client_base.value());
}

int main() {
    if (!std::filesystem::exists("generated"))
        std::filesystem::create_directory("generated");

    if (!process::attach("cs2.exe")) {
        spdlog::error("failed to attach to process.");

        return 1;
    }

    spdlog::info("attached to process!");

    const auto schema_system = sdk::CSchemaSystem::get();

    if (schema_system == nullptr) {
        spdlog::error("failed to get schema system.");

        return 1;
    }

    spdlog::info("schema system: {:#x}", reinterpret_cast<std::uint64_t>(schema_system));

    for (const sdk::CSchemaSystemTypeScope* type_scope : schema_system->get_type_scopes()) {
        spdlog::info("generating files for {}...", type_scope->get_module_name().c_str());

        generate_header_for_type_scope(type_scope);
        generate_json_for_type_scope(type_scope);
    }

    fetch_offsets();

    spdlog::info("done!");

    return 0;
}