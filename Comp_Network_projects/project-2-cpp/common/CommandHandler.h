/*
GPT-5 Generated class
Prompt:
Where would be the best place to store a dictionary that maps command names to a function<int,char*[]>?

If I want to be able to import the class into both the server and client code then I should create a class 
in a separate file. I want to be able to define functions in the client/server class and insert them into a 
dictionary held within the CommandHandler class
*/

#ifndef COMMAND_HANDLER_H
#define COMMAND_HANDLER_H

#define MAX_LINE 256
#include <unordered_map>
#include <functional>
#include <string>
#include <iostream>
#include <cstring>
#include <sstream>
#include <vector>

/*
CommandHandler
----------------

    This class is used to register commands and execute them for both the server and the client.
    Each handler takes in an int argc and char** argv and returns an int
    to mimic a traditional C-style command-line interface.

Usage Pattern:
    - Server and Client both create an instance of CommandHandler.
    - They register their supported commands by name using `registerCommand`.
    - When input is received (e.g., a protocol header), the caller parses the
      text into whitespace-delimited tokens and passes the raw line into
      `executeCommand`. This method tokenizes the string, resolves the command
      function by `argv[0]`, and invokes it.

Protocol Note:
    - This project’s protocol sends a text header line per request. For example:
        put <pathLength> <fileSize>\n
      The `executeCommand` tokenization will place:
        argv[0] = "put"
        argv[1] = "<pathLength>"
        argv[2] = "<fileSize>"
*/
class CommandHandler {
    public:
        using CommandFn = std::function<int(int, char**)>;

    private:
        std::unordered_map<std::string, CommandFn> nameToHandler;

    public:
        CommandHandler() = default;
        ~CommandHandler() = default;

        /*
        registerCommand
        ----------------
        Associates a command name with a handler function. Any previous handler
        for the same name is replaced.
        */
        void registerCommand(const std::string &name, CommandFn fn);

        /*
        executeCommand
        --------------
        Tokenizes a writable buffer in-place (whitespace-delimited) into
        `argv`, then executes the handler registered under `argv[0]`.

        Parameters:
            command - writable C string containing the full command line.
                      The function will modify it using strtok.

        Behavior:
            - If no tokens are found or the command is unknown, the function
              returns without invoking a handler.
        */
        void executeCommand(char *command) const;
};

#endif // COMMAND_HANDLER_H


