#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h> 
#include <readline/readline.h>
#include <readline/history.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>

#define MAX_NUM_TOKENS 100
#define PATH_MAX 1000
#define MAX_MATCHES 1024
#define MAX_LINE_LENGTH 1024

// To compile: "gcc -o main main.c -lreadline" to avoid linker errors when attempting to compile

char** paths; // string array for PATH
static int path_count; // number of entries in PATH
static int history_count = 1;
static int history_saved = 0; // used for history -a (append)

// built in commands (as opposed to external found in PATH)
char* builtins[] = {"type", "echo", "exit", "pwd", "cd", "history"};
static int num_builtins = sizeof(builtins) / sizeof(builtins[0]); // 6



// returns successive matches for the current input on repeated tab presses
char *command_generator(const char *text, int state) {
  static int match_index, len;
  static char *matches[MAX_MATCHES];
  static int num_matches;

  if (state == 0) {
      match_index = 0;
      len = strlen(text);
      num_matches = 0;

      // Clear previous matches
      for (int i = 0; i < MAX_MATCHES; i++) {
          free(matches[i]);
          matches[i] = NULL;
      }

      // check builtins
      for (int i = 0; i < num_builtins; i++) {
          if (strncmp(builtins[i], text, len) == 0) {
              matches[num_matches++] = strdup(builtins[i]);
              if (num_matches >= MAX_MATCHES) break;
          }
      }

      // check all executables in PATH (much like find_in_path())
      for (int i = 0; i < path_count; i++) {
          DIR *dir = opendir(paths[i]);
          if (!dir) continue;

          struct dirent *entry;
          while ((entry = readdir(dir)) != NULL) {
              if (entry->d_name[0] == '.') continue;

              // prefix match first n chars
              if (strncmp(entry->d_name, text, len) == 0) {
                  // avoid dupes
                  int duplicate = 0;
                  for (int j = 0; j < num_matches; j++) {
                      if (strcmp(matches[j], entry->d_name) == 0) {
                          duplicate = 1;
                          break;
                      }
                  }

                  if (!duplicate) {
                      matches[num_matches++] = strdup(entry->d_name);
                      if (num_matches >= MAX_MATCHES) break;
                  }
              }
          }
          closedir(dir);
      }
  }

  // Return matches one by one
  if (match_index < num_matches) {
      return strdup(matches[match_index++]);
  }

  return NULL;
}

// called by readline when user pressed Tab ("\t")
char** my_completion(const char* text, int start, int end) {
  rl_attempted_completion_over = 1;  // don't use default filename completion
  return rl_completion_matches(text, command_generator);
}

// break apart input into tokens, and store into args[] string array
static int tokenize_input (char* rest, char* args[]) {
  // 2-D array: each token has its own array, used to build the full token that will go into args
  static char token_bufs[MAX_NUM_TOKENS][1000]; // max length of tokens: 999 chars
  int argc = 0;
  int token_i = 0;
  if (!rest) {
    args[argc] = NULL;
    return argc;
  }

  while (*rest) {
    // Skip leading spaces
    while (*rest == ' ') rest++;
    if (*rest == '\0') break;

    char *buf = token_bufs[token_i]; // 1 row of token_bufs
    int j = 0;

    while (*rest) {
      if (*rest == ' ') {
        break;
      }
      if (*rest == '\\') {
        rest++;
        if (*rest) {
          buf[j] = *rest;
          j++;
          rest++;
        }
        continue;
      }

      if (*rest == '\'') {
        rest++;  // skip starting quote
        while (*rest && *rest != '\'') {
          if (j < 999) {
            buf[j] = *rest;
            j++;
            rest++;
          } else {
            rest++;
          }
        }
        if (*rest == '\'') {
          if (*(rest + 1) == '\'') {
            rest += 2;
            continue;
          }
          rest++;  // skip ending quote
          break;
        }
      } else if (*rest == '\"'){
        rest++;  // skip starting quote
        while (*rest && *rest != '\"') {
          if (j < 999) {
            // handle exceptions preceded by backslash
            if (*rest == '\\' && ((*(rest + 1) == '\\') || (*(rest + 1) == '$') || (*(rest + 1) == (int)10) || (*(rest + 1) == '\"'))) {
              rest++;
            }
            buf[j] = *rest;
            j++;
          }
          rest++;
        }
        if (*rest == '\"') {
          rest++;  // skip ending quote
        }
      }
      else {
        if (j < 999) { 
          buf[j] = *rest;
          j++;
        }
        rest++;
      }
    }

    buf[j] = '\0';
    args[argc] = buf;
    argc++;
    token_i++;
  }

  args[argc] = NULL;
  return argc;
}


