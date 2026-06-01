#pragma once

// Entry point for the kernel shell task.
// Register with the scheduler via task_create(shell_task).
void shell_task(void);