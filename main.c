// Shalvin Deo  s4236151    UQ ITEE     Aug 21 2014

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>

// Maximum allowed command length. Command lines longer than this are ignored.
#define MAX_COMMAND_LENGTH 256
// Maximum number of arguments. Lines with more arguments are ignored.
#define MAX_ARGS 40

// Some utility constants
typedef int bool;
#define false 0
#define true 1
#define READ_END 0
#define WRITE_END 1
#define REDIRECT_NONE -2
#define REDIRECT_TBA -1

// Give the shell prompt some colour.
#define ANSI_COLOR_GREEN   "\x1b[32m"
#define ANSI_COLOR_BLUE    "\x1b[34m"
#define ANSI_COLOR_RESET   "\x1b[0m"

// Enumerated status messages which indicate the kind of command parsed.
typedef enum {
    IGNORE, 
    CMD_CD,
    CMD_EXIT,
    CMD_EXEC,
    CMD_NOTFOUND,
    CMD_NOCMD,
    CMD_EXEC_ERR
} parse_status;

// Enumerated error messages that indicate the error when executing a command.
typedef enum {
    NONE,
    SYNTAX,
    PIPE1_FAIL,
    PIPE2_FAIL,
    COMMAND_NOT_FOUND,
    FILE_NOT_FOUND,
    FILE_OPEN_ERR,
    OTHER
} exec_error;

// Struct representing a command section. A command section is a part of a
// command that is either separated by a newline or a pipe symbol. Information
// regarding the execution of the command is stored in the struct prior to the
// execution of the command.
typedef struct {
    char **argv;
    int argc;
    int inputfd;
    int outputfd;
    bool background;
} Command;

// Checks for any background jobs that have finished and reaps any remaining 
// child proceses.
void check_background() {
    int pid, status;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (WIFEXITED(status)) {
            printf("%d Finished with exit status: %d\n", pid, 
                    WEXITSTATUS(status));
        }
    }
}


// Print shell prompt.
void prompt() {
    char *dirName = malloc(sizeof(char) * 256);
    getcwd(dirName, 256);

    // Check for any background jobs that have finished.
    check_background();

    // Print the prompt.
    printf(ANSI_COLOR_GREEN "itsh" ANSI_COLOR_RESET ":" 
            ANSI_COLOR_BLUE "%s" ANSI_COLOR_RESET "# ", dirName);
    free(dirName);
}

// Tokenize command string. Parameter tArray should be freed afterwards.
// Returns 0 if no error with tArray set to null pointer.
int command_tokens(char *cmdString, char ***tArray) {
    int tokenCount = 0;
    char *token;
    char **tokenArray = NULL;
    const char *delim = " ";
    int cmdStringLen = strlen(cmdString) - 1;
    
    // If empty line or line is a comment then ignore.
    if (cmdStringLen == 0 || cmdString[0] == '#') {
        tArray = NULL;
        return 0;
    }

    // Shorten command string by replacing newline with null pointer.
    cmdString[cmdStringLen] = '\0';
    
    // Loop to tokenize string. Token array is resized on every iteration.
    token = strtok(cmdString, delim);
    while (token) {
        tokenArray = realloc(tokenArray, sizeof(char *) * ++tokenCount);
        tokenArray[tokenCount-1] = malloc(sizeof(char) * strlen(token));
        strcpy(tokenArray[tokenCount-1], token);
        token = strtok(NULL, " ");

        // Abort if loop exceeds maximum allowed arguments/tokens.
        if (tokenCount > MAX_ARGS) {
            free(tokenArray);
            tArray = NULL;
            return 0;
        }
    }
    tokenArray = realloc(tokenArray, sizeof(char *) * (tokenCount + 1));
    tokenArray[tokenCount] = '\0';

    *tArray = tokenArray;
    return tokenCount;
}

// Return the index of string str in token array. Must be an exact match.
int index_of(char **tokens, int numTokens, char *str) {
    int i;

    for (i = 0; i < numTokens; i++) {
        if (strcmp(tokens[i], str) == 0) {
            return i;
        }
    }

    return -1;
}

// Returns a pointer to a new token array with the contents of 'tokens'
// copied. Returned array must be freed.
char** copy_token_array(char **tokens, int nTokens) {
    int i;
    char **newTokenArray = malloc(sizeof(char *) * (nTokens + 1));
    
    for (i = 0; i < nTokens; i++) {
        //newTokenArray[i] = tokens[i];
        newTokenArray[i] = malloc(sizeof(char) * strlen(tokens[i]));
        strcpy(newTokenArray[i], tokens[i]);
    }
    newTokenArray[nTokens] = '\0';

    return newTokenArray;
}

// Frees a token array.
void free_token_array(char ***tokens, int nTokens) {
    int i;

    for (i = 0; i < nTokens; i++) {
        free((*tokens)[i]);
    }
    if (nTokens > 0) {
        free(*tokens);
    }
}

// Create a new Command. Command should be freed afterwards.
// inputfd & outputfd should be set to -1 if not needed.
Command* new_command(int argc, char **argv, int inputfd, 
        int outputfd) {
    Command *cmd = malloc(sizeof(Command));

    cmd->argc = argc;
    cmd->argv = copy_token_array(argv, argc);
    cmd->inputfd = inputfd;
    cmd->outputfd = outputfd;
    cmd->background = false;

    return cmd;
}

// Free command structure 
void free_command(Command *cmd) {
    free(cmd->argv);
    free(cmd);
}

// Take a command structure and set the inputfd and outputfd values after
// parsing the string argv of the command structure.
exec_error command_redirect(Command **commandPtr) {
    FILE *file;
    int inputRedirectIndex, outputRedirectIndex;
    Command *cmd = *commandPtr;
    char **cmdTokens = cmd->argv;
    int argc = cmd->argc;
    
    // Check if should be a background process.
    if (strcmp(cmdTokens[argc-1], "&") == 0 && argc > 1) {
        cmd->background = true;

        // Delete '&' from command and reduce size of array
        cmdTokens[argc-1] = NULL;
        argc--;
    }

    // Search for input and output operators in tokenized command.
    inputRedirectIndex = index_of(cmdTokens, argc, "<");
    outputRedirectIndex = index_of(cmdTokens, argc, ">");

    // Rough syntax check. Shouldn't be dealing with pipes. Error if pipe found.
    if (index_of(cmdTokens, argc, "|") > 0 || 
            outputRedirectIndex >= (argc - 1) ||
            inputRedirectIndex >= (argc - 1)) {
        return SYNTAX;
    }

    if (inputRedirectIndex > 0 && cmd->inputfd != REDIRECT_NONE) {

        // Open file for reading only. Check for errors before assigning fd to
        // Command struct.
        file = fopen(cmdTokens[inputRedirectIndex+1], "r");
        if (file == NULL) {
            fprintf(stderr, "%s: No such file\n",
                    cmdTokens[inputRedirectIndex+1]);
            return FILE_NOT_FOUND;
        }
        cmd->inputfd = fileno(file);
        cmd->argc = inputRedirectIndex;

    } else if (cmd->background) {

        // If background command, redirect input to /dev/null if no input file
        // specified.
        file = fopen("/dev/null", "r");
        if (file == NULL) {
            fprintf(stderr, "Error opening /dev/null.\n");
            return OTHER;
        }
        cmd->inputfd = fileno(file);
    }

    if (outputRedirectIndex > 0 && cmd->outputfd != REDIRECT_NONE) {

        // Open file for write only. Creates file if it doesn't exist &
        // Overwrites files that do exist. Check for errors then assign fd to
        // struct.
        file = fopen(cmdTokens[outputRedirectIndex+1], "w");
        if (file == NULL) {
            return FILE_OPEN_ERR;
        }
        cmd->outputfd = fileno(file);

        if (inputRedirectIndex > outputRedirectIndex || 
                inputRedirectIndex == REDIRECT_TBA) {
            cmd->argc = outputRedirectIndex;
        }
    }

    // Reduce size of argument array
    if (cmd->argc < argc) {
        cmd->argv[cmd->argc] = NULL;
    }
    return 0;
}

