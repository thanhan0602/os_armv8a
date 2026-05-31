#ifndef KERNEL_LOADER_H
#define KERNEL_LOADER_H

struct process;

struct process *loader_load_process_image(const char *path);

#endif