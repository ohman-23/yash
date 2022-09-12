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
#define MAX_ARGS 128

// IDENTIFIERS
#define INPUT_REDIRECT "<"
#define OUTPUT_REDIRECT ">"
#define ERROR_REDIRECT "2>"
#define PIPE "|"
#define SEND_TO_BACKGROUND "&"

// JOB CONTROL COMMANDS
#define FOREGROUND "fg"
#define BACKGROUND "bg"
#define JOBS "jobs"
#define FG_JOB_NUM -1

// CUSTOM ERRORS IDS
#define SUCCESS 1
#define INPUT_PARSING_ERROR -1
#define COMMAND_PROCESSING_ERROR -1
#define FILE_REDIRECTION_ERROR -1

// MISC
#define TERMINAL_PROMPT "# "
#define COMMAND_DELIMETER " \t"

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
void continue_background_job(job_t *job, int fg);
void execute_in_foreground(job_t *job);
void nuke_all_file_descriptors();
int execute_custom_commands();

// JOB CONTROL DATA STRUCTURE
job_t *job_list_head;

// JOB CONTROL FUNCTIONS
void update_job_command_str(job_t *job, int fg);
void print_job_table(int print_running_jobs, int print_stopped_jobs, int print_done_jobs);
void print_job(job_t *job, int is_most_recent_job, int custom_command, int command_is_fg);
job_t *find_job(pid_t pgid);
job_t *find_next_job_to_bg();
job_t *find_next_job_to_fg();
int find_most_recent_job_num();
int remove_job(pid_t pgid, int fg_free);
void remove_done_jobs();
void add_job(job_t *job);
void free_job_table();
void free_job(job_t *job);
void free_process(process_t *process);
int apply_file_redirects(process_t *process);
void update_job_table_statuses();
int update_job_status(int status, pid_t pid);

// CUSTOM COMMAND FUNCTIONS
int execute_custom_commands();
void execute_bg();
void execute_fg();
void execute_jobs();

// DEBUGGING FUNCTIONS
void print_parsed_command_debug(char *buffer[]);
void print_job_debug(job_t *job);
void print_process_debug(process_t *process);

// MISC SINGLETONS - IMPORTANT FOR TERMINAL CONTROL
pid_t shell_pid;

int main(int argc, char const *argv)
{
    /* since we're emulating the shell, we want it to ignore being terminated or stopped.
    If any background process tries to write to the
    */
    signal(SIGINT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTOU, SIG_IGN); // only allow pg in control of the fg to write to terminal
    signal(SIGTTIN, SIG_IGN);

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
        // running updates for status updates on processes (since we are using a different pattern)
        update_job_table_statuses();

        // get input from the user
        char *command = readline(TERMINAL_PROMPT);

        // this is how we exit the command line with Ctrl-D (it sends an EOF to the readline command)
        if (command == NULL)
        {
            free(command);
            free_job_table();
            exit(EXIT_SUCCESS);
        }
        char *command_copy = strdup(command);
        char *tokenized_command[MAX_ARGS];
        int tokenized_command_length = parse_command(command_copy, tokenized_command);

        if (tokenized_command_length == 0)
        {
            free(command);
            free(command_copy);
            // print DONE jobs
            print_job_table(0, 0, 1);
            remove_done_jobs();
            continue;
        }

        // check for the execution of custom commands
        if (execute_custom_commands(command))
        {
            free(command);
            free(command_copy);
            // print DONE jobs
            print_job_table(0, 0, 1);
            remove_done_jobs();
            continue;
        }

        job_t *job = (job_t *)malloc(sizeof(job_t));
        memset(job, 0, sizeof(job_t));
        job->command = command;
        int processing_status = process_input(tokenized_command_length, tokenized_command, job);

        if (processing_status == COMMAND_PROCESSING_ERROR)
        {

            free(command_copy);
            free_job(job);
            // print DONE jobs
            print_job_table(0, 0, 1);
            remove_done_jobs();
            continue;
        }
        execute_job(job);
        update_job_table_statuses();
        print_job_table(0, 0, 1);
        remove_done_jobs();
        free(command_copy);
    }

    return 0;
}

// ==== COMMAND LINE PARSING / INTERPRETATION ==== //

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

    process = (process_t *)malloc(sizeof(process_t));
    memset(process, 0, sizeof(process_t));

    while (ind < tokenized_command_length)
    {
        if (strcmp(tokenized_command[ind], INPUT_REDIRECT) == 0)
        {
            // check that it is not the first or last last string
            if ((argv_ind == 0) || (ind + 1 == tokenized_command_length))
            {
                // free_process(process);
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
        job->job_number = FG_JOB_NUM;
    }

    add_job(job);

    // delegate job to being registered in the foreground or background
    if (!job->background)
    {
        execute_in_foreground(job);
    }
    // else we continue on with execution, given that the process is now successfully running in the background
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
            // have to set as pid since pid==pgid, and job->pgid may not be set yet due to race conditions
            tcsetpgrp(STDIN_FILENO, pid);
        }

        int redirects_status = apply_file_redirects(job->first_process);
        if (redirects_status == FILE_REDIRECTION_ERROR)
        {
            exit(EXIT_FAILURE);
        }
        // means we're good to execvp
        process_t *process = job->first_process;
        if (execvp(process->argv[0], process->argv) < 0)
        {
            if (process->redirect_error_filename != NULL)
            {
                exit(EXIT_FAILURE);
            }
        }
        exit(EXIT_FAILURE);
    }
    return pid;
}

int execute_pipe_process(job_t *job)
{
    int pgid = fork();
    if (pgid < 0)
    {
        exit(EXIT_FAILURE);
    }
    if (pgid == 0)
    {
        // signal stuff to be inherited by children
        signal(SIGINT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        // we want children processes to be able to write to the terminal if the pg is in the foreground
        signal(SIGTTOU, SIG_DFL);
        signal(SIGTTIN, SIG_IGN);

        setpgid(0, 0);

        // setting process to be either in foreground or background
        if (!job->background)
        {
            // have to set as pid since pid==pgid, and job->pgid may not be set yet due to race conditions
            tcsetpgrp(STDIN_FILENO, pgid);
        }

        // create fd for pipe
        int pipe_fd[2];
        int pipe_creation_status = pipe(pipe_fd);
        if (pipe_creation_status < 0)
        {
            exit(EXIT_FAILURE);
        }
        // first process
        int pid1 = fork();
        if (pid1 < 0)
        {
            exit(EXIT_FAILURE);
        }
        if (pid1 == 0)
        {
            // assign this child process to pg of parent process!
            // setpgid(0, pgid); // may have to disable this for SIGNALS TO BE RECIEVED BY CHILDREN
            // apply pipe stdout redirection
            close(pipe_fd[0]);
            dup2(pipe_fd[1], STDOUT_FILENO);
            close(pipe_fd[1]);

            // apply file redirects
            int redirects_status = apply_file_redirects(job->first_process);
            if (redirects_status == FILE_REDIRECTION_ERROR)
            {
                // terminate the program if any file redirections encountered
                exit(EXIT_FAILURE);
            }
            // means we're good to execvp
            process_t *process = job->first_process;
            if (execvp(process->argv[0], process->argv) < 0)
            {
                if (process->redirect_error_filename != NULL)
                {
                    exit(EXIT_FAILURE);
                }
            }
            exit(EXIT_FAILURE);
        }
        // second process
        int pid2 = fork();
        if (pid2 < 0)
        {
            exit(EXIT_FAILURE);
        }
        if (pid2 == 0)
        {
            // assign this child process to pg of parent process!
            // setpgid(0, pgid); // may have to disable this for SIGNALS TO BE RECIEVED BY CHILDREN
            // apply pipe stdin redirection
            close(pipe_fd[1]);
            dup2(pipe_fd[0], STDIN_FILENO);
            close(pipe_fd[0]);
            // apply file redirects
            int redirects_status = apply_file_redirects(job->second_process);
            if (redirects_status == FILE_REDIRECTION_ERROR)
            {
                // terminate the program if any file redirections encountered
                exit(EXIT_FAILURE);
            }
            // means we're good to execvp
            process_t *process = job->second_process;
            if (execvp(process->argv[0], process->argv) < 0)
            {
                if (process->redirect_error_filename != NULL)
                {
                    exit(EXIT_FAILURE);
                }
            }
            exit(EXIT_FAILURE);
        }
        close(pipe_fd[0]);
        close(pipe_fd[1]);

        // wait for each process to be done, not stopped! (need to wait for both because the left may be a sleep!)
        waitpid(pid1, NULL, 0);
        waitpid(pid2, NULL, 0);
        // terminate the process naturally to inform shell of completion
        exit(EXIT_SUCCESS);
    }
    return pgid;
}

void execute_in_foreground(job_t *job)
{
    int status;
    pid_t pid;

    // have to set this due to race conditions (the shell may reach foreground execution before the child has the chance to tcsetgrp)
    tcsetpgrp(STDIN_FILENO, job->pgid);
    do
    {
        pid = waitpid(-1, &status, WUNTRACED);
    } while (update_job_status(status, pid) && (job->status == RUNNING));
    tcsetpgrp(STDIN_FILENO, shell_pid);
}

void continue_background_job(job_t *job, int fg)
{
    // make sure to turn off notifications for this job when it finishes in foreground!
    // by default foreground jobs will not start off with the notification flag
    int kill_signal_sent_status = kill(-job->pgid, SIGCONT);
    if (kill_signal_sent_status < 0)
    {
        job->status = STOPPED;
        return;
    }
    if (fg)
    {
        execute_in_foreground(job);
    }
    // else we continue on with execution, given that the process is now successfully running in the background
}

void print_file_redirection_error_str(char *filename)
{
    int len = strlen(filename);
    char *error_str = (char *)malloc((100 + len) * sizeof(char));
    memset(error_str, '\0', (len + 100) * sizeof(char));
    sprintf(error_str, "-yash: %s: No such file or directory", filename);
    perror(error_str);
    free(error_str);
}

int apply_file_redirects(process_t *process)
{
    // create and use dup while checking each of the file redirections
    int status = 0;
    if (process->redirect_error_filename != NULL)
    {
        int fd = open(process->redirect_error_filename, O_CREAT | O_WRONLY | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH);
        if (fd < 0)
        {
            status = -1;
            print_file_redirection_error_str(process->redirect_error_filename);
        }
        dup2(fd, STDERR_FILENO);
        close(fd);
    }
    if (process->redirect_input_filename != NULL)
    {
        int fd = open(process->redirect_input_filename, O_RDONLY);
        if (fd < 0)
        {
            status = -1;
            print_file_redirection_error_str(process->redirect_input_filename);
        }
        dup2(fd, STDIN_FILENO);
        close(fd);
    }
    if (process->redirect_output_filename != NULL)
    {
        int fd = open(process->redirect_output_filename, O_CREAT | O_WRONLY | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH);
        if (fd < 0)
        {
            status = -1;
            print_file_redirection_error_str(process->redirect_output_filename);
        }
        dup2(fd, STDOUT_FILENO);
        close(fd);
    }
    if (status == -1)
    {
        nuke_all_file_descriptors();
        return FILE_REDIRECTION_ERROR;
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

// ==== JOB/PROCESS UPDATE FUNCTIONS ==== //

int update_job_status(int status, pid_t pid)
{
    // there's one error you may have to account for here, specifically pipes (what exactly is a child process?)
    // what if a process related to a pipe is stopped or finished but you confuse it for the entire process?
    // solution now, when we setpgid to pid for both processes - we know if will be put into the list!
    // [UPDATE] - no, waitpid only waits for direct children :)

    // for unblocking waitpid calls, -1 may be input if there are no processes to update
    if (pid <= 0)
    {
        // pid < 0 means an error, and pid == 0 means WNOHANG was provided and child process is not terminated yet
        return 0;
    }
    job_t *job = find_job(pid);
    if (job == NULL)
    {
        // job NOT found
        return 1;
    }
    if (WIFSTOPPED(status))
    {
        // then this process group with pgid was STOPPED
        // printf("STOPPED\n");
        job->status = STOPPED;
        if (WSTOPSIG(status) == SIGTSTP || WSTOPSIG(status) == SIGSTOP)
        {
            job->background = 1;
            if (job->job_number == FG_JOB_NUM)
            {
                // this means we are transitioning a foreground job in to the background
                // because of foreground jobs blocking stdin, we can be assured there will only ever be one at a given time
                remove_job(pid, 0);
                job->job_number = find_most_recent_job_num() + 1;
                add_job(job);
            }
        }
    }
    else
    {
        // then this process group with pgid terminated ab-or normally, so just mark it as done
        // if it was terminated via SIGINT, it will not display done because the job was moved to the fg earlier!
        job->status = DONE;
    }
    return 1;
}

void update_job_table_statuses()
{
    int status;
    pid_t pid;
    // poll all child processes of the shell to see if any new processes have finished!
    do
    {
        pid = waitpid(-1, &status, WNOHANG | WUNTRACED);
    } while (update_job_status(status, pid));
}

void update_job_command_str(job_t *job, int apply_bg)
{
    int len = strlen(job->command);
    if (apply_bg)
    {
        if (job->command[len - 1] == '&' && job->command[len - 2] == ' ')
        {
            // suffix already included, no update required
            return;
        }
        char bg_suffix[2] = " &";
        char *updated_command = (char *)malloc((len + 3) * sizeof(char));
        memset(updated_command, '\0', (len + 3) * sizeof(char));
        strcat(updated_command, job->command);
        strcat(updated_command, bg_suffix);
        free(job->command);
        job->command = updated_command;
    }
    else
    {
        // we're updating a string from the bg to the fg
        if (!(job->command[len - 1] == '&' && job->command[len - 2] == ' '))
        {
            // suffix not present, no need for removal
            return;
        }
        // remove the background job suffix via null termination!
        job->command[len - 1] = '\0';
        job->command[len - 2] = '\0';
    }
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
        curr = curr->next;
    }
}

void remove_done_jobs()
{
    job_t *curr = job_list_head;
    job_t *prev = NULL;
    if (curr == NULL)
    {
        return;
    }
    while (curr != NULL)
    {
        job_t *temp = curr->next;
        if (curr->status == DONE)
        {
            if (prev == NULL)
            {
                // we're removing the first node of the list
                job_list_head = temp;
                free_job(curr);
                curr = temp;
            }
            else
            {
                // we're removing an element in the middle of the list
                prev->next = temp;
                free_job(curr);
                curr = temp;
            }
        }
        else
        {
            // if an job is not DONE, we want to simply keep traversing
            prev = curr;
            curr = temp;
        }
    }
}

int remove_job(pid_t pgid, int fg_free)
{
    // because of fg and bg (moving fg to a bg process), we may not want to completely free a job!
    // fg_free indicates that we just want to detach a job from the job LL to reinsert as a "bg" job
    job_t *curr = job_list_head;
    job_t *prev = NULL;
    if (curr == NULL)
    {
        return 0;
    }
    while (curr != NULL)
    {
        if (curr->pgid == pgid)
        {
            if (prev == NULL)
            {
                // remove head of job list / only one item in the LL
                job_list_head = curr->next;
                if (fg_free)
                {
                    free_job(curr);
                }
                return 1;
            }
            prev->next = curr->next;
            if (fg_free)
            {
                free_job(curr);
            }
            return 1;
        }
        prev = curr;
        curr = curr->next;
    }
    return 0;
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
    // printf("Job with pgid: [%d] not found!\n", pgid);
    return NULL;
}

void print_job_table(int print_running_jobs, int print_stopped_jobs, int print_done_jobs)
{
    int most_recent_job_num = find_most_recent_job_num();
    if (most_recent_job_num == 0)
    {
        // there are currently no background jobs in the job table
        return;
    }
    // there are background jobs listed
    job_t *curr = job_list_head;
    int job_status;
    int is_most_recent_job;
    while (curr != NULL)
    {
        job_status = curr->status;
        is_most_recent_job = (most_recent_job_num == curr->job_number) ? 1 : 0;
        if (job_status == RUNNING && print_running_jobs == 1 && curr->background)
        {
            print_job(curr, is_most_recent_job, 0, 0);
        }
        else if (job_status == STOPPED && print_stopped_jobs == 1 && curr->background)
        {
            print_job(curr, is_most_recent_job, 0, 0);
        }
        else if (job_status == DONE && print_done_jobs == 1 && curr->background)
        {
            print_job(curr, is_most_recent_job, 0, 0);
        }
        curr = curr->next;
    }
}

// ==== CUSTOM COMMAND FUNCTIONS ==== //
int execute_custom_commands(char *command)
{
    if (strcmp(command, FOREGROUND) == 0)
    {
        execute_fg();
        return 1;
    }
    else if (strcmp(command, BACKGROUND) == 0)
    {
        execute_bg();
        return 1;
    }
    else if (strcmp(command, JOBS) == 0)
    {
        execute_jobs();
        return 1;
    }
    return 0;
}

void execute_bg()
{
    // job statuses may have finished executing in the time of commandline processing to executing this command
    update_job_table_statuses();
    job_t *bg_job = find_next_job_to_bg();
    if (bg_job == NULL)
    {
        // there are no stopped background jobs, so just end execution
        return;
    }
    // update command associated with job to match BASH format when bringing a job into the foreground
    update_job_command_str(bg_job, 1);
    int most_recent_job_num = find_most_recent_job_num();
    int is_most_recent_job = (most_recent_job_num == bg_job->job_number) ? 1 : 0;
    bg_job->status = RUNNING;
    print_job(bg_job, is_most_recent_job, 1, 0);
    // santity check
    bg_job->background = 1;
    // send SIGCONT to bg_job pg, and wait for it in the background
    continue_background_job(bg_job, 0);
}

job_t *find_next_job_to_bg()
{
    // exclude done jobs, but also be sure to take into account if the job already notified the terminal if it was completed
    job_t *curr = job_list_head;
    job_t *job_to_continue = NULL;
    if (curr == NULL)
    {
        return NULL;
    }
    while (curr != NULL)
    {
        if (curr->background && curr->status == STOPPED)
        {
            job_to_continue = curr;
        }
        curr = curr->next;
    }
    return job_to_continue;
}

void execute_fg()
{
    // job statuses may have finished executing in the time of commandline processing to executing this command
    update_job_table_statuses();
    job_t *fg_job = find_next_job_to_fg();
    if (fg_job == NULL)
    {
        // there are no running or stopped background jobs, so just end execution
        return;
    }
    // don't need to update the job number: we can't enter any new commands (and thus no new jobs) since this job will be in the foreground
    // update command associated with job to match BASH format when bringing a job into the foreground
    update_job_command_str(fg_job, 0);
    int most_recent_job_num = find_most_recent_job_num();
    int is_most_recent_job = (most_recent_job_num == fg_job->job_number) ? 1 : 0;
    fg_job->status = RUNNING;
    print_job(fg_job, is_most_recent_job, 1, 1);
    // make sure the job in the job list is no longer a background job
    // do this after finding the most recent job to account for the fact that it might be latest stopped job
    fg_job->background = 0;
    // send SIGCONT to fg_job pg, and wait for it in the foreground
    continue_background_job(fg_job, 1);
}

job_t *find_next_job_to_fg()
{
    // exclude done jobs, but also be sure to take into account if the job already notified the terminal if it was completed
    if (job_list_head == NULL)
    {
        return NULL;
    }
    job_t *curr = job_list_head;
    job_t *job_to_continue = NULL;
    while (curr != NULL)
    {
        if (curr->status != DONE)
        {
            job_to_continue = curr;
        }
        curr = curr->next;
    }
    return job_to_continue;
}

void execute_jobs()
{
    // job statuses may have finished executing in the time of commandline processing to executing this command
    update_job_table_statuses();
    // print DONE jobs
    print_job_table(0, 0, 1);
    // we have to do this due to the manner in which the execution of the shell while loop works
    remove_done_jobs();
    print_job_table(1, 1, 0);
}

// ==== DEBUGGING FUNCTIONS ==== //

void print_job(job_t *job, int is_most_recent_job, int custom_command, int command_is_fg)
{
    char status[20] = "Unknown";
    memset(status, '\0', sizeof(char) * 20);
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

    if (custom_command && command_is_fg)
    {
        // means the command "fg" was run, so print the command directly to the terminal
        // no discrimination whether
        printf("%s\n", job->command);
    }
    else if (is_most_recent_job && custom_command && !command_is_fg)
    {
        // printing job in the "jobs" format for the most recent job created
        printf("[%d]+\t%s\n", job->job_number, job->command);
    }
    else if (!is_most_recent_job && custom_command && !command_is_fg)
    {
        // printing job in the "jobs" format for previous jobs
        printf("[%d]-\t%s\n", job->job_number, job->command);
    }
    else if (is_most_recent_job && !custom_command)
    {
        // printing job in the "job table" format for the most recent job created
        printf("[%d]+\t%s\t\t\t%s\n", job->job_number, status, job->command);
    }
    else if (!is_most_recent_job && !custom_command)
    {
        // printing job in the "job table" format for previous jobs
        printf("[%d]-\t%s\t\t\t%s\n", job->job_number, status, job->command);
    }
}

int find_most_recent_job_num()
{
    // In this case, it okay to include jobs that are DONE. This function is only called in functions where done jobs are printed out before they are erased
    // so any cases where added bg jobs may appear to be +1 of the latest bg job will never happen since done jobs will be printed first!
    job_t *curr = job_list_head;
    int recent_job_num = 0;
    if (curr == NULL)
    {
        return 0;
    }
    int include_job;
    while (curr != NULL)
    {
        // see if we include this job in assessing the most recent job number
        recent_job_num = (curr->job_number > recent_job_num && curr->background) ? curr->job_number : recent_job_num;
        curr = curr->next;
    }
    return recent_job_num;
}

void free_job_table()
{
    job_t *curr = job_list_head;
    job_t *next;
    if (curr == NULL)
    {
        return;
    }
    while (curr != NULL)
    {
        next = curr->next;
        free_job(curr);
        curr = next;
    }
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
    if (job->command)
    {
        free(job->command);
    }
    // free job object itself
    free(job);
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
    // the process struct doesn't have any primitive types, so this is likely not necessary - still good to sanity check!
    free(process);
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
    print_parsed_command_debug(process->argv);
    printf("input file: %s\n", process->redirect_input_filename);
    printf("output file: %s\n", process->redirect_output_filename);
    printf("error file: %s\n", process->redirect_error_filename);
}
