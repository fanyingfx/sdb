#include "libsdb/breakpoint_site.hpp"
#include "libsdb/register_info.hpp"
#include "libsdb/types.hpp"
#include <algorithm>
#include <cstdint>
#include <editline/readline.h>
#include <fmt/base.h>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <iostream>
#include <libsdb/error.hpp>
#include <libsdb/parse.hpp>
#include <libsdb/process.hpp>
#include <sstream>
#include <stdexcept>
#include <string.h>
#include <string>
#include <string_view>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <type_traits>
#include <unistd.h>
#include <variant>
#include <vector>

namespace {
std::vector<std::string> split(std::string_view str, char delimiter) {
    std::vector<std::string> out{};
    std::stringstream        ss{std::string{str}};
    std::string              item;
    while (std::getline(ss, item, delimiter)) {
        out.push_back(item);
    }
    return out;
}
bool is_prefix(std::string_view str, std::string_view of) {
    if (str.size() > of.size()) return false;
    return std::equal(str.begin(), str.end(), of.begin());
}
void wait_on_signal(pid_t pid) {
    int wait_status;
    int options = 0;
    if (waitpid(pid, &wait_status, options) < 0) {
        std::perror("waitpid failed");
        std::exit(-1);
    }
}
void print_stop_reason(const sdb::process& process, sdb::stop_reason reason) {
    std::string message;
    switch (reason.reason) {
    case sdb::process_state::exited:
        message = fmt::format("exited with status {}", static_cast<int>(reason.info));
        break;
    case sdb::process_state::terminated:
        message = fmt::format("terminated with signal {}", sigabbrev_np(reason.info));
        break;
    case sdb::process_state::stopped:
        message = fmt::format("stopped with signal {} at {:#x}", sigabbrev_np(reason.info), process.get_pc().addr());
        break;

    case sdb::process_state::running: throw std::runtime_error("Process is not stopped!"); break;
    }
    fmt::print("Process {} {}\n", process.pid(), message);
}
void print_help(const std::vector<std::string>& args) {
    if (args.size() == 1) {
        std::cerr << R"(Available commands:
breakpoint  - Commands for operating on breakpoints
continue    - Resume the process
register    - Commands for operating on registers
step        - Step over a single instruction
)";
    } else if (is_prefix(args[1], "register")) {
        std::cerr << R"(Available commands:
read
read <register>
read all
write <register> <value>
)";
    } else if (is_prefix(args[1], "breakpoint")) {
        std::cerr << R"(Avaiable commands:
list
delete <id>
disable <id>
enable <id>
set <address>
)";
    }
}
void handle_register_read(sdb::process& process, const std::vector<std::string>& args) {
    auto format = [](auto t) {
        if constexpr (std::is_floating_point_v<decltype(t)>) {
            return fmt::format("{}", t);
        } else if constexpr (std::is_integral_v<decltype(t)>) {
            return fmt::format("{:#0{}x}", t, sizeof(t) * 2 + 2);
        } else {
            return fmt::format("[{:#04x}]", fmt::join(t, ","));
        }
    };
    if (args.size() == 2 or (args.size() == 3 and args[2] == "all")) {
        for (auto& info : sdb::g_register_infos) {
            auto should_print = (args.size() == 3 or info.type == sdb::register_type::gpr) and info.name != "orig_rax";
            if (!should_print) continue;
            auto value = process.get_registers().read(info);
            fmt::print("{}:\t{}\n", info.name, std::visit(format, value));
        }
    } else if (args.size() == 3) {
        try {
            auto info  = sdb::register_info_by_name(args[2]);
            auto value = process.get_registers().read(info);
            fmt::print("{}:\t{}\n", info.name, std::visit(format, value));
        } catch (sdb::error& err) {
            std::cerr << "No such register\n";
            return;
        }
    } else {
        print_help({"help", "register"});
    }
}

sdb::registers::value parse_register_value(sdb::register_info info, std::string_view text) {
    try {
        if (info.format == sdb::register_format::uint) {
            switch (info.size) {
            case 1: return sdb::to_integral<std::uint8_t>(text, 16).value();
            case 2: return sdb::to_integral<std::uint16_t>(text, 16).value();
            case 4: return sdb::to_integral<std::uint32_t>(text, 16).value();
            case 8: return sdb::to_integral<std::uint64_t>(text, 16).value();
            }
        } else if (info.format == sdb::register_format::double_float) {
            return sdb::to_float<double>(text).value();
        } else if (info.format == sdb::register_format::long_double) {
            return sdb::to_float<long double>(text).value();
        } else if (info.format == sdb::register_format::vector) {
            if (info.size == 8) {
                return sdb::parse_vector<8>(text);
            } else if (info.size == 16) {
                return sdb::parse_vector<16>(text);
            }
        }
    } catch (...) {}
    sdb::error::send("Invalid format");
}
void handle_register_write(sdb::process& process, const std::vector<std::string>& args) {
    if (args.size() != 4) {
        print_help({"help", "register"});
        return;
    }
    try {
        auto info  = sdb::register_info_by_name(args[2]);
        auto value = parse_register_value(info, args[3]);
        process.get_registers().write(info, value);
    } catch (sdb::error& err) {
        std::cerr << err.what() << "\n";
        return;
    }
}
void handle_register_command(sdb::process& process, const std::vector<std::string>& args) {
    if (args.size() < 2) {
        print_help({"help", "register"});
        return;
    }
    if (is_prefix(args[1], "read")) {
        handle_register_read(process, args);
    } else if (is_prefix(args[1], "write")) {
        handle_register_write(process, args);
    } else {
        print_help({"help", "register"});
    }
}
void handle_breakpoint_command(sdb::process& process, const std::vector<std::string>& args) {
    if (args.size() < 2) {
        print_help({"help", "breakpoint"});
        return;
    }
    auto command = args[1];
    if (is_prefix(command, "list")) {
        if (process.breakpoint_sites().empty()) {
            fmt::print("No breakpoints set\n");
        } else {
            fmt::print("Current breakpoints:\n");
            process.breakpoint_sites().for_each([](auto& site) {
                fmt::print("{}: address = {:#x},{}\n",
                           site.id(),
                           site.address().addr(),
                           site.is_enable() ? "enabled" : "disabled");
            });
        }
        return;
    }
    if (args.size() < 3) {
        print_help({"help", "breakpoint"});
        return;
    }
    if (is_prefix(command, "set")) {
        auto address = sdb::to_integral<std::uint64_t>(args[2], 16);
        if (!address) {
            fmt::print(stderr,
                       "Breakpoint command expects address in "
                       "hexadecimal, prefixed with '0x'\n");
            return;
        }
        process.create_breakpoint_site(sdb::virt_addr{*address}).enable();
        return;
    }
    auto id = sdb::to_integral<sdb::breakpoint_site::id_type>(args[2]);
    if (!id) {
        std::cerr << "Command expects breakpoint id";
        return;
    }
    if (is_prefix(command, "enable")) {
        process.breakpoint_sites().get_by_id(*id).enable();
    } else if (is_prefix(command, "disable")) {
        process.breakpoint_sites().get_by_id(*id).disable();
    } else if (is_prefix(command, "delte")) {
        process.breakpoint_sites().remove_by_id(*id);
    }
}
void handle_command(std::unique_ptr<sdb::process>& process, std::string_view line) {
    auto args    = split(line, ' ');
    auto command = args[0];
    if (is_prefix(command, "continue")) {
        process->resume();
        auto reason = process->wait_on_signal();
        print_stop_reason(*process, reason);
    } else if (is_prefix(command, "help")) {
        print_help(args);
    } else if (is_prefix(command, "register")) {
        handle_register_command(*process, args);
    } else if (is_prefix(command, "breakpoint")) {
        handle_breakpoint_command(*process, args);
    } else if (is_prefix(command, "step")) {
        auto reason = process->step_instruction();
        print_stop_reason(*process, reason);
    } else {
        std::cerr << "Unknow command\n";
    }
}
std::unique_ptr<sdb::process> attach(int argc, const char** argv) {
    if (argc == 3 and argv[1] == std::string_view("-p")) {
        pid_t pid = std::atoi(argv[2]);
        return sdb::process::attach(pid);
    } else {
        auto program_path = argv[1];
        auto proc         = sdb::process::launch(program_path);
        fmt::print("Launched process with PID {}\n", proc->pid());
        return proc;
    }
}
void main_loop(std::unique_ptr<sdb::process>& process) {
    char* line = nullptr;
    while ((line = readline("sdb> ")) != nullptr) {
        std::string line_str;
        if (line == std::string_view("")) {
            free(line);
            if (history_length > 0) {
                line_str = history_list()[history_length - 1]->line;
            }
        } else {
            line_str = line;
            add_history(line);
            free(line);
        }
        if (!line_str.empty()) {
            try {
                handle_command(process, line_str);
            } catch (const sdb::error& err) {
                std::cout << err.what() << '\n';
            }
        }
    }
}

}   // namespace
int main(int argc, const char** argv) {
    if (argc == 1) {
        std::cerr << "No arguments given\n";
        return -1;
    }
    try {
        auto process = attach(argc, argv);
        main_loop(process);
    } catch (const sdb::error& err) {
        std::cout << err.what() << '\n';
    }
}