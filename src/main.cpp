#include <csignal>
#include <cstdio>

#include <future>
#include <functional>
#include <iostream>

#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#include "spdlog/spdlog.h"
#include "spdlog/sinks/basic_file_sink.h"

#include "bpf_provider.h"
#include "consumer.h"
#include "sysdig_provider.h"

std::function<void(int)> exit_handler;

void sig_handler(int signal) {
    exit_handler(signal);
}

void print_usage() {
    std::cerr << R"(Usage: debugger [OPTION...] <cmd> <arg1> <arg2>
    --help               Print this message
    --sysdig             Use sysdig as provider instead of bpf
    -logp logs-path      Set custom path to logs output file
)";
}

int main(int argc, char **argv) {
    // Parsing command line arguments
    bool use_sysdig = false;
    std::string logs_file_path = std::string(LOGSDIR) + "/logs_" 
        + std::to_string(time(NULL)) + ".txt";

    argv++; argc--;
    while (true) {
        if (argc <= 0 || !argv[0] || !strlen(argv[0])) {
            // Malformed arguments
            print_usage();
            return 1;
        }
        std::string arg(argv[0]);
        if (arg[0] != '-') {
            // No more arguments
            break;
        }
        if (arg == "--sysdig") {
            // Using sysdig as provider, bpf is default
            use_sysdig = true;
        } else if (arg == "-logp") {
            argv++; argc--;
            logs_file_path = std::string(argv[0]);
        } else if (arg == "--help") {
            print_usage();
            return 0;
        } else {
            // Unknown argument
            std::cerr << "Unknown argument " << arg << "\n";
            print_usage();
            return 1;
        }
        argv++; argc--;
    }
    
    // Setting up logs using spdlog library from https://github.com/gabime/spdlog    
    auto logger = spdlog::basic_logger_mt("file_logger", logs_file_path);
    spdlog::set_default_logger(logger);
    spdlog::set_pattern("[%d/%m/%Y %T%z][%-20!s:%-4!# %-10!!][%-5!l] %v");
    
    SPDLOG_INFO("Starting debugger execution");

    sigset_t sig_usr, default_set;
    sigemptyset(&sig_usr);
    sigaddset(&sig_usr, SIGUSR1);
    sigprocmask(SIG_BLOCK, &sig_usr, &default_set);

    pid_t pid = fork();
    if (!pid) {
        struct sigaction resume;
        resume.sa_handler = [](int signal){};
        resume.sa_flags = 0;
        sigaction(SIGUSR1, &resume, nullptr);

        SPDLOG_INFO("Program process going to sleep");
        
        sigsuspend(&default_set);
        sigprocmask(SIG_SETMASK, &default_set, nullptr);

        execvp(*argv, argv);
        return 1;
    }

    sigprocmask(SIG_SETMASK, &default_set, nullptr);

    Consumer consumer(pid);

    Provider* provider_ptr = nullptr;
    if (use_sysdig)
        provider_ptr = new SysdigProvider(pid);
    else
        provider_ptr = new BPFProvider(pid);

    exit_handler = [&](int signal) { 
        consumer.stop();
        provider_ptr->stop();
    };

    // Cleaner handling of Ctrl-C
    std::signal(SIGTERM, sig_handler);
    std::signal(SIGINT, sig_handler);

    std::thread consumer_thread = std::thread([&]() {
        consumer.start(
            *provider_ptr,
            (use_sysdig) ? true : false // BPF doesn't convert buffers to hex yet
        );
    });

    int ret = provider_ptr->start();
    provider_ptr->stop();
    consumer_thread.join();

    delete provider_ptr;

    return ret;
}
