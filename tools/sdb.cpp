#include <iostream>
#include <libsdb/libsdb.hpp>
#include <string_view>
#include <sys/ptrace.h>
#include <unistd.h>
namespace {
pid_t attach(int argc, const char **argv) {
  pid_t pid = 0;
  if (argc == 3 && argv[1] == std::string_view("-p")) {
    pid = std::atoi(argv[2]);
    if(pid <= 0){
        std::cerr << "Invalid pid\n";
        return -1;
    }
    if(ptrace(PTRACE_ATTACH,pid,nullptr,nullptr)<0){
        std::perror("Could not attach");
        return -1;
    }
  } else {
    const char* program_path = argv[1];
    if((pid = fork())<0){
        std::perror("fork failed");
        return -1;
    }
    if(pid == 0){
        if(ptrace(PTRACE_TRACEME,0,nullptr,nullptr)<0){
            std::perror("Tracing failed");
            return -1;
        }
    }
  }
  return pid;
}
} // namespace
int main(int argc, const char **argv) {
  if (argc == 1) {
    std::cerr << "No arguments given\n";
    return -1;
  }
  pid_t pid = attach(argc, argv);
}