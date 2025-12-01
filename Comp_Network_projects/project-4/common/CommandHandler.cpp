#include "CommandHandler.h"

void CommandHandler::registerCommand(const std::string &name, CommandFn fn) {
    nameToHandler[name] = fn;
}

void CommandHandler::executeCommand(char *command) const {
    // 1) Tokenization: parse the command line into argc/argv
    //    We use C-style tokenization to produce classic argv semantics where
    //    argv[0] is the command name, and subsequent items are arguments.
    std::vector<char*> argv;
    int argc = 0;

    // Make a writable copy of the command string
    char* cmd_copy = strdup(command);
    if (cmd_copy == nullptr) {
        std::cerr << "Failed to allocate memory for command copy" << std::endl;
        return;
    }

    // strtok modifies the input buffer in-place by inserting null terminators
    // at delimiter positions. This is why we duplicated the input above.
    char* token = strtok(cmd_copy, " \t\n");
    while (token != nullptr) {
        argv.push_back(token);
        argc++;
        token = strtok(nullptr, " \t\n");
    }

    std::cout << "command: " << command << std::endl;
    std::cout << "argc: " << argc << std::endl;
    std::cout << "argv: ";
    for (int i = 0; i < argc; i++) {
        std::cout << argv[i] << ",";
    }
    std::cout << std::endl;

    // 2) Validation: ensure at least a command token exists
    if (argc == 0) {
        free(cmd_copy);
        return;
    }

    // 3) Lookup: resolve the handler by command name
    auto it = nameToHandler.find(argv[0]);
    if (it == nameToHandler.end()) {
        std::cerr << "Unknown command: " << argv[0] << std::endl;
        free(cmd_copy);
        return;
    }


    CommandFn func = it->second;

    if (func == nullptr) {
        std::cerr << "Command function is null for: " << argv[0] << std::endl;
        free(cmd_copy);
        return;
    }
    
// convert argv to char**
    char** argv_data = new char*[argc];
    for (int i = 0; i < argc; i++) {
        argv_data[i] = argv[i];
    }
    std::cout << argv_data[0] << std::endl;
    // 4) Execute the command
    func(argc, argv_data);
    std::cout << "command executed" << std::endl;
    
    // 5) Cleanup: free duplicated buffer and argv array
    free(cmd_copy);
    std::cout << "memory freed" << std::endl;
    delete[] argv_data;
}