// Execute two commands with the output of the first piped into the input of
// the second.
exec_error execute_piped(Command *cmd1, Command *cmd2) {
    int fds[2], child1, child2, status1, status2;
    exec_error error = NONE;

    if (pipe(fds) != 0) {
        error = OTHER;
        return error;
    }

    // Fork for first half of pipe
    child1 = fork();
    if (child1 == 0) {
        // Child. Check for input redirection then exec.
        if (cmd1->inputfd > 0) {
            dup2(cmd1->inputfd, STDIN_FILENO);
            close(cmd1->inputfd);
        }
        dup2(fds[WRITE_END], STDOUT_FILENO);
        close(fds[WRITE_END]);
        error = PIPE1_FAIL;
        if (execvp(cmd1->argv[0], cmd1->argv) != 0) {
            error = COMMAND_NOT_FOUND;
        }
        exit((int) error);
    } else {
        // Parent. Close write end of pipe and wait for child to exit.
        close(fds[WRITE_END]);
        waitpid(child1, &status1, 0);

        // Check for any errors in child. Command should abort if errors found.
        if (WIFEXITED(status1)) {
            if (WEXITSTATUS(status1) != 0) {
                error = (exec_error) WEXITSTATUS(status1);
            }
        }
    }
    
    // Ensure first fork executed without error then fork for second half of 
    // pipe.
    if (child1 != 0 && error == NONE) {
        child2 = fork();
        if (child2 == 0) {
            // Child. Check for any output redirection first then exec.
            if (cmd2->outputfd > 0) {
                dup2(cmd2->outputfd, STDOUT_FILENO);
                close(cmd2->outputfd);
            }
            dup2(fds[READ_END], STDIN_FILENO);
            close(fds[READ_END]);
            error = PIPE2_FAIL;
            if (execvp(cmd2->argv[0], cmd2->argv) != 0) {
                error = COMMAND_NOT_FOUND;
            }
            exit((int) error);
        } else {
            // Parent. Close read end of pipe and wait while blocking.
            close(fds[READ_END]);
            waitpid(child2, &status2, 0);

            if (WIFEXITED(status2)) {
                error = (exec_error) WEXITSTATUS(status2);
            }
        }
    }

    free(cmd1);
    free(cmd2);
    return error;
}

// Execute a command without piping.
exec_error execute_unpiped(Command *cmd) {
    int childPid, status;
    exec_error error = NONE;
    
    childPid = fork();

    if (childPid == 0) {
        // Set IO redirections before executing
        if (cmd->inputfd > -1) {
            dup2(cmd->inputfd, STDIN_FILENO);
            close(cmd->inputfd);
        }
        if (cmd->outputfd > -1) {
            dup2(cmd->outputfd, STDOUT_FILENO);
            close(cmd->outputfd);
        }
        if (execvp(cmd->argv[0], cmd->argv) != 0) {
            if (errno == ENOENT) {
                error = COMMAND_NOT_FOUND;
            } else {
                error = OTHER;
            }
        }
        // Exit with status corresponding to the exec_error value.
        exit((int) error);
    } else {
        // Wait as parent. Close any file descriptors not needed.
        if (cmd->inputfd > -1) {
            close(cmd->inputfd);
        }
        if (cmd->outputfd > -1) {
            close(cmd->outputfd);
        }
        if (cmd->background) {
            // Command needs to be backgrounded, do not block waiting.
            //waitpid(childPid, &status, WNOHANG);
            printf("%d\n", childPid);
        } else {
            // Wait blocking until foreground command completes.
            waitpid(childPid, &status, 0);

            // Check if child exited with a command not found error.
            if (WIFEXITED(status) && ((exec_error) WEXITSTATUS(status)) 
                    == COMMAND_NOT_FOUND) {
                fprintf(stderr, "No such command.\n");
            }
        }
    }

    free_command(cmd);
    return error;
}

// Takes a tokenized command string and prepares a command structures which are
// then executed.
exec_error prepare_command(int argc, char **cmdTokens) {
    int pipeIndex, argSplit1Len, argSplit2Len;
    char **argSplit1;
    char **argSplit2;
    Command *cmd1;
    Command *cmd2;
    exec_error error = NONE;

    pipeIndex = index_of(cmdTokens, argc, "|");

    if (pipeIndex > 0) {
        // Split arguments into two for each side of the pipe.
        argSplit1Len = pipeIndex;
        argSplit1 = copy_token_array(cmdTokens, argSplit1Len);
        argSplit1[argSplit1Len] = '\0';
        argSplit2Len = argc - pipeIndex - 1;
        argSplit2 = copy_token_array(cmdTokens + pipeIndex + 1, argSplit2Len);
        argSplit2[argSplit2Len] = '\0';
        
        // Prepare input (left) side of pipe. First command will take
        // arguments from left of pipe. REDIRECT_TBA allows fd to be allocated
        // by command_redirect for possible input files. REDIRECT_NONE makes 
        // sure redirection is not done to the output as this will be piped 
        // later.
        cmd1 = new_command(argSplit1Len, argSplit1, REDIRECT_TBA, REDIRECT_NONE);
        error = command_redirect(&cmd1);
        if (error != NONE) {
            free(cmd1);
            return error;
        }

        // Prepare output (right) side of pipe taking the remaining arguments
        // on the RHS of the pipe. REDIRECT constants are switched as this 
        // will be executed on the other half of the pipe.
        cmd2 = new_command(argSplit2Len, argSplit2, REDIRECT_NONE, REDIRECT_TBA);
        error = command_redirect(&cmd2);
        if (error != NONE) {
            free(cmd1);
            free(cmd2);
            return error;
        }

        // Execute piped command
        error = execute_piped(cmd1, cmd2);
        
        free_token_array(&argSplit1, argSplit1Len);
        free_token_array(&argSplit2, argSplit2Len);
    
    } else {
        // No pipe found. Prepare any redirects.
        cmd1 = new_command(argc, cmdTokens, REDIRECT_TBA, REDIRECT_TBA);
        error = command_redirect(&cmd1);

        if (error != NONE) {
            free(cmd1);
            return error;
        }

        error = execute_unpiped(cmd1);
    }

    return error;
}

parse_status parse_command_string(char *cmdString, char **cmdArgs) {
    int nTokens;
    parse_status status;
    exec_error error;
    char **tokens = NULL;

    // Tokenize command string.
    nTokens = command_tokens(cmdString, &tokens);
    
    if (nTokens <= 0) {
        // Ignore if no commands
        status = IGNORE;
    } else if (strcmp(tokens[0], "cd") == 0) {
        // Command is 'cd'. Change directory to first argument or HOME if none.
        status = CMD_CD;
        if (nTokens == 1) {
            chdir(getenv("HOME"));
        } else {
            chdir(tokens[1]);
        }
    } else if (strcmp(tokens[0], "exit") == 0) {
        // Command is 'exit'. Prepare for exit.
        status = CMD_EXIT;
    } else {
        status = CMD_EXEC;
        error = prepare_command(nTokens, tokens);
        
        if (error == FILE_NOT_FOUND) {
            status = CMD_NOTFOUND;
        } else if (error == COMMAND_NOT_FOUND) {
            status = CMD_NOCMD;
        } else if (error != NONE) {
            status = CMD_EXEC_ERR;
        }
    }

    free_token_array(&tokens, nTokens);
    return status;
}

void shell_main_loop(char **cmdString, FILE *stream,
        bool doPrompt) {
    size_t cmdLen = 0;
    char *commandArgs;
    parse_status status;

    if (doPrompt) {
        prompt();
    }
    while(getline(cmdString, &cmdLen, stream) > 0) {
        // Ignore if line is larger than maximum length.
        if (cmdLen >= MAX_COMMAND_LENGTH) {
            fprintf(stderr, "Command exceeds maximum allowed command " 
                    "length (%d)\n", MAX_COMMAND_LENGTH);
            free(*cmdString);
            cmdLen = 0;
            prompt();
            continue;
        }
        
        // Parse command string.
        status = parse_command_string(*cmdString, &commandArgs);
        
        // Handle statuses after parsing.
        if (status == CMD_EXIT) {
            break;
        } else if (status == CMD_NOCMD) {
            // Print error
        } else if (status == CMD_EXEC_ERR) {
            fprintf(stderr, "Error executing command.\n");
        }
        
        if (doPrompt) {
            prompt();
        }
    }
}

int main(const int argc, const char **argv) {
    char *cmdString = NULL;
    FILE* shellFile;

    // Ignore  signal.
    signal(SIGINT, SIG_IGN);

    // Check arguments for shell file and execute if file is valid.
    if (argc >= 2) {
        shellFile = fopen(argv[1], "r");
        if (shellFile == NULL) {
            fprintf(stderr, "%s: No such shell file.\n", argv[1]);
            exit(1);
        }

        // Execute shell using a file as input and without prompts.
        shell_main_loop(&cmdString, shellFile, false);
    } else if (!isatty(STDIN_FILENO)) {
        // Enter main loop but do not prompt as no TTY connected.
        shell_main_loop(&cmdString, stdin, false);
    } else {
        // Enter main shell loop as normal with prompts.
        shell_main_loop(&cmdString, stdin, true);
    }

    return 0;
}

