#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>
#include <readline/readline.h>
#include <readline/history.h>

// LIMITS
#define MAX_ARGS 100

// IDENTIFIERS
#define INPUT_REDIRECT "<"
#define OUTPUT_REDIRECT ">"
#define ERROR_REDIRECT "2>"
#define PIPE "|"
#define SEND_TO_BACKGROUND "&"

// JOB COMMANDS
#define FOREGROUND "fg"
#define BACKGROUND "bg"
#define JOBS "jobs"

// CUSTOM ERRORS
#define SUCCESS 0
#define INPUT_PARSING_ERROR 1
#define COMMAND_PROCESSING_ERROR 2

// MISC
#define REDIRECTION_REQUIRED 1400 // change this later
#define TERMINAL_PROMPT "# "
#define COMMAND_DELIMETER " "
#define PLUS "+"
#define MINUS "-"

// ENUMS
enum job_status
{
    RUNNING,
    STOPPED,
    DONE
};

// STRUCTS
typedef struct process
{
    pid_t pid;
    char *argv[MAX_ARGS + 1];
    char *redirect_input_filename;
    char *redirect_output_filename;
    char *redirect_error_filename;
    enum job_status status;
} process_t;

typedef struct process_group
{
    pid_t pgid;
    char *command;
    int job_number;
    int background;
    int status; // used for wait_pid
    process_t *first_process;
    process_t *second_process;
} job_t;

// FUNCTION DEFINITIONS
int parse_command(char *command, char *buffer[]);
int process_input(int tokenized_command_length, char *tokenized_command[], job_t *job);
void execute_job();
void execute_process();
void print_bg_job_updates();

// JOB LL
job_t *job_list_head;
job_t *recent_stopped_job;
int job_count;

// JOB DATA STRUCTURE FUNCTIONS
void print_jobs(job_t *head);
job_t *find_job(job_t *job);
int remove_job(job_t *job);
void free_job(job_t *job);
void free_process(process_t *process);
int is_job_done(job_t *job);
int is_job_stopped(job_t *job);

// DEBUG FUNCTIONS
void print_parsed_command(int len, char *buffer[]);

// MISC SINGLETONS
pid_t shell_pid;

