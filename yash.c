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

// CUSTOM ERRORS - make them negative numbers
#define SUCCESS 42
#define INPUT_PARSING_ERROR -101
#define COMMAND_PROCESSING_ERROR -102
#define INPUT_FILE_REDIRECTION_FAILURE -104
#define OUTPUT_FILE_REDIRECTION_FAILURE -105
#define ERROR_FILE_REDIRECTION_FAILURE -106

// MISC
#define REDIRECTION_REQUIRED 1400 // change this later
#define TERMINAL_PROMPT "# "
#define COMMAND_DELIMETER " "
#define PLUS "+"
#define MINUS "-"
#define TRUE 1
#define FALSE 0

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
} process_t;

typedef struct process_group
{
    pid_t pgid;
    char *command;
    int job_number;
    int background;
    int display_update;
    enum job_status status;
    process_t *first_process;
    process_t *second_process;
    struct process_group *next;
} job_t;

// FUNCTION DEFINITIONS
int parse_command(char *command, char *buffer[]);
int process_input(int tokenized_command_length, char *tokenized_command[], job_t *job);
void execute_job(job_t *job);
int execute_process(job_t *job);
int execute_pipe_process(job_t *job);
void execute_in_background(job_t *job);
void continue_background_job(job_t *job); // make sure to turn off notifications for this job when it finishes in foreground!
void execute_in_foreground(job_t *job);
void print_bg_job_updates();
void nuke_all_file_descriptors();

// JOB LL
job_t *job_list_head;
job_t *recent_stopped_job;

// JOB DATA STRUCTURE FUNCTIONS
void print_job_table();
void print_job(job_t *job, int is_most_recent_job);
job_t *find_job(pid_t pgid);
int find_most_recent_job_num();
int remove_job(job_t *job);
void add_job(job_t *job);
void free_job_table(job_t *job);
void free_job(job_t *job);
void free_process(process_t *process);
int apply_file_redirects(process_t *process); // if there is an error here, we immediately EXIT the process [to be collected with status later]
int is_job_done(job_t *job);
int is_job_stopped(job_t *job);
int update_job_table(job_t *job); // make sure just to remove any processes in the job that are registered as fg, this is fine as true fg processes will hang and not ask for another prompt until complete!
int update_job_status(int status, pid_t pid);

// DEBUG FUNCTIONS
void print_parsed_command_debug(char *buffer[]);
void print_job_debug(job_t *job);
void print_process_debug(process_t *process);

// MISC SINGLETONS
pid_t shell_pid;

int main(int argc, char const *argv)
{
    /* since we're emulating the shell, we want it to ignore being terminated or stopped.
    If any background process tries to write to the
    */
    signal(SIGINT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);

    shell_pid = getpid();

    /* here, we want to set the pg containing the shell process to the pid of the shell process
    this is done so that we can restore terminal control to the shell later on
    */
    if (setpgid(0, 0) < 0)
    {
        perror("Error when setting pg for shell");
        exit(1);
    }

    // initialize job linked list
    job_list_head = NULL;

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

        // TODO: check for custom commands first FUNCTION

        job_t *job = (job_t *)malloc(sizeof(job_t));
        memset(job, 0, sizeof(job_t));
        job->command = command; // TODO: possibly delete this later if this actually does nothing
        int processing_status = process_input(tokenized_command_length, tokenized_command, job);

        if (processing_status == COMMAND_PROCESSING_ERROR)
        {
            free(command);
            free(command_copy);
            free_job(job);
            print_bg_job_updates();
            continue;
        }
        execute_job(job);

        // launch jobs

        // process that and place that into a command buffer
        // process the command buffer into seperate processes with error handling (automatically before launching processes)
        // start a command for non pipe and pipe (make sure to set process groups in both)
        // print out done and stopped jobs
    }

    return 0;
}

