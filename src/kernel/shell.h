#ifndef KERNEL_SHELL_H
#define KERNEL_SHELL_H

// Command interpreter for the Console window. Runs one typed line; output goes to
// the console log. Call from the desktop task (commands read disks and may start
// programs).
void shell_exec(const char *line);

// Current folder, for the prompt ("/" or "/APPS").
const char *shell_cwd(void);

#endif // KERNEL_SHELL_H
