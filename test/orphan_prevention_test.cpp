/* Copyright (c) 2026 louzt
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 */

#ifdef __linux__
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

int main()
{
	printf("[TEST] Testing Linux PR_SET_PDEATHSIG parent termination behavior...\n");

	int pipe_fds[2];
	if (pipe(pipe_fds) < 0)
		return 1;

	pid_t intermediate_pid = fork();
	if (intermediate_pid < 0)
		return 1;

	if (intermediate_pid == 0)
	{
		// Intermediate parent process (simulating fossilize_replay master process or steam)
		close(pipe_fds[0]);

		pid_t parent_pid = getpid();
		pid_t child_pid = fork();

		if (child_pid == 0)
		{
			// Child process (simulating replayer slave process)
			prctl(PR_SET_PDEATHSIG, SIGKILL);
			if (getppid() != parent_pid)
				_exit(2);

			pid_t my_pid = getpid();
			// Send actual child PID to top-level test runner
			if (write(pipe_fds[1], &my_pid, sizeof(my_pid)) < 0)
				_exit(1);
			close(pipe_fds[1]);

			// Sleep while waiting for parent to die
			while (true)
			{
				sleep(1);
			}
			_exit(0);
		}
		else
		{
			close(pipe_fds[1]);
			// Intermediate parent sleeps until forcibly killed by test runner
			while (true)
			{
				sleep(1);
			}
			_exit(0);
		}
	}

	// Top-level test runner process
	close(pipe_fds[1]);
	pid_t child_pid = 0;
	if (read(pipe_fds[0], &child_pid, sizeof(child_pid)) <= 0)
	{
		fprintf(stderr, "Failed to read child PID from pipe.\n");
		return 1;
	}
	close(pipe_fds[0]);

	printf("[TEST] Spawned intermediate parent PID: %d, worker child PID: %d\n", intermediate_pid, child_pid);

	// Verify child is initially alive
	if (kill(child_pid, 0) != 0)
	{
		fprintf(stderr, "Child process failed to start properly.\n");
		return 1;
	}

	// Forcibly kill intermediate parent process with SIGKILL
	printf("[TEST] Sending SIGKILL to intermediate parent process %d...\n", intermediate_pid);
	kill(intermediate_pid, SIGKILL);

	int status = 0;
	waitpid(intermediate_pid, &status, 0);

	// Give Linux kernel time to deliver PDEATHSIG (SIGKILL) to child
	usleep(100000); // 100ms

	// Verify child process is dead (ESRCH: No such process)
	if (kill(child_pid, 0) == -1 && errno == ESRCH)
	{
		printf("[TEST] SUCCESS: Child process %d was cleanly terminated by PDEATHSIG when parent died!\n", child_pid);
		return 0;
	}
	else
	{
		fprintf(stderr, "[TEST] ERROR: Child process %d is still alive after parent died (ORPHANED)!\n", child_pid);
		// Cleanup orphan process if test fails
		kill(child_pid, SIGKILL);
		return 1;
	}
}
#else
int main()
{
	return 0;
}
#endif