// ==== COMMAND LINE PARSING / INTERPRETATION ==== //

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
            if ((argv_ind == 0) || (ind + 1 == tokenized_command_length))
            {
                free_process(process);
                printf("Error [process_input]: < needs to be placed between two tokens\n");
                return COMMAND_PROCESSING_ERROR;
            }
            // don't put redirect symbol or filename into arguments
            process->redirect_input_filename = strdup(tokenized_command[++ind]);
        }
        else if (strcmp(tokenized_command[ind], OUTPUT_REDIRECT) == 0)
        {
            // check that it is not the first or last last string
            if ((argv_ind == 0) || (ind + 1 == tokenized_command_length))
            {
                free_process(process);
                printf("Error [process_input]: > needs to be placed between two command tokens\n");
                return COMMAND_PROCESSING_ERROR;
            }
            process->redirect_output_filename = strdup(tokenized_command[++ind]);
        }
        else if (strcmp(tokenized_command[ind], ERROR_REDIRECT) == 0)
        {
            // check that it is not the first or last last string
            if ((argv_ind == 0) || (ind + 1 == tokenized_command_length))
            {
                free_process(process);
                printf("Error [process_input]: 2> needs to be placed between two command tokens\n");
                return COMMAND_PROCESSING_ERROR;
            }
            process->redirect_error_filename = strdup(tokenized_command[++ind]);
        }
        else if (strcmp(tokenized_command[ind], PIPE) == 0)
        {
            if ((argv_ind == 0) || (ind + 1 == tokenized_command_length))
            {
                free_process(process);
                printf("Error [process_input]: | needs to be placed between two command tokens\n");
                return COMMAND_PROCESSING_ERROR;
            }
            // point job first process to this process
            job->first_process = process;

            // create a new process and reset the argv counter
            process = (process_t *)malloc(sizeof(process_t));
            memset(process, 0, sizeof(process_t));
            argv_ind = -1;

            // set creating second process
            creating_second_process = 1;

            // don't put pipe symbol into any argv
        }
        else if (strcmp(tokenized_command[ind], SEND_TO_BACKGROUND) == 0)
        {
            // check that if found it is the last character - error
            if ((ind == 0) || ind + 1 < tokenized_command_length)
            {
                free_process(process);
                printf("Error [process_input]: & cannot be the only command, and can only be placed at the end of a command\n");
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
        job->first_process = process;
    }
    else
    {
        job->second_process = process;
    }
    return SUCCESS;
}

void clean_terminal(char *command, char *command_copy)
{
    free(command);
    free(command_copy);
}

// ==== PROCESS LAUNCHING ==== //
void execute_job(job_t *job)
{
    int pgid = -1;
    // check if you need to launch a piped process or a single process
    if (job->first_process != NULL && job->second_process != NULL)
    {
        pgid = execute_pipe_process(job);
    }
    else
    {
        pgid = execute_process(job);
    }

    // update controlling job
    job->pgid = pgid;
    job->status = RUNNING;
    if (job->background)
    {
        job->job_number = find_most_recent_job_num() + 1;
    }
    else
    {
        job->job_number = 1000;
    }

    add_job(job);
    // print_job_debug(job_list_head);
    // print_job_debug(job);
    // print_job_table();

    // delegate job to being registered in the foreground or background
    if (job->background)
    {
        execute_in_background(job);
    }
    else
    {
        execute_in_foreground(job);
    }
    // makes sense to register job during the background or foreground portion because then a valid job is created
}

