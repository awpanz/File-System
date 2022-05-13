#ifndef SFS_API_H
#define SFS_API_H

// You can add more into this file.
#define MAX_FILENAME_LEN 20
#define BLOCK_SIZE 1024
#define NUM_BLOCKS 1024
#define MAX_OPEN_FILE 100

typedef struct SUPER_BLOCK
{
    int magic;
    int block_size;
    int file_sys_len;
    int i_node_len;
    int i_rootdir;
    int num_inodes;
    int dir_num_elements;
} super_block;

typedef struct I_NODE
{
    // Total size of i node is 64 bytes => there are 1024/64 = 16 i nodes per block
    int valid;  // If the i node is valid (1), not available to override 
    int num_indirectptr;
    int size;
    int directptr[12];
    int indirectptr;
} i_node;

typedef struct DIRECTORY_ENTRY
{
    // Total size of directory entry is 28 bytes
    char filename[MAX_FILENAME_LEN];
    int valid;
    int i_node;
} dir_entry;

typedef struct OPEN_FILE_ENTRY
{
    int valid;
    int fileptr;
    int iptr;
} open_entry;

typedef struct INDIRECT_PTR_ENTRY
{
    int datablockindex;
} indirect_ptr;


void mksfs(int);

int sfs_getnextfilename(char*);

int sfs_getfilesize(const char*);

int sfs_fopen(char*);

int sfs_fclose(int);

int sfs_fwrite(int, const char*, int);

int sfs_fread(int, char*, int);

int sfs_fseek(int, int);

int sfs_remove(char*);

#endif