int main(int argc, char const *argv)
{
    /* since we're emulating the shell, we want it to ignore being terminated or stopped.
    If any background process tries to write to the
    */
    signal(SIGINT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    // signal(SIGTTOU, SIG_DFL);

    shell_pid = getpid();

    /* here, we want to set the pg containing the shell process to the pid of the shell process
    this is done so that we can restore terminal control to the shell later on
    */
    if (setpgid(0, 0) < 0)
    {
        perror("Error when setting pg for shell");
        exit(1);
    }

    tcsetpgrp(STDIN_FILENO, shell_pid);

    while (1)
    {
        // running updates for status updates on processes

        // get input from the user
        char *command = readline(TERMINAL_PROMPT);

        // this is how we exit the command line with Ctrl-D (it sends an EOF to the readline command)
        if (command == NULL)
        {
            // TODO: clean out entire job list
            free(command);
            exit(INPUT_PARSING_ERROR);
        }
        char *command_copy = strdup(command);
        char *tokenized_command[MAX_ARGS];
        int tokenized_command_length = parse_command(command_copy, tokenized_command);

        if (tokenized_command_length == 0)
        {
            free(command);
            free(command_copy);
            print_bg_job_updates();
            continue;
        }

        // TODO: check for custom commands first

        job_t *job = (job_t *)malloc(sizeof(job_t));
        memset(job, 0, sizeof(job_t));
        int processing_status = process_input(tokenized_command_length, tokenized_command, job);

        if (processing_status == COMMAND_PROCESSING_ERROR)
        {
            free(command);
            free(command_copy);
            free(job);
            print_bg_job_updates();
        }

        // process that and place that into a command buffer
        // process the command buffer into seperate processes with error handling (automatically before launching processes)
        // start a command for non pipe and pipe (make sure to set process groups in both)
        // print out done and stopped jobs
    }

    return 0;
}

/*
Parses commandline provided, returns the length of the tokenized command, including args, returns 0
if an empty line is provided
*/
int parse_command(char *command, char *buffer[])
{
    int ind = 0;
    char *token = strtok(command, COMMAND_DELIMETER);
    while (token != NULL)
    {
        buffer[ind] = token;
        ind++;
        token = strtok(NULL, COMMAND_DELIMETER);
    }
    // ensure list is null terminated
    buffer[ind] = NULL;
    return ind;
}

int process_input(int tokenized_command_length, char *tokenized_command[], job_t *job)
{
    process_t *process;
    int ind = 0;
    int argv_ind = 0;
    int creating_second_process = 0;

    // TODO: make a create process function
    process = (process_t *)malloc(sizeof(process_t));
    memset(process, 0, sizeof(process_t));

    while (ind < tokenized_command_length)
    {
        if (strcmp(tokenized_command[ind], INPUT_REDIRECT) == 0)
        {
            // check that it is not the first or last last string
            if ((ind == 0) || (ind + 1 == tokenized_command_length))
            {
                free_process(process);
                perror("Error [process_input]: < needs to be placed between two tokens\n");
                return COMMAND_PROCESSING_ERROR;
            }
            // don't put redirect symbol or filename into arguments
            process->redirect_input_filename = strdup(tokenized_command[++ind]);
        }
        else if (strcmp(tokenized_command[ind], OUTPUT_REDIRECT) == 0)
        {
            // check that it is not the first or last last string
            if ((ind == 0) || (ind + 1 == tokenized_command_length))
            {
                free_process(process);
                perror("Error [process_input]: > needs to be placed between two command tokens\n");
                return COMMAND_PROCESSING_ERROR;
            }
            process->redirect_output_filename = strdup(tokenized_command[++ind]);
        }
        else if (strcmp(tokenized_command[ind], ERROR_REDIRECT) == 0)
        {
            // check that it is not the first or last last string
            if ((ind == 0) || (ind + 1 == tokenized_command_length))
            {
                free_process(process);
                perror("Error [process_input]: 2> needs to be placed between two command tokens\n");
                return COMMAND_PROCESSING_ERROR;
            }
            process->redirect_error_filename = strdup(tokenized_command[++ind]);
        }
        else if (strcmp(tokenized_command[ind], PIPE) == 0)
        {
            if ((ind == 0) || (ind + 1 == tokenized_command_length))
            {
                free_process(process);
                perror("Error [process_input]: < needs to be placed between two command tokens\n");
                return COMMAND_PROCESSING_ERROR;
            }
            // create a new process and reset the argv counter
            // point job first process to this process
            // don't put pipe symbol into any argv
            // change process pointer to the second pointer
            // set creating second process
        }
        else if (strcmp(tokenized_command[ind], SEND_TO_BACKGROUND) == 0)
        {
            // check that if found it is the last character - error
            if ((ind == 0) || ind + 1 <= tokenized_command_length)
            {
                free_process(process);
                perror("Error [process_input]: & cannot be the only command, and can only be placed at the end of a command\n");
                return COMMAND_PROCESSING_ERROR;
            }
            job->background = 1;
        }
        else
        {
            process->argv[argv_ind] = strdup(tokenized_command[ind]);
        }
        ind++;
        argv_ind++;
    }

    if (!creating_second_process)
    {
        job->first_process;
    }
    else
    {
        job->second_process;
    }

    return SUCCESS;
}

void clean_terminal(char *command, char *command_copy)
{
    free(command);
    free(command_copy);
}

void print_bg_job_updates()
{
    printf("Not Implemented Yet!\n");
    return;
}

// ==== JOB DATA STRUCTURE FUNCTIONS ==== //
void free_job(job_t *job)
{
    if (job->first_process)
    {
        free_process(job->first_process);
    }
    if (job->second_process)
    {
        free_process(job->second_process);
    }
}
void free_process(process_t *process)
{
    // free all memory used for filename storage
    if (process->redirect_input_filename)
    {
        free(process->redirect_input_filename);
    }
    if (process->redirect_output_filename)
    {
        free(process->redirect_output_filename);
    }
    if (process->redirect_error_filename)
    {
        free(process->redirect_error_filename);
    }
    // free all memory used for argv arg storage
    int i = 0;
    while (process->argv[i] != NULL)
    {
        free(process->argv);
        i++;
    }
}

/* TODO:
    1. Input Parsing
    2. File Redirection
    3. Pipes
    4. Signals
    5. Jobs/Custom Commands
        - jobs only prints out bg jobs
        - can start jobs in bg with &
        - stopped fg jobs go to bg with (^Z)
        - need to track the most recently added job in bg list
        - job numbers (id) and number of job (count) are different,
          jobs added to job list have a new id of the highest id currently in the
          job list
        -
 */

/*
    1. What is the most recent BG process [or recent process?]
    2. Can you only have output and input redirection from a FILE
*/

// ==== DEBUGGING FUNCTIONS ==== //
void print_parsed_command(int len, char *buffer[])
{
    printf("In Token Print:%d\n", len);
    for (int i = 0; i < len; i++)
    {
        printf("[%d]: %s\n", i, buffer[i]);
    }
    return;
}