int execute_process(job_t *job)
{
    int pid = fork();
    if (pid < 0)
    {
        exit(EXIT_FAILURE);
    }
    if (pid == 0)
    {
        // signal stuff
        signal(SIGINT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        signal(SIGTTOU, SIG_IGN);
        // assign child to its own process group
        setpgid(0, 0);

        // check if you need to launch in the foreground or the background (need to set tcgrep thing)
        if (!job->background)
        {
            tcsetpgrp(STDIN_FILENO, job->pgid);
        }

        int redirects_status = apply_file_redirects(job->first_process);
        if (redirects_status < 0)
        {
            printf("File Redirection Error: %d\n", redirects_status);
            exit(redirects_status);
        }
        // means we're good to execvp
        process_t *process = job->first_process;
        if (execvp(process->argv[0], process->argv) < 0)
        {
            if (process->redirect_error_filename != NULL)
            {
                perror(job->command);
                exit(EXIT_FAILURE);
            }
        }
    }
    return pid;
}

int execute_pipe_process(job_t *job)
{
    printf("[execute_pipe_process] - Not Implemented Yet!\n");
    return 0;
}

void execute_in_background(job_t *job)
{
    printf("[execute_in_background] - Not Implemented Yet!\n");
    return;
}
void execute_in_foreground(job_t *job)
{
    int status;
    pid_t pid;

    tcsetpgrp(STDIN_FILENO, job->pgid);
    do
    {
        print_job_table();
        pid = waitpid(-1, &status, WUNTRACED);
    } while (update_job_status(status, pid) && !is_job_stopped(job));
    printf("We have now left the foreground loop\n");
    tcsetpgrp(STDIN_FILENO, shell_pid);
}

int apply_file_redirects(process_t *process)
{
    // create and use dup while checking each of the file redirections
    int status = 0;
    if (process->redirect_input_filename != NULL)
    {
        int fd = open(process->redirect_input_filename, O_RDONLY);
        if (fd < 0)
        {
            nuke_all_file_descriptors();
            return INPUT_FILE_REDIRECTION_FAILURE;
        }
        dup2(fd, STDIN_FILENO);
        close(fd);
    }
    if (process->redirect_output_filename != NULL)
    {
        int fd = open(process->redirect_output_filename, O_CREAT | O_WRONLY | O_TRUNC, S_IRWXU);
        if (fd < 0)
        {
            nuke_all_file_descriptors();
            return OUTPUT_FILE_REDIRECTION_FAILURE;
        }
        dup2(fd, STDOUT_FILENO);
        close(fd);
    }
    if (process->redirect_error_filename != NULL)
    {
        int fd = open(process->redirect_error_filename, O_CREAT | O_WRONLY | O_TRUNC, S_IRWXU);
        if (fd < 0)
        {
            nuke_all_file_descriptors();
            return ERROR_FILE_REDIRECTION_FAILURE;
        }
        dup2(fd, STDERR_FILENO);
        close(fd);
    }
    return SUCCESS;
}

void nuke_all_file_descriptors()
{
    // effectively closes all of the standard input, output, and error file descriptors
    dup2(STDIN_FILENO, STDIN_FILENO);
    dup2(STDOUT_FILENO, STDIN_FILENO);
    dup2(STDERR_FILENO, STDIN_FILENO);
}

void print_bg_job_updates()
{
    printf("[print_bg_job_updates] - Not Implemented Yet!\n");
    return;
}

// ==== JOB/PROCESS UPDATE FUNCTIONS ==== //

int update_job_status(int status, pid_t pid)
{
    // there's one error you may have to account for here, specifically pipes (what exactly is a child process?)
    // what if a process related to a pipe is stopped or finished but you confuse it for the entire process?
    pid_t pgid = getpgid(pid);
    printf("Updating Status for [%d]", pid);
    if (pgid != pid)
    {
        // my fix to the above issue for now ...
        return 1;
    }
    // for unblocking waitpid calls, -1 may be input if there are no processes to update
    if (pid < 0)
    {
        return 0;
    }
    job_t *job = find_job(pgid);
    if (job == NULL)
    {
        // job NOT found
        return 1;
    }

    if (WIFSTOPPED(status))
    {
        printf("STOPPED! STOPPED_SIG: %d, SIGSTOP: %d\n", WSTOPSIG(status), SIGSTOP);
        if (WSTOPSIG(status) == SIGSTOP)
        {
            printf("SIGSTOP\n");
        }
    }
    else
    {
        printf("DONE\n");
    }
    if (WIFSTOPPED(status))
    {
        printf("In process STOPPED!\n");
        // then this process group with pgid was STOPPED
        job->status = STOPPED;
        recent_stopped_job = job;
        if (WSTOPSIG(status) == SIGTSTP || WSTOPSIG(status) == SIGSTOP)
        {
            job->display_update = 0;
        }
    }
    else
    {
        printf("In process DONE\n");
        // then this process group with pgid terminated ab-or normally, so just mark it as done
        job->status = DONE;
    }
    return 1;
}

// ==== JOB DATA STRUCTURE FUNCTIONS ==== //

void add_job(job_t *job)
{
    // job will already by malloc'ed into existance!
    job_t *curr = job_list_head;
    if (curr == NULL)
    {
        job_list_head = job;
        return;
    }
    while (curr != NULL)
    {
        if (curr->next == NULL)
        {
            curr->next = job;
            return;
        }
    }
}

job_t *find_job(pid_t pgid)
{
    job_t *curr = job_list_head;
    if (curr == NULL)
    {
        return NULL;
    }
    while (curr != NULL)
    {
        if (curr->pgid == pgid)
        {
            return curr;
        }
        curr = curr->next;
    }
    printf("Job with pgid: [%d] not found!\n", pgid);
    return NULL;
}

void print_job_table()
{
    int most_recent_job_num = find_most_recent_job_num();
    if (most_recent_job_num == 0)
    {
        // there are currently no background jobs in the job table
        // TODO: How do we handle this case?
        printf("No jobs currently running in background!\n");
        return;
    }
    // there are background jobs listed
    job_t *curr = job_list_head;
    while (curr != NULL)
    {
        // TODO: CHANGE BACK TO curr->background
        if (1)
        {
            int is_most_recent_job = (most_recent_job_num == curr->job_number) ? 1 : 0;
            print_job(curr, is_most_recent_job);
        }
        curr = curr->next;
    }
}

void print_job(job_t *job, int is_most_recent_job)
{
    char status[20] = "Unknown";
    memset(status, '\0', sizeof(status));
    switch (job->status)
    {
    case (RUNNING):
        strcpy(status, "Running");
        break;
    case (STOPPED):
        strcpy(status, "Stopped");
        break;
    case (DONE):
        strcpy(status, "Done");
        break;
    default:
        break;
    }

    if (is_most_recent_job)
    {
        printf("[%d]+\t%s\t\t\t%s\n", job->job_number, status, job->command);
    }
    else
    {
        printf("[%d]-\t%s\t\t\t%s\n", job->job_number, status, job->command);
    }
}

int find_most_recent_job_num()
{
    job_t *curr = job_list_head;
    int recent_job_num = 0;
    if (curr == NULL)
    {
        return 0;
    }
    while (curr != NULL)
    {
        // ensures that foreground jobs are not counted towards "recent jobs"
        // TODO: ADD curr->background && after FOREGROUND WORKING
        recent_job_num = (curr->job_number > recent_job_num) ? curr->job_number : recent_job_num;
        curr = curr->next;
    }
    return recent_job_num;
}

int is_job_stopped(job_t *job)
{
    if (job->status == STOPPED)
    {
        return 1;
    }
    return 0;
}

int is_job_done(job_t *job)
{
    if (job->status == DONE)
    {
        return 1;
    }
    return 0;
}

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
        free(process->argv[i]);
        i++;
    }
}

// ==== DEBUGGING FUNCTIONS ==== //
void print_parsed_command_debug(char *buffer[])
{
    printf("[");
    for (int i = 0; buffer[i] != NULL; i++)
    {
        printf("%s\t", buffer[i]);
    }
    printf("]\n");
    return;
}

void print_job_debug(job_t *job)
{
    printf("pgid: %d\n", job->pgid);
    printf("command: %s\n", job->command);
    printf("job_number: %d\n", job->job_number);
    printf("background: %d\n", job->background);
    printf("display_update: %d\n", job->display_update);
    printf("status: %d\n", job->status);
    if (job->first_process != NULL)
    {
        printf("First Process:\n---\n");
        print_process_debug(job->first_process);
    }
    if (job->second_process != NULL)
    {
        printf("Second Process:\n---\n");
        print_process_debug(job->second_process);
    }
}

void print_process_debug(process_t *process)
{
    printf("pid: %d\n", process->pid);
    print_parsed_command_debug(process->argv);
    printf("input file: %s\n", process->redirect_input_filename);
    printf("output file: %s\n", process->redirect_output_filename);
    printf("error file: %s\n", process->redirect_error_filename);
}
