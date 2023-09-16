#include "process.hpp"
#include "utility/safe_handle.hpp"
#include "utility/string.hpp"

#include <charconv>
#include <vector>

#include <Windows.h>
#include <TlHelp32.h>

namespace process {
    std::uint32_t process_id = 0;

    utility::SafeHandle process_handle;

    std::optional<std::uint32_t> get_process_id_by_name(const std::string_view process_name) noexcept {
        const utility::SafeHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));

        if (snapshot.get() == INVALID_HANDLE_VALUE)
            return std::nullopt;

        PROCESSENTRY32 process_entry = {
            .dwSize = sizeof(PROCESSENTRY32)
        };

        for (Process32First(snapshot.get(), &process_entry); Process32Next(snapshot.get(), &process_entry);) {
            if (std::string_view(process_entry.szExeFile) == process_name)
                return process_entry.th32ProcessID;
        }

        return std::nullopt;
    }

    bool attach(const std::string_view process_name) noexcept {
        const auto id = get_process_id_by_name(process_name);

        if (!id.has_value())
            return false;

        process_id = id.value();

        process_handle = utility::SafeHandle(OpenProcess(PROCESS_ALL_ACCESS, 0, process_id));

        return process_handle.get() != nullptr;
    }

    std::optional<utility::Address> find_pattern(const std::string_view module_name, const std::string_view pattern) noexcept {
        constexpr auto pattern_to_bytes = [](const std::string_view pattern) {
            std::vector<std::int32_t> bytes;

            for (auto i = 0u; i < pattern.size(); ++i) {
                switch (pattern[i]) {
                    case '?':
                        bytes.push_back(-1);
                        break;

                    case ' ':
                        break;

                    default: {
                        if (i + 1 < pattern.size()) {
                            std::int32_t value = 0;

                            if (const auto [ptr, ec] = std::from_chars(pattern.data() + i, pattern.data() + i + 2, value, 16); ec == std::errc()) {
                                bytes.push_back(value);

                                ++i;
                            }
                        }

                        break;
                    }
                }
            }

            return bytes;
        };

        const auto module_base = get_module_base_by_name(module_name);

        if (!module_base.has_value())
            return std::nullopt;

        const auto headers = std::make_unique<std::uint8_t[]>(0x1000);

        if (!read_memory(module_base.value(), headers.get(), 0x1000))
            return std::nullopt;

        const auto dos_header = reinterpret_cast<PIMAGE_DOS_HEADER>(headers.get());

        if (dos_header->e_magic != IMAGE_DOS_SIGNATURE)
            return std::nullopt;

        const auto nt_headers = reinterpret_cast<PIMAGE_NT_HEADERS>(headers.get() + dos_header->e_lfanew);

        if (nt_headers->Signature != IMAGE_NT_SIGNATURE)
            return std::nullopt;

        const std::uint32_t module_size = nt_headers->OptionalHeader.SizeOfImage;

        const auto module_data = std::make_unique<std::uint8_t[]>(module_size);

        if (!read_memory(module_base.value(), module_data.get(), module_size))
            return std::nullopt;

        const auto pattern_bytes = pattern_to_bytes(pattern);

        for (auto i = 0u; i < module_size - pattern.size(); ++i) {
            bool found = true;

            for (auto j = 0u; j < pattern_bytes.size(); ++j) {
                if (module_data[i + j] != pattern_bytes[j] && pattern_bytes[j] != -1) {
                    found = false;

                    break;
                }
            }

            if (found)
                return utility::Address(module_base.value() + i);
        }

        return std::nullopt;
    }

    std::optional<std::uintptr_t> get_module_base_by_name(const std::string_view module_name) noexcept {
        const utility::SafeHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, process_id));

        if (snapshot.get() == INVALID_HANDLE_VALUE)
            return std::nullopt;

        MODULEENTRY32 module_entry = {
            .dwSize = sizeof(MODULEENTRY32)
        };

        for (Module32First(snapshot.get(), &module_entry); Module32Next(snapshot.get(), &module_entry);) {
            if (utility::string::equals_ignore_case(module_entry.szModule, module_name))
                return reinterpret_cast<std::uintptr_t>(module_entry.modBaseAddr);
        }

        return std::nullopt;
    }

    std::optional<std::uintptr_t> get_module_export_by_name(const std::uintptr_t module_base, const std::string_view function_name) noexcept {
        const auto headers = std::make_unique<std::uint8_t[]>(0x1000);

        if (!read_memory(module_base, headers.get(), 0x1000))
            return std::nullopt;

        const auto dos_header = reinterpret_cast<PIMAGE_DOS_HEADER>(headers.get());

        if (dos_header->e_magic != IMAGE_DOS_SIGNATURE)
            return std::nullopt;

        const auto nt_headers = reinterpret_cast<PIMAGE_NT_HEADERS>(headers.get() + dos_header->e_lfanew);

        if (nt_headers->Signature != IMAGE_NT_SIGNATURE)
            return std::nullopt;

        const IMAGE_DATA_DIRECTORY export_data_directory = nt_headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];

        if (export_data_directory.VirtualAddress == 0 || export_data_directory.Size == 0)
            return std::nullopt;

        const auto export_directory_buffer = std::make_unique<std::uint8_t[]>(export_data_directory.Size);

        if (!read_memory(module_base + export_data_directory.VirtualAddress, export_directory_buffer.get(), export_data_directory.Size))
            return std::nullopt;

        const auto export_directory = reinterpret_cast<PIMAGE_EXPORT_DIRECTORY>(export_directory_buffer.get());

        const std::uintptr_t delta = reinterpret_cast<std::uintptr_t>(export_directory) - export_data_directory.VirtualAddress;

        const auto name_table = reinterpret_cast<std::uint32_t*>(export_directory->AddressOfNames + delta);
        const auto name_ordinal_table = reinterpret_cast<std::uint16_t*>(export_directory->AddressOfNameOrdinals + delta);
        const auto function_table = reinterpret_cast<std::uint32_t*>(export_directory->AddressOfFunctions + delta);

        for (auto i = 0u; i < export_directory->NumberOfNames; ++i) {
            const std::string_view current_function_name = reinterpret_cast<const char*>(name_table[i] + delta);

            if (current_function_name != function_name)
                continue;

            const std::uint16_t function_ordinal = name_ordinal_table[i];
            const std::uintptr_t function_address = module_base + function_table[function_ordinal];

            if (function_address >= module_base + export_data_directory.VirtualAddress &&
                function_address <= module_base + export_data_directory.VirtualAddress + export_data_directory.Size
            ) {
                // TODO: Handle forwarded exports.

                return std::nullopt;
            }

            return function_address;
        }

        return std::nullopt;
    }

    std::optional<std::vector<std::string>> loaded_modules() noexcept {
        const utility::SafeHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, process_id));

        if (snapshot.get() == INVALID_HANDLE_VALUE)
            return std::nullopt;

        MODULEENTRY32 module_entry = {
            .dwSize = sizeof(MODULEENTRY32)
        };

        std::vector<std::string> loaded_modules;

        for (Module32First(snapshot.get(), &module_entry); Module32Next(snapshot.get(), &module_entry);)
            loaded_modules.emplace_back(module_entry.szModule);

        return loaded_modules;
    }

    bool read_memory(const std::uintptr_t address, void* buffer, const std::size_t size) noexcept {
        return ReadProcessMemory(process_handle.get(), reinterpret_cast<void*>(address), buffer, size, nullptr);
    }

    bool write_memory(const std::uintptr_t address, const void* buffer, const std::size_t size) noexcept {
        return WriteProcessMemory(process_handle.get(), reinterpret_cast<void*>(address), buffer, size, nullptr);
    }

    std::string read_string(const std::uintptr_t address, const std::size_t length) noexcept {
        std::string buffer(length, '\0');

        if (!read_memory(address, buffer.data(), length))
            return {};

        if (const auto it = std::ranges::find(buffer, '\0'); it != buffer.end())
            buffer.erase(it, buffer.end());

        return buffer;
    }
}