// checks if a token is a builtin command or not
// returns 0 if token is builtin and 1 otherwise (either invalid or an external)
static char check_builtin(char *token) {
  int num_builtins = sizeof(builtins) / sizeof(builtins[0]);
  for (int i = 0; i < num_builtins; i++) {
    if (strcmp(builtins[i], token) == 0) {
      return 0; // builtin found
    }
  }
  return 1; // builtin not found
}

static char* find_in_path(char* token, char** paths) {
  DIR* dir;
  struct dirent* entry;

  for (int i = 0; i < path_count; i++) {
    dir = opendir(paths[i]);
    if (!dir) continue;    // skip invalid/unopenable PATH entry
    while ((entry = readdir(dir)) != NULL) {
      if (strcmp(entry->d_name, token) == 0) {
        return(paths[i]);
      }
    }
    closedir(dir);
  }
  return "";
}

// search inside envp (PATH, cwd, etc.)
static char* find_in_env(char* envp[], char *token) {
  int token_len = strlen(token);
  for (int i = 0; envp[i] != NULL; i++) {
    if (!strncmp(envp[i], token, token_len)) {
        return envp[i] + token_len;
    }
}
return 0; // not found
}

// if pipe is involved, don't add new line (prevents off-by-one when using wc)
int is_pipe(int fd) {
  struct stat st;
  if (fstat(fd, &st) == -1) return 0;
  return S_ISFIFO(st.st_mode);
}

static void echo_handler(char* args[], int argc) {
  for (int i = 1; i < argc; i++) { 
    if (args[i] == NULL) {
      break;
    }
    printf("%s", args[i]); // print rest of input, excluding first token (echo)
    if (i < argc - 1) {printf(" ");}
  }
  // only add new line if not piped 
  if (!is_pipe(STDOUT_FILENO)) { 
    printf("\n");
  }
}

static void type_handler(char* args[], int argc, char** paths) {
  char *next_token;
  for (int i = 1; i < argc; i++) {
    next_token = args[i];
    if (!check_builtin(next_token)) {
      printf("%s is a shell builtin\n", next_token); // check if next_token is a builtin command
    } else {
      char* search_path = find_in_path(next_token, paths); // search for next_token in PATH
      if (strcmp(search_path, "")) {
        printf("%s is %s/%s\n", next_token, search_path, next_token);
      } else {
        printf("%s: not found\n", next_token);
      }
    }
  }
}

static void cd_handler(char* args[], int argc, char* envp[]) {
  int cd;
  if (!strcmp(args[1], "~")) { // return to home directory
      cd = chdir(find_in_env(envp, "HOME="));
  } else {
    cd = chdir(args[1]);
  }
  if (cd != 0) {
    printf("cd: %s: No such file or directory\n", args[1]);
  }
}

static void pwd_handler(void){
  char cwd[PATH_MAX];
  if (getcwd(cwd, sizeof(cwd)) != NULL) {
    printf("%s\n", cwd);
  } else {
    printf("Error retrieving current working directory\n");
  }
}

static void history_handler(int argc, char* args[]) {
    if (argc > 3) {
        printf("history: too many arguments\n");
        return;
    }

    if (argc == 1) {
        // Just print full history
        for (int i = 0; i < history_length; i++) {
          HIST_ENTRY *entry = history_get(i + history_base);
          if (entry)
            printf("%d  %s\n", i + 1, entry->line);
        }
    } else {
        if (strcmp(args[1], "-r") == 0) {
            if (argc < 3) {
                printf("history -r: filename required\n");
                return;
            }
            int ret = read_history(args[2]);
            if (ret != 0) {
                perror("history -r");
            }
        } else if (strcmp(args[1], "-w") == 0) {
            if (argc < 3) {
                printf("history -w: filename required\n");
                return;
            }
            int ret = write_history(args[2]);
            if (ret != 0) {
                perror("history -w");
            }
        } else if (strcmp(args[1], "-a") == 0) {
          if (argc < 3) {
            printf("history -a: filename required\n");
            return;
          }

          int new_entries = history_length - history_saved;
          if (new_entries > 0) {
            int ret = append_history(new_entries, args[2]);
            if (ret != 0) {
              perror("history -a");
            } else {
              history_saved = history_length; // update the save point
            }
          }
        } else {
            int limit = atoi(args[1]);
            if (limit <= 0) {
                printf("history: invalid argument\n");
                return;
            }

            int start = history_length - limit;
            if (start < 0) start = 0;

            for (int i = start; i < history_length; i++) {
                HIST_ENTRY *entry = history_get(i + history_base);
                if (entry)
                    printf("%d  %s\n", i + 1, entry->line);
            }
          }
    }
}

int main(int argc, char *argv[], char * envp[]) {
  // Flush after every printf
  setbuf(stdout, NULL);
  
  // Wait for user input;
  char input[MAX_NUM_TOKENS];

  // grab PATH using getenv(). Alternatively, use parameter "char *envp[]" for main().
  char* path = strdup(find_in_env(envp, "PATH="));
  if (!path) {
    printf("No PATH provided");
    return 1;
  }

  // allocate array of strings
  paths  = calloc((PATH_MAX), sizeof(char*)); // not accepting a PATH with any more than 999 paths
  if (!paths) {
    printf("String array allocation: Out of memory");
    return 1;
  }

  // separate PATH into tokens and store into string array 
  char *token;
  path_count = 0;
  while ((token = strsep(&path, ":")) != NULL) {
    paths[path_count] = token; 
    path_count++;
  }
  rl_attempted_completion_function = my_completion;

  char* histfile_env = find_in_env(envp, "HISTFILE=");
    if (histfile_env && histfile_env[0] != '\0') {
      // Load history from file
      read_history(histfile_env);
  }

  while (1) {

    // begin prompt ($)
    char* line = readline("$ ");
    if (!line) break;  // EOF (Ctrl+D)
    if (*line) add_history(line);  // save non-empty input
    strncpy(input, line, MAX_NUM_TOKENS - 1);
    input[MAX_NUM_TOKENS - 1] = '\0';
    free(line);
    
    char* temp_input = input;

    // tokenize input (accounting for single quotes)
    // Then, store into args[] array and update builtins to use args[] instead of temp_input
    char* args[MAX_NUM_TOKENS]; // array to hold args
    argc = tokenize_input(temp_input, args);
    char* search_path = find_in_path(args[0], paths);

    // temp input is unchanged -- use it for pipe checking
    char** pipes = calloc((MAX_NUM_TOKENS), sizeof(char*));
    if (!pipes) {
      printf("malloc failure");
      return 1;
    }

    char* rest = temp_input;
    char* segment;
    int pipe_count = 0;
    while ((segment = strsep(&rest, "|")) != NULL) {
      
      // Trim leading and following spaces
      while (*segment == ' ') segment++;
      char *end = segment + strlen(segment) - 1;
      while (end > segment && (*end == ' ' || *end == '\n')) {
          *end = '\0';
          end--;
      }

      pipes[pipe_count] = segment;
      pipe_count++;
    }


    // check for exit prompt
    if (!strcmp(args[0], "exit")) {
      if (histfile_env && histfile_env[0] != '\0') {
        write_history(histfile_env);  // save all history to file
      }
      break;
    }
    
    int i = 0;
    int stdoutput = -1;
    char* output_file = NULL;

    int redirect_type = -1;
    int fd = -1;

    // search for redirect char
    while (args[i]) {
      if (!strcmp(args[i], ">") || !strcmp(args[i], "1>") 
        || !strcmp(args[i], "2>") 
        || !strcmp(args[i], ">>") || !strcmp(args[i], "1>>")
        || !strcmp(args[i], "2>>")) {

        if (!strcmp(args[i], ">") || !strcmp(args[i], "1>")) {
          redirect_type = 1; // stdout
        } else if (!strcmp(args[i], "2>") ){
          redirect_type = 2; // stderr
        } else if (!strcmp(args[i], ">>") || !strcmp(args[i], "1>>")){
          redirect_type = 3; // append stdout
        } else {
          redirect_type = 4; // append stderr
        }
        stdoutput = i;
        args[stdoutput] = NULL; // break between LHS and RHS of redirect operator
        output_file = args[stdoutput + 1]; // extract output file path
        break;
      }
      i++;
    }

    // for stdout redirect:
    // pull command from left side of > operator (LHS)
    // use dup/dup2(?) to override stdout temporarily
    // redirect stdout to the file on the RHS
    // execute LHS
    // make sure stdout is reverted back to normal 1 afterwards.

    // special case: cd affects parent process directly
    if (!strcmp(args[0], "cd")) {
      cd_handler(args, argc, envp);
    }  else if (!strcmp(args[0], "history")) {
      history_handler(argc, args);
    } else if (pipe_count > 1) {
      
      int pipefd[2 * (pipe_count - 1)]; // two fds per pipe

      // create all pipes up front
      for (int i = 0; i < pipe_count - 1; i++) {
          if (pipe(pipefd + i*2) < 0) {
              perror("pipe");
              exit(1);
          }
      }

      // fork each command
      for (int i = 0; i < pipe_count; i++) {
          pid_t pid = fork();
          if (pid < 0) {
              perror("fork");
              exit(1);
          }

          if (pid == 0) {
              // CHILD

              // stdin from previous pipe (if not first command)
              if (i != 0) {
                  dup2(pipefd[(i-1)*2], STDIN_FILENO);
              }

              // stdout to next pipe (if not last command)
              if (i != pipe_count - 1) {
                  dup2(pipefd[i*2 + 1], STDOUT_FILENO);
              }

              // close all pipe fds in child
              for (int j = 0; j < 2*(pipe_count-1); j++) {
                  close(pipefd[j]);
              }

              // tokenize this segment
              char* segment_args[MAX_NUM_TOKENS];
              tokenize_input(pipes[i], segment_args);

              // exec or handle built-in
              if (!strcmp(segment_args[0], "echo")) {
                echo_handler(segment_args, argc);
                _exit(0);
              } else if (!strcmp(segment_args[0], "pwd")) {
                  pwd_handler();
                  _exit(0);
              } else if (!strcmp(segment_args[0], "type")) {
                  type_handler(segment_args, argc, paths);
                  _exit(0);
              } else {
                  char* spath = find_in_path(segment_args[0], paths);
                  if (strcmp(spath, "") != 0) {
                      char *cmd_path = malloc(strlen(spath) + strlen(segment_args[0]) + 2);
                      sprintf(cmd_path, "%s/%s", spath, segment_args[0]);
                      execve(cmd_path, segment_args, envp);
                      perror("exec");
                      _exit(1);
                  } else {
                      fprintf(stderr, "%s: command not found\n", segment_args[0]);
                      _exit(1);
                  }
              }
          }
      }

      // close all pipe fds in parent
      for (int i = 0; i < 2 * (pipe_count - 1); i++) {
          close(pipefd[i]);
      }

      // wait for all children
      for (int i = 0; i < pipe_count; i++) {
          wait(NULL);
      }
    } else {
      pid_t pid = fork();
      if (pid == -1) {
        printf("fork failed");
        return 1;
      }

      if (pid == 0) {
        // CHILD process
        if (output_file) {
          if (redirect_type == 1 || redirect_type == 2) {
            fd = open(output_file, O_CREAT | O_WRONLY | O_TRUNC, 0644);
          } else if (redirect_type == 3 || redirect_type == 4) {
            fd = open(output_file, O_CREAT | O_WRONLY | O_APPEND, 0644);
          }
          if (fd < 0) {
            printf("error opening file\n");
            exit(1);
          }
          switch (redirect_type) {
            case 1:
            case 3:
              dup2(fd, STDOUT_FILENO); // redirect stdout
              break;
            case 2:
            case 4:
              dup2(fd, STDERR_FILENO); // redirect stderr
              break;
          }
          close(fd);               // close original fd
        }

        if (!strcmp(args[0], "echo")) {
          echo_handler(args, argc);

        } else if (!strcmp(args[0], "type")) {
          type_handler(args, argc, paths);

        } else if (!strcmp(args[0], "pwd")) {
          pwd_handler();

        } else if (!strcmp(args[0], "history")) {
          history_handler(argc, args);

        } else if (strcmp(search_path, "") != 0) {
          char *complete_path = malloc(strlen(search_path) + strlen(args[0]) + 2); // "/" and "\0"
          if (!complete_path) {
              exit(1);
          }
          sprintf(complete_path, "%s/%s", search_path, args[0]);
          execve(complete_path, args, envp);
          _exit(EXIT_FAILURE);   // exec never returns
        } else {
          printf("%s: command not found\n", args[0]);
        }
        exit(0); // exit after builtin execution

      } else {
        // PARENT
        int status;
        waitpid(pid, &status, 0);
      }
    }
  }
  return 0;
}
