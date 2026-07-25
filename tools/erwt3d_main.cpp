#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <sstream>
#include <cstring>
#include <unordered_set>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>

struct Task {
    std::string command;
    std::vector<std::string> keyValueArgs;
};

static bool fileExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r");
    size_t end = s.find_last_not_of(" \t\r");
    return (start == std::string::npos) ? "" : s.substr(start, end - start + 1);
}

static std::string resolveBinary(const std::string& cmd) {
    if (cmd.find("erwt3d_") == 0 || cmd.find('/') != std::string::npos)
        return cmd;
    if (cmd == "contest")
        return "erwt3d_contest";
    if (cmd == "bench-contest" || cmd == "bench_contest")
        return "erwt3d_bench_contest";
    if (cmd == "bench-line" || cmd == "bench_line")
        return "erwt3d_bench_line";
    if (cmd == "precompute-x" || cmd == "precompute_x")
        return "erwt3d_precompute_x";
    if (cmd == "benchmark" || cmd == "bench")
        return "erwt3d_bench";
    if (cmd == "gen-test-data" || cmd == "gen_test_data")
        return "gen_test_data";
    return "erwt3d_" + cmd;
}

static bool isKnownCommand(const std::string& s) {
    static const std::unordered_set<std::string> cmds = {
        "convert", "verify", "slice", "line", "info",
        "bench", "benchmark", "bench-contest", "bench_contest", "contest",
        "bench-line", "bench_line",
        "precompute-x", "precompute_x",
        "gen-test-data", "gen_test_data",
        // also accept prefixed names
        "erwt3d_convert", "erwt3d_verify", "erwt3d_slice",
        "erwt3d_line", "erwt3d_info", "erwt3d_bench",
        "erwt3d_bench_contest", "erwt3d_contest", "erwt3d_precompute_x",
        "erwt3d_bench_line",
    };
    return cmds.find(s) != cmds.end();
}

struct ParsedArg {
    std::string key;
    std::string value;
    bool isFlag;  // true if no '=' or value is true/yes/on
    bool isNegated; // false/no/off → skip this argument
};

static ParsedArg parseKeyValue(const std::string& arg) {
    ParsedArg result;
    size_t eq = arg.find('=');
    if (eq == std::string::npos) {
        result.key = trim(arg);
        result.value = "";
        result.isFlag = true;
        result.isNegated = false;
    } else {
        result.key = trim(arg.substr(0, eq));
        result.value = trim(arg.substr(eq + 1));
        std::string lowerVal;
        for (char c : result.value) lowerVal += std::tolower(c);
        if (lowerVal == "true" || lowerVal == "yes" || lowerVal == "on") {
            result.isFlag = true;
            result.isNegated = false;
        } else if (lowerVal == "false" || lowerVal == "no" || lowerVal == "off") {
            result.isFlag = false;
            result.isNegated = true;
        } else {
            result.isFlag = false;
            result.isNegated = false;
        }
    }
    return result;
}

static void buildArgv(const Task& task,
                       const std::string& binDir,
                       std::vector<const char*>& argvOut,
                       std::string& binPathOut) {
    std::string binaryName = resolveBinary(task.command);
    binPathOut = binDir + "/" + binaryName;

    argvOut.clear();
    argvOut.push_back(binPathOut.c_str());

    for (const auto& kv : task.keyValueArgs) {
        ParsedArg pa = parseKeyValue(kv);
        if (pa.isNegated) continue;

        std::string dashKey = "--" + pa.key;
        // Need stable storage for these strings
        // We'll use a separate vector to hold them
        argvOut.push_back(strdup(dashKey.c_str()));
        if (!pa.isFlag) {
            argvOut.push_back(strdup(pa.value.c_str()));
        }
    }
    argvOut.push_back(nullptr);
}

// Special case: handle directories for some commands
static std::string getBinDir(const std::string& selfPath) {
    size_t slash = selfPath.find_last_of('/');
    if (slash != std::string::npos)
        return selfPath.substr(0, slash);
    // Search common locations
    if (fileExists("./build/erwt3d_convert")) return "./build";
    if (fileExists("./erwt3d_convert")) return ".";
    if (fileExists("../build/erwt3d_convert")) return "../build";
    // Use PATH or hope for the best
    return "./build";
}

static int runTask(const Task& task, const std::string& binDir, bool dryRun) {
    std::string binPath;
    std::vector<const char*> argvVec;
    std::vector<void*> allocs;

    std::string binaryName = resolveBinary(task.command);
    binPath = binDir + "/" + binaryName;

    // Commands that take positional arguments (not --key value)
    bool positionalOnly = (task.command == "info");

    argvVec.push_back(binPath.c_str());
    for (const auto& kv : task.keyValueArgs) {
        ParsedArg pa = parseKeyValue(kv);
        if (pa.isNegated) continue;

        if (positionalOnly) {
            // Pass value directly, no --key prefix
            const char* val = strdup(pa.isFlag ? pa.key.c_str() : pa.value.c_str());
            allocs.push_back(const_cast<char*>(val));
            argvVec.push_back(val);
        } else {
            std::string dashKey = "--" + pa.key;
            char* k = strdup(dashKey.c_str());
            allocs.push_back(k);
            argvVec.push_back(k);
            if (!pa.isFlag) {
                char* v = strdup(pa.value.c_str());
                allocs.push_back(v);
                argvVec.push_back(v);
            }
        }
    }
    argvVec.push_back(nullptr);

    if (dryRun) {
        std::cout << "[DRY RUN] " << binPath;
        for (size_t i = 1; argvVec[i]; ++i)
            std::cout << " " << argvVec[i];
        std::cout << "\n";
        for (auto p : allocs) free(p);
        return 0;
    }

    pid_t pid = fork();
    if (pid < 0) {
        std::cerr << "Error: fork failed\n";
        for (auto p : allocs) free(p);
        return 1;
    }

    if (pid == 0) {
        execvp(binPath.c_str(), const_cast<char* const*>(argvVec.data()));
        std::cerr << "Error: Cannot execute " << binPath << ": " << strerror(errno) << "\n";
        std::exit(1);
    }

    for (auto p : allocs) free(p);

    int status;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        if (code != 0)
            std::cerr << "Command failed (exit=" << code << "): " << binaryName << "\n";
        return code;
    }
    std::cerr << "Command terminated abnormally\n";
    return 1;
}

// Parse config file into tasks (sw4 style)
static bool parseConfigFile(const std::string& path, std::vector<Task>& tasks) {
    std::ifstream file(path);
    if (!file) {
        std::cerr << "Error: Cannot open " << path << "\n";
        return false;
    }

    Task current;
    enum State { SEEK_COMMAND, COLLECT_ARGS, AFTER_COMMAND };
    State state = SEEK_COMMAND;
    int lineno = 0;
    std::string line;

    auto flush = [&]() {
        if (!current.command.empty()) {
            tasks.push_back(current);
            current = Task();
        }
        state = SEEK_COMMAND;
    };

    while (std::getline(file, line)) {
        ++lineno;
        std::string trimmed = trim(line);

        if (trimmed.empty() || trimmed[0] == '#') {
            if (state == COLLECT_ARGS) flush();
            continue;
        }

        if (trimmed.substr(0, 3) == "---") {
            flush();
            continue;
        }

        // Check if first token is a known command
        std::string firstToken = trimmed.substr(0, trimmed.find_first_of(" \t="));
        bool startsWithCommand = isKnownCommand(firstToken);

        if (state == SEEK_COMMAND || state == AFTER_COMMAND) {
            if (startsWithCommand) {
                flush();
                current.command = firstToken;
                // Rest of line after command are key=value pairs
                std::string rest = trim(trimmed.substr(firstToken.size()));
                if (!rest.empty()) {
                    std::istringstream iss(rest);
                    std::string token;
                    while (iss >> token) {
                        if (!token.empty()) current.keyValueArgs.push_back(token);
                    }
                }
                state = COLLECT_ARGS;
            } else if (trimmed.find('=') != std::string::npos && !current.command.empty()) {
                // Continuation of previous command with indented key=value
                current.keyValueArgs.push_back(trimmed);
                state = COLLECT_ARGS;
            } else {
                std::cerr << "Warning: " << path << ":" << lineno
                          << ": Unexpected line (not a command or key=value), ignored\n";
            }
        } else if (state == COLLECT_ARGS) {
            if (startsWithCommand) {
                // New command starts
                flush();
                current.command = firstToken;
                std::string rest = trim(trimmed.substr(firstToken.size()));
                if (!rest.empty()) {
                    std::istringstream iss(rest);
                    std::string token;
                    while (iss >> token) {
                        if (!token.empty()) current.keyValueArgs.push_back(token);
                    }
                }
            } else {
                // More args for current command
                current.keyValueArgs.push_back(trimmed);
            }
        }
    }

    flush();
    return !tasks.empty();
}

static void printUsage(const char* prog) {
    std::cerr << "Usage: " << prog << " [options] config.txt\n"
              << "       " << prog << " <command> key=value ...\n\n"
              << "sw4-style config file or direct command mode.\n\n"
              << "Config file format:\n"
              << "  # comment\n"
              << "  command\n"
              << "    key = value\n"
              << "    key = value\n\n"
              << "  command key=value key=value\n\n"
              << "Commands:\n"
              << "  convert       Raw ↔ single-file optimized package\n"
              << "  contest       Unified 330-slice production entry\n"
              << "  verify        Correctness verification\n"
              << "  slice         Single slice read\n"
              << "  line          Single line read\n"
              << "  info          File information\n"
              << "  bench         Generic benchmark\n"
              << "  bench-contest Competition benchmark\n"
              << "  bench-line    Primary-axis line benchmark\n"
              << "  precompute-x  Precompute X-planes\n\n"
              << "Options:\n"
              << "  --dry-run, -n  Print what would be executed\n"
              << "  --help, -h     Show this help\n";
}

int main(int argc, char* argv[]) {
    bool dryRun = false;
    std::vector<std::string> positionalArgs;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dry-run") == 0 || std::strcmp(argv[i], "-n") == 0) {
            dryRun = true;
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            printUsage(argv[0]); return 0;
        } else {
            positionalArgs.push_back(argv[i]);
        }
    }

    if (positionalArgs.empty()) {
        printUsage(argv[0]); return 1;
    }

    const std::string& first = positionalArgs[0];
    std::string binDir = getBinDir(argv[0]);

    // Mode detection
    bool isFileMode = fileExists(first) && !isKnownCommand(first);

    std::vector<Task> tasks;

    if (isFileMode) {
        // Config file mode
        if (!parseConfigFile(first, tasks)) {
            std::cerr << "Error: No valid tasks found in " << first << "\n";
            return 1;
        }
    } else if (isKnownCommand(first)) {
        // Direct command mode: erwt3d command key=value ...
        Task t;
        t.command = first;
        for (size_t i = 1; i < positionalArgs.size(); ++i)
            t.keyValueArgs.push_back(positionalArgs[i]);
        tasks.push_back(t);
    } else {
        std::cerr << "Error: '" << first << "' is not a known command or config file\n";
        printUsage(argv[0]);
        return 1;
    }

    int exitCode = 0;
    for (size_t i = 0; i < tasks.size(); ++i) {
        if (tasks.size() > 1) {
            std::cout << "\n===== Task " << (i + 1) << "/" << tasks.size()
                      << ": " << tasks[i].command << " =====\n";
        }
        int ret = runTask(tasks[i], binDir, dryRun);
        if (ret != 0) exitCode = ret;
    }

    if (tasks.size() > 1) {
        std::cout << "\n===== " << (exitCode == 0 ? "ALL PASS" : "SOME FAILED") << " =====\n";
    }

    return exitCode;
}
