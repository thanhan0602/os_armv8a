#ifndef KERNEL_DEVICE_TREE_H
#define KERNEL_DEVICE_TREE_H

int device_tree_init(unsigned long fdt_pa);
int device_tree_is_valid(void);
unsigned long device_tree_blob_pa(void);
unsigned long device_tree_blob_size(void);

#endif