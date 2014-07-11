using namespace std;

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <uv.h>

#include "globals.h"
#include "jobs.h"
#include "helper-routines.h"
#include "csapp.h"

static char prompt[] = "tsh> ";

void eval(char *cmdline);
int builtin_cmd(char **argv);
void do_bgfg(char **argv);
void waitfg(pid_t pid);

static inline void install_handlers();
static inline void fork_child(char** argv, sigset_t mask);

void sigchld_handler(int sig);
void sigtstp_handler(int sig);
void sigint_handler(int sig);

int main(int argc, char **argv)
{
  int emit_prompt = 1; // emit prompt (default)

  // Redirect stderr to stdout (so that driver will get all output
  // on the pipe connected to stdout)
  dup2(1, 2);

  /* Parse the command line */
  char c;
  while ((c = getopt(argc, argv, "hvp")) != EOF) {
    switch (c) {
    case 'h':             // print help message
      usage();
      break;
    case 'v':             // emit additional diagnostic info
      break;
    case 'p':             // don't print a prompt
      emit_prompt = 0;  // handy for automatic testing
      break;
    default:
      usage();
    }
  }
  install_handlers();

  // Initialize the job list
  initjobs(jobs);

  // Execute the shell's read/eval loop
  for(;;) {
    // Read command line
    if (emit_prompt) {
      printf("%s", prompt);
      fflush(stdout);
    }

    char cmdline[MAXLINE];

    if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin))
      app_error("fgets error");

    if (feof(stdin)) {
      fflush(stdout);
      exit(0);
    }

    // Evaluate command line
    eval(cmdline);
    fflush(stdout);
    fflush(stdout);
  }

  exit(0); //control never reaches here
}

static inline void install_handlers() {
  Signal(SIGINT,  sigint_handler);   // ctrl-c
  Signal(SIGTSTP, sigtstp_handler);  // ctrl-z
  Signal(SIGCHLD, sigchld_handler);  // Terminated or stopped child
  Signal(SIGQUIT, sigquit_handler);
}

/////////////////////////////////////////////////////////////////////////////
//
// eval - Evaluate the command line that the user has just typed in
//
// If the user has requested a built-in command (quit, jobs, bg or fg)
// then execute it immediately. Otherwise, fork a child process and
// run the job in the context of the child. If the job is running in
// the foreground, wait for it to terminate and then return.  Note:
// each child process must have a unique process group ID so that our
// background children don't receive SIGINT (SIGTSTP) from the kernel
// when we type ctrl-c (ctrl-z) at the keyboard.
//

static inline void spawn_child(pid_t* pid, char** argv, sigset_t* mask) {
  *pid = Fork();

  if (*pid == 0){
    if (setpgid(0, 0) < 0)
      unix_error("setgpid error");

    Sigprocmask(SIG_UNBLOCK, mask, NULL);

    if (execve(argv[0], argv, environ) < 0){
      printf("%s: Command not found.\n", argv[0]);
      exit(0);
    }
  }
}

void eval(char *cmdline){
  char *argv[MAXARGS];
  int bg = parseline(cmdline, argv);

  if (argv[0] == NULL)
    return;   /* ignore empty lines */

  pid_t pid;
  sigset_t mask;

  if (!builtin_cmd(argv)){
    // mask before fork
    Sigemptyset(&mask);
    Sigaddset(&mask, SIGCHLD);
    Sigprocmask(SIG_BLOCK, &mask, NULL);

    spawn_child(&pid, argv, &mask);

    if (!bg){
      if (!addjob(jobs, pid, FG, cmdline))
        return;

      Sigprocmask(SIG_UNBLOCK, &mask, NULL);
      waitfg(pid);
    } else{
      if (!addjob(jobs, pid, BG, cmdline))
        return;

      printf("[%d] (%d) %s", pid2jid(pid), pid, cmdline);
      Sigprocmask(SIG_UNBLOCK, &mask, NULL);
    }
  }
  return;
}


/////////////////////////////////////////////////////////////////////////////
//
// builtin_cmd - If the user has typed a built-in command then execute it
//
int builtin_cmd(char **argv){

  if (strcmp(argv[0], "quit") == 0)
    exit(0);

  if (strcmp(argv[0], "&") == 0)
    return 1;

  if (strcmp(argv[0], "jobs") == 0){
    listjobs(jobs);
    return 1;
  }

  if (strcmp(argv[0], "bg") == 0 || strcmp(argv[0], "fg") == 0){
    do_bgfg(argv);
    return 1;
  }

  return 0;
}

/////////////////////////////////////////////////////////////////////////////
//
// do_bgfg - Execute the builtin bg and fg commands
//
void do_bgfg(char **argv)
{
  struct job_t *jobp = NULL;

  /* Ignore command if no argument */
  if (argv[1] == NULL) {
    printf("%s command requires PID or %%jobid argument\n", argv[0]);
    return;
  }

  /* Parse the required PID or %JID arg */
  if (isdigit(argv[1][0])) {
    pid_t pid = atoi(argv[1]);

    if (!(jobp = getjobpid(jobs, pid))) {
      printf("(%d): No such process\n", pid);
      return;
    }

  } else if (argv[1][0] == '%') {
    int jid = atoi(&argv[1][1]);

    if (!(jobp = getjobjid(jobs, jid))) {
      printf("%s: No such job\n", argv[1]);
      return;
    }
  }
  else {
    printf("%s: argument must be a PID or %%jobid\n", argv[0]);
    return;
  }

  if (!kill(-jobp->pid, SIGCONT)){}

  if (strcmp(argv[0], "bg") == 0){
    printf("[%d] (%d)\n", pid2jid(jobp->pid), jobp->pid);
    jobp->state = BG;
  }

  if (strcmp(argv[0], "fg") == 0){
    jobp->state = FG;
    waitfg(jobp->pid);
  }

  return;
}

/////////////////////////////////////////////////////////////////////////////
//
// waitfg - Block until process pid is no longer the foreground process
//
void waitfg(pid_t pid){
  while (pid == fgpid(jobs)){}
}

/////////////////////////////////////////////////////////////////////////////
//
// sigchld_handler - The kernel sends a SIGCHLD to the shell whenever
//     a child job terminates (becomes a zombie), or stops because it
//     received a SIGSTOP or SIGTSTP signal. The handler reaps all
//     available zombie children, but doesn't wait for any other
//     currently running children to terminate.
//
void sigchld_handler(int sig){
  int status;
  pid_t wpid;

  while ((wpid = waitpid(-1, &status, WNOHANG|WUNTRACED)) > 0){
    if (WIFSIGNALED(status))
      printf("Job [%d] (%d) terminated by signal %d\n", pid2jid(wpid), wpid, WTERMSIG(status));


    else if (WIFSTOPPED(status)){
      printf("Job [%d] (%d) stopped by signal %d\n", pid2jid(wpid), wpid, WSTOPSIG(status));
      getjobpid(jobs, wpid)->state = ST;
      return;
    }

    deletejob(jobs, wpid);
  }

  if (errno == 0)
    return;

  if (errno != ECHILD)
    unix_error("waitpid error");

  return;
}

/////////////////////////////////////////////////////////////////////////////
//
// sigint_handler - The kernel sends a SIGINT to the shell whenver the
//    user types ctrl-c at the keyboard.  Catch it and send it along
//    to the foreground job.
//
void sigint_handler(int sig){
  pid_t pid = fgpid(jobs);

  if (pid != 0)
    kill(-pid, sig);

  return;
}

/////////////////////////////////////////////////////////////////////////////
//
// sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
//     the user types ctrl-z at the keyboard. Catch it and suspend the
//     foreground job by sending it a SIGTSTP.
//
void sigtstp_handler(int sig){
  pid_t pid = fgpid(jobs);

  if (pid != 0)
    kill(-pid, sig);

  return;
}
