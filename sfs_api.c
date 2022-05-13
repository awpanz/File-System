#include <stdio.h>
#include <stdlib.h> 
#include <string.h>

#include "disk_emu.h" 
#include "sfs_api.h"


/*----------------------------------------------------------------------*/
/*                        Disk structure                                */
/*                                                                      */
/*  | 1 |----------16----------|--------------1006--------------| 1 |   */
/*    ^             ^                           ^                 ^     */
/* superblock   i-node table               data blocks      free bitmap */
/*----------------------------------------------------------------------*/
const int super_block_starting_ind = 0;
const int num_inodes_blcks = 16;
const int i_node_starting_ind = 1;
const int data_starting_ind = i_node_starting_ind + num_inodes_blcks;
const int num_data_blcks = NUM_BLOCKS - 1 /*superblock*/ - num_inodes_blcks - 1 /*free bitmap*/;
const int max_num_inodes = num_inodes_blcks * BLOCK_SIZE/sizeof(i_node);
const int num_directptr = 12;
int dir_entry_per_block = BLOCK_SIZE/sizeof(dir_entry); 
int inode_per_block = BLOCK_SIZE/sizeof(i_node);
int max_file_size = num_directptr*BLOCK_SIZE + BLOCK_SIZE/sizeof(indirect_ptr) * BLOCK_SIZE;

/*----------------*/
/* CACHE ELEMENTS */
/*----------------*/
// Free bitmap should be initialised with the number of blocks in the disk
unsigned char * freebitmapCACHE;
// Our cache will hold the directory contents
// Size of directory entry is not a factor of Block Size => 
// There will be internal waste in each block (we will not split directory entry accross multiple blocks)
// Number of entries per block should be floor of 1024/28 = 36 => real size is 1008 => 16 bytes of internal waste
// Support 5 directory blocks in the cache => 4*36 = 144 entries
const int max_cache_directory_entries = 144;
// Must be hardcoded, compiler does not recognize constant for max_cache_directory_entries
dir_entry * directoryCACHE[144];
// Super block cache 
super_block * superblockCACHE;
// There are 16 i node blocks and 16 i nodes per block
// This will hold at most 256 i nodes
// Must be hardcoded, compiler does not recognize constant for max_num_inodes
i_node * inodetableCACHE[256];


// Open File Descriptor Table
open_entry * open_fdt[MAX_OPEN_FILE];

int next_file_directory_index = 0;

/* Method to update in the cache and the disk the free bitmap table */
int update_freebitmap_CACHE_and_DISK(int blockIndex, int flag)
{
    unsigned char * freebitmapCACHE_temp = freebitmapCACHE + blockIndex;
    if(flag == 1)
    {
        *freebitmapCACHE_temp = '1';
    }
    else if(flag == 0)
    {
        *freebitmapCACHE_temp = '0';
    }
    else
    {
        return -1;
    }
    write_blocks(NUM_BLOCKS - 1, 1, freebitmapCACHE);
    return 0;   
}

void mksfs(int fresh)
{
    char * filename = "sfs_file";

    char * superblock = (char *) malloc(BLOCK_SIZE);
    unsigned char * freebitmap = (unsigned char *) malloc(BLOCK_SIZE);
    
    if(!fresh)
    {
        // Get existing disk
        init_disk(filename, BLOCK_SIZE, NUM_BLOCKS);

        // Reset the file directory index to the start of the directory
        next_file_directory_index = 0;

        /*-------------------------*/
        /* Create superblock cache */
        /*-------------------------*/
        // Read superblock from memory
        char * superblock_disk = (char *) malloc(BLOCK_SIZE);
        read_blocks(0, 1, superblock_disk);
        super_block * sb_disk = (super_block *) superblock_disk;
        super_block * sb_cache = (super_block *) superblock;
        // Copy disk contents to cache
        sb_cache->magic = sb_disk->magic;
        sb_cache->block_size = sb_disk->block_size;
        sb_cache->file_sys_len = sb_disk->file_sys_len;
        sb_cache->i_node_len = sb_disk->i_node_len;
        sb_cache->i_rootdir = sb_disk->i_rootdir;
        sb_cache->num_inodes = sb_disk->num_inodes; 
        sb_cache->dir_num_elements = sb_disk->dir_num_elements;

        superblockCACHE = (super_block *) superblock;
        free(superblock_disk);

        /*--------------------------*/
        /* Create inode table cache */
        /*--------------------------*/
        char * inodetable_disk = (char *) malloc(BLOCK_SIZE);
        for(int j = 0; j < num_inodes_blcks; j++)
        {
            // Copy current inode block from disk
            read_blocks(i_node_starting_ind + j, 1, inodetable_disk);
            for(int i = 0; i < inode_per_block; i++)
            {
                i_node * in = (i_node *) malloc(sizeof(i_node));
                in->valid = ((i_node *) (inodetable_disk + i * sizeof(i_node)))->valid;
                in->num_indirectptr = ((i_node *) (inodetable_disk + i * sizeof(i_node)))->num_indirectptr;
                in->size = ((i_node *) (inodetable_disk + i * sizeof(i_node)))->size;
                for(int k = 0; k < num_directptr; k++)
                {
                    in->directptr[k] = ((i_node *) (inodetable_disk + i * sizeof(i_node)))->directptr[k];
                }
                in->indirectptr = ((i_node *) (inodetable_disk + i * sizeof(i_node)))->indirectptr;

                inodetableCACHE[j*inode_per_block + i] = in;
            }
        }

        free(inodetable_disk);
        
        /*--------------------------*/
        /* Create freebit map cache */
        /*--------------------------*/
        char * freebitmap_disk = (char *) malloc(BLOCK_SIZE);
        // Copy disk content to cache
        unsigned char * freebitmap_cache_bit = freebitmap;
        unsigned char * freebitmap_disk_bit = (unsigned char *) freebitmap_disk;
        for (int i = 0; i < BLOCK_SIZE; i++)
        {
            *freebitmap_cache_bit = *freebitmap_disk_bit;
            freebitmap_cache_bit++;
            freebitmap_disk_bit++;
        }

        freebitmapCACHE = freebitmap;
        free(freebitmap_disk);

        /*------------------------*/
        /* Create directory cache */
        /*------------------------*/
        char * directory_block_disk = (char *) malloc(BLOCK_SIZE);
        i_node * dir_inode = inodetableCACHE[superblockCACHE->i_rootdir];
        int num_dir_entries = dir_inode->size / sizeof(dir_entry);
        int num_dir_blocks = num_dir_entries / dir_entry_per_block;
        
        // Copy all blocks pointed to by the direct pointers of the directory inode
        for(int i = 0; i < num_dir_blocks && i < num_directptr; i++)
        {
            // Read each direct pointer block
            read_blocks(data_starting_ind + dir_inode->directptr[i], 1, directory_block_disk);
            
            for(int j = 0; j < dir_entry_per_block; j++)
            {
                dir_entry * dir_e = (dir_entry *) malloc(sizeof(dir_entry));      

                dir_e->valid = ((dir_entry *) (directory_block_disk + j * sizeof(dir_entry)))->valid;
                strcpy(dir_e->filename, ((dir_entry *) (directory_block_disk + j * sizeof(dir_entry)))->filename);
                dir_e->i_node = ((dir_entry *) (directory_block_disk + j * sizeof(dir_entry)))->i_node;
                
                
                directoryCACHE[i * dir_entry_per_block + j] = dir_e;
            }
        }

        // If there is an indirect pointer block
        if(dir_inode->indirectptr != -1)
        {
            // Copy all blocks pointed to by the indirect pointers of the directory inode
            // Get the indirect pointer block
            char * indirectptr_block_disk = (char *) malloc(BLOCK_SIZE);
            read_blocks(data_starting_ind + dir_inode->indirectptr, 1, indirectptr_block_disk);
            
            // For each pointer in the indirect pointer block, go to their block
            for(int i = 0; i < (num_dir_blocks - num_directptr) && i < BLOCK_SIZE/sizeof(indirect_ptr); i++)
            {
                int datablock_index = ((indirect_ptr *) (indirectptr_block_disk + i * sizeof(indirect_ptr)))->datablockindex;
                // Read each direct pointer block
                read_blocks(data_starting_ind + datablock_index, 1, directory_block_disk);

                for(int j = 0; j < dir_entry_per_block; j++)
                {
                    dir_entry * dir_e = (dir_entry *) malloc(sizeof(dir_entry));
                    dir_e->valid = ((dir_entry *) (directory_block_disk + j * sizeof(dir_entry)))->valid;
                    strcpy(dir_e->filename, ((dir_entry *) (directory_block_disk + j * sizeof(dir_entry)))->filename);
                    dir_e->i_node = ((dir_entry *) (directory_block_disk + j * sizeof(dir_entry)))->i_node;
                    directoryCACHE[num_directptr * dir_entry_per_block + i * dir_entry_per_block + j] = dir_e;
                }
            }

           free(indirectptr_block_disk);
        }

        free(directory_block_disk);
    }
    else 
    {
        /*-----------------*/
        /* Create new disk */
        /*-----------------*/
        init_fresh_disk(filename, BLOCK_SIZE, NUM_BLOCKS);

        /*-------------------*/
        /* Create superblock */
        /*-------------------*/
        super_block * sb = (super_block *) superblock;
        sb->magic = (int) 0xACBD0005;
        sb->block_size = BLOCK_SIZE;
        sb->file_sys_len = NUM_BLOCKS;
        sb->i_node_len = 64;
        sb->i_rootdir = 0;
        sb->num_inodes = 1; // Start at 1 because we have the directory i node
        sb->dir_num_elements = 0;  // Start with 0 elements in the directory

        // The super block is the first block of the disk
        write_blocks(0, 1, superblock);
        // Update the cache to reflect the current state of the super block
        superblockCACHE = (super_block *) superblock;

        /*-------------------------*/
        /* Create Directory I Node */
        /*-------------------------*/
        // Create the i node for the directory, size should be 64 bytes
        char * i_node_rootdir = (char *) malloc(sizeof(i_node));
        i_node * in = (i_node *) i_node_rootdir;
        in->valid = 1; 
        in->num_indirectptr = 0;
        // Directory starts by being empty, No directory entries to start with
        in->size = 0;
        // Initialize unused pointers at -1
        for(int i = 0; i < num_directptr; i++)
        {
            in->directptr[i] = -1;
        }
        in->indirectptr = -1;

        // Write directory i node to i node table
        write_blocks(i_node_starting_ind, 1, i_node_rootdir);

        // Start i node table cache 
        // There will be 256 i nodes at most in the cache
        inodetableCACHE[0] = (i_node *) i_node_rootdir;
        // Initialize every other inode element in the cache table to invalid
        for(int i = 1; i < max_num_inodes; i++)
        {
            i_node * in = (i_node *) malloc(sizeof(i_node));
            in->valid = 0;
            in->num_indirectptr = 0;
            in->size = 0;
            for(int j = 0; j < num_directptr; j++)
            {
                in->directptr[j] = -1;
            }
            in->indirectptr = -1;

            inodetableCACHE[i] = in;
        }

        /*--------------------*/
        /* Create free bitmap */
        /*--------------------*/
        // Size is number of blocks in disk (will fit in 1 block)
        unsigned char * freebitmaptemp = freebitmap;
        // Initialize to 1 every block (FREE)
        for (int i = 0; i < BLOCK_SIZE; i++)
        {
            *freebitmaptemp = '1';
            freebitmaptemp++;
        }
        // Unfree superblock
        freebitmaptemp = freebitmap;
        *freebitmaptemp = '0';

        // Unfree first i node block
        freebitmaptemp = freebitmap + i_node_starting_ind;
        *freebitmaptemp = '0';

        // Unfree bitmap block
        freebitmaptemp = freebitmap + NUM_BLOCKS - 1;
        *freebitmaptemp = '0';

        // Write free bitmap at last block in disk
        write_blocks(NUM_BLOCKS - 1, 1, freebitmap);
        // Update the cache to reflect the current state of the freebitmap
        freebitmapCACHE = freebitmap;
        
       // Initialize directory entry cache
        for(int i = 0; i < max_cache_directory_entries; i++)
        {
            dir_entry * dir_e = (dir_entry *) malloc(sizeof(dir_entry));
            dir_e->valid = 0;
            dir_e->filename[0] = '\0';
            directoryCACHE[i] = dir_e;
        }
    }

    // We will have a new fdt even if we import an existing file system as it resides in the program memory
    // Set every element of the open fdt at invalid so that we can override any element in the table
    for(int i = 0; i < MAX_OPEN_FILE; i++)
    {
        open_entry * open_e = (open_entry *) malloc(sizeof(open_entry));
        open_e->valid = 0;
        open_fdt[i] = open_e;
    }
}

int sfs_getnextfilename(char* fname)
{
    int count = -1;

    int i = -1;
    while(count < next_file_directory_index && i < (max_cache_directory_entries - 1))
    {
        i++;
        if(directoryCACHE[i]->valid)
        {
            count++;
        }
    }

    if(count == next_file_directory_index)
    {
        memcpy(fname, directoryCACHE[i]->filename, MAX_FILENAME_LEN);
        next_file_directory_index++;
        return 1;
    }

    return 0;
}

int sfs_getfilesize(const char* path)
{
    for(int i = 0; i < max_cache_directory_entries; i++)
    {
        dir_entry * direntry = directoryCACHE[i];
        if(direntry->valid)
        {
            // If it is the file we are looking for
            if(strcmp(direntry->filename, path) == 0)
            {
                // Find the associated inode 
                if(inodetableCACHE[direntry->i_node]->valid)
                {
                    // Return the size of the file stored in the file inode
                    return inodetableCACHE[direntry->i_node]->size;
                }
                else
                {
                    printf("Invalid inode for the fdt\n");
                    return -1;
                }
                
            }
        }
    }

    // Return -1 in case of file not found
    return -1;
}

/* Find a random free block in the data blocks using the free bitmap */
// Return DATA BLOCK index (need be added to starting data block index) on sucess
// Return -1 if no more free data blocks
// Take as argument an update free bitmap flag, set to 1 will update the freebitmap in cache and disk
int find_free_data_block(int updateFreebitmap)
{
    // Look at free bitmap from disk 
    // We want to look at the data blocks in the range [data_starting_ind, index_last_data_block]
    int data_last_ind = NUM_BLOCKS - 1;
    int total_data_blocks = data_last_ind - data_starting_ind;
    // Randomize starting index to verify in the data blocks 
    int data_index = rand() % (total_data_blocks);

    for(int i = 0; i < total_data_blocks; i++)
    {
        if(*(freebitmapCACHE + (data_index % total_data_blocks)) == '1')
        {
            if(updateFreebitmap)
            {
                // Make bit unvailable
                update_freebitmap_CACHE_and_DISK(data_starting_ind + data_index, 0);        
            }
            
            return data_index;
        }
        else 
        {
            data_index += 1;
        }
    }
    
    // No more free data blocks
    return -1;
}

/* Save the current inode table cache to the disk */
// Take as argument the modified block index to save
void save_inodetableCACHE_to_DISK(int inodetable_blockIndex)
{
    // Copy the contents of the block in byte structure
    char * inode_block = (char *) malloc(BLOCK_SIZE);

    // Copy every inode of the block to be updated
    for(int i = 0; i < inode_per_block; i++)
    {
        char * inode_block_temp = inode_block + i*sizeof(i_node);
        
        // Source
        i_node * incache = inodetableCACHE[inodetable_blockIndex*inode_per_block + i]; 
        
        // Target
        i_node * indisk = (i_node *) inode_block_temp;
        indisk->valid = incache->valid;
        indisk->size = incache->size;
        indisk->num_indirectptr = incache->num_indirectptr;
        for(int j = 0; j < num_directptr; j++)
        {
            indisk->directptr[j] = incache->directptr[j];
        }
        indisk->indirectptr = incache->indirectptr;
    }
    write_blocks(i_node_starting_ind + inodetable_blockIndex, 1, inode_block);
    free(inode_block);
    
    // Verify if it is a new block in the free bitmap cache 
    // If the bit is set to '1', this block was free => update the block to be unavailable
    if(*(freebitmapCACHE + i_node_starting_ind + inodetable_blockIndex) == '1')
    {    
        update_freebitmap_CACHE_and_DISK(i_node_starting_ind + inodetable_blockIndex, 0);
    }
}


void save_directoryCACHE_to_DISK(int dirIndex)
{
    // Represents the block of data of directory to be copied to disk
    int blockIndex = dirIndex/dir_entry_per_block;
    // Copy the contents of the block in byte structure
    char * directory_block = (char *) malloc(BLOCK_SIZE);

    // Actual block to write to memory
    int dirBlock;

    // Copy every directory entry of the block to be updated
    for(int i = 0; i < dir_entry_per_block; i++)
    {
        char * dir_block_temp = directory_block + i*sizeof(dir_entry);
        // Source
        dir_entry * dir_entry_cache = directoryCACHE[blockIndex*dir_entry_per_block + i]; 
        
        // Target
        dir_entry * dir_entry_disk = (dir_entry *) dir_block_temp;

        // Copy from cache to disk
        dir_entry_disk->valid = dir_entry_cache->valid;
        strcpy(dir_entry_disk->filename, dir_entry_cache->filename);
        dir_entry_disk->i_node = dir_entry_cache->i_node;
    }

    // Find the data block in memory for the inodetable_datablockIndex
    // If first 12 blocks, it will be the index of the directptr
    if(blockIndex < num_directptr)
    {
        dirBlock = inodetableCACHE[superblockCACHE->i_rootdir]->directptr[blockIndex];
    }
    // It is an indirect pointer, must read the indirectptr block
    else
    {
        char * indirectptr = (char *) malloc(BLOCK_SIZE);
        int indirectptrblock = data_starting_ind + inodetableCACHE[superblockCACHE->i_rootdir]->indirectptr;
        read_blocks(indirectptrblock, 1, indirectptr);
        
        indirect_ptr * indptr = (indirect_ptr *) (indirectptr + (blockIndex - num_directptr) * sizeof(indirect_ptr));
        dirBlock = indptr->datablockindex;
        free(indirectptr);
    }

    write_blocks(data_starting_ind + dirBlock, 1, directory_block);
    free(directory_block);

    // Verify if it is a new block in the free bitmap cache 
    // If the bit is set to '1', this block was free => update the block to be unavailable
    if(*(freebitmapCACHE + data_starting_ind + blockIndex) == '1')
    {
        update_freebitmap_CACHE_and_DISK(data_starting_ind + dirBlock, 0);
    }
}

/* Save a new data block to the corresponding i node entry*/
// Update the cache and the disk of the inode
// Return 0 on success, -1 on failure
int add_data_block_to_inode(int datablockIndex, int inodeIndex)
{
    char * block = (char *) malloc(BLOCK_SIZE);
    int indirectptr_block;

    if(inodeIndex >= max_num_inodes || inodeIndex < 0)
    {
        printf("Out of bound inodeIndex %d\n", inodeIndex);
        return -1;
    }

    i_node * in = inodetableCACHE[inodeIndex];
    
    if(in->valid)
    {
        for(int i = 0; i < num_directptr; i++)
        {
            if(in->directptr[i] < 0)
            {
                in->directptr[i] = datablockIndex;
                return 0;
            }
        }

        // If not found in directptr, look in indirect ptr
        // If no indirectptr, add block and add entry
        if(in->indirectptr < 0)
        {
            // Find free data block for indirect ptr block and update the free bit map
            indirectptr_block = find_free_data_block(1);
            if(indirectptr_block < 0)
            {
                return -1;
            }

            // Udpate inode in cache
            in->indirectptr = indirectptr_block;
            in->num_indirectptr = in->num_indirectptr + 1;

            // Add data block
            block = (char *) malloc(BLOCK_SIZE);
            indirect_ptr * indirectptr = (indirect_ptr *) block;
            indirectptr->datablockindex = datablockIndex;
        }
        // Add to existing indirect pointers
        else
        {
            // Verify if space remaining in the indirect block
            if(in->num_indirectptr < BLOCK_SIZE/sizeof(indirect_ptr))
            {
                indirectptr_block = in->indirectptr;
                in->num_indirectptr = in->num_indirectptr + 1;

                block = (char *) malloc(BLOCK_SIZE);
                // Copy current block and append
                read_blocks(indirectptr_block + data_starting_ind, 1, block);
                // Go to end of entries in block
                char * block_temp = block + in->num_indirectptr * sizeof(indirect_ptr);
                indirect_ptr * indirectptr = (indirect_ptr *) block_temp;
                indirectptr->datablockindex = datablockIndex;
            }
            // No space remaining => we are at the max file size
            else
            {
                return -1;
            }
        }

        // After updating cache and preparing data structures, write to disk
        write_blocks(indirectptr_block + data_starting_ind, 1, block);

        // Save updated inode table to disk
        int inodetableblock = inodeIndex/num_inodes_blcks; 
        save_inodetableCACHE_to_DISK(inodetableblock);
    }
    else 
    {
        printf("Add data block to inode with invalid inode index %d\n", inodeIndex);
        return -1;
    }

    return 0;
}

int add_directory_entry(char * name, int inodeIndex)
{
    int dirIndex = -1;
    int dir_data_block_index;

    // Get the i node for the root directory from the superblock
    int dir_inode_index = superblockCACHE->i_rootdir;
    // Number of valid elements in the directory
    int dir_num_elements = superblockCACHE->dir_num_elements;

    // Go to i node of root directory from cache 
    i_node * directory_in = inodetableCACHE[dir_inode_index];

    // Current size of data in the directory, will need to be updated with the new directory entry
    int directory_size = directory_in->size;
    // Total number of directory entries, valid or invalid
    // This will indicate how many data blocks are used for the directory
    int total_dir_entries = directory_size/sizeof(dir_entry);

    /*--------------------------------*/
    /* Find new directory entry index */
    /*--------------------------------*/
    // If the number of valid entries == number of valid + invalid entries ==> invalid entries = 0
    // Therefore we can go to the next to last index
    if(dir_num_elements == total_dir_entries)
    {
        dirIndex = dir_num_elements;
    }
    // If not, we know between index 0 and index total_dir_entries - 1, there will be an invalid entry we can update
    // Find this invalid entry
    else
    {
        for(int i = 0; dirIndex < 0 && i < total_dir_entries; i++)
        {
            dir_entry * curentry = directoryCACHE[i];
            if(!(curentry->valid))
            {
                dirIndex = i;
            }
        }
    }

    int direntry_block_index = dirIndex/dir_entry_per_block;
    
    // Update the data block index to be the new direntry block index
    // If we need to create a new block index in the directory, we will update this value
    dir_data_block_index = direntry_block_index;

    // After these steps, dirIndex can either be an existing index in range [0, total_dir_entries[ or a new index = dir_num_elements
    // If the index is existing, we can update the cache and disk
    // If not existing, we need to verify if we need to create a new data block for the next entry  
    if(dirIndex == dir_num_elements)
    {
        // Update size of directory i node, contain 1 more entry
        directory_in->size = directory_size + sizeof(dir_entry);
        // Save inode modification on the disk
        save_inodetableCACHE_to_DISK(dir_inode_index/inode_per_block);

        // Verify if new block is necessary
        // If previous index and new index are not the same block, NEED NEW BLOCK
        if(dirIndex == 0 || (dirIndex - 1)/dir_entry_per_block != direntry_block_index)
        {
            // Find available data block 
            // The index returned is starting at data block 0. 
            // Update the data block index to point the newly allocated block in data blocks
            dir_data_block_index = find_free_data_block(1 /* Flag to update freebitmap */);
            // Verify if data block was available
            if(dir_data_block_index < 0)
            {
                return -1;
            }

            // Add new data block to directory inode in CACHE
            int r = add_data_block_to_inode(dir_data_block_index, dir_inode_index);
            // Verify if error in the previous method
            if(r == -1)
            {
                return -1;
            }

        }
    }

    /*-------------------------------*/
    /* Udpate new dir entry in CACHE */
    /*-------------------------------*/
    dir_entry * direntry = directoryCACHE[dirIndex];
    strcpy(direntry->filename, name);
    direntry->valid = 1;
    direntry->i_node = inodeIndex;

    /*--------------------------*/
    /* Udpate directory in Disk */
    /*--------------------------*/
    save_directoryCACHE_to_DISK(dirIndex);

    /*------------------------*/
    /* Update directory inode */
    /*------------------------*/
    // The size was previously updated if we added a new directory entry in the cache 
    // It no new directory entry, inode table is up to date

    /*-------------------*/
    /* Update Superblock */
    /*-------------------*/
    // Add new directory entry to directory, update number of directory entries
    superblockCACHE->dir_num_elements = dir_num_elements + 1;
    // Udpate superblock to disk
    write_blocks(0, 1, (char *) superblockCACHE);

    return 0;
}

int sfs_fcreate(char* name)
{
    int inodeIndex = -1;
    i_node * file_inode = NULL;

    /*---------------------------*/
    /* Verify inode availability */
    /*---------------------------*/
    if(superblockCACHE->num_inodes == max_num_inodes)
    {
        // No i node available to create a new file
        printf("Max inode number reached, cant create new file\n");
        return -1; 
    }
    
    /*----------------------*/
    /* Find available inode */
    /*----------------------*/
    for(int i = 0; inodeIndex < 0 && i < max_num_inodes; i++)
    {
        i_node * in = inodetableCACHE[i];
        if(!(in->valid)) 
        {
            inodeIndex = i;
            file_inode = in;
        }   
    }  

    /*---------------------------*/
    /* Update new inode in CACHE */
    /*---------------------------*/
    file_inode->valid = 1;
    // Not yet written to file => size is 0
    file_inode->size = 0;
    for (int i = 0; i < num_directptr; i++)
    {
        file_inode->directptr[i] = -1;
    }
    file_inode->indirectptr = -1;
    file_inode->num_indirectptr = 0;

    /*-------------------------------------------*/
    /* Persist change to inodetableCACHE to DISK */
    /*-------------------------------------------*/
    // Get the block index of the inodetable element
    int inodetable_block_ind = inodeIndex / inode_per_block;
    save_inodetableCACHE_to_DISK(inodetable_block_ind);

    /*------------------*/
    /* Add to directory */
    /*------------------*/
    int r = add_directory_entry(name, inodeIndex);
    // Verify if error in the previous method
    if(r == -1)
    {
        return -1;
    }

    /*-------------------*/
    /* Update Superblock */
    /*-------------------*/
    // Add new i node entry, update number of valid i nodes in the superblock
    superblockCACHE->num_inodes = superblockCACHE->num_inodes + 1;
    // Udpate superblock to disk
    write_blocks(0, 1, (char *) superblockCACHE);

    return inodeIndex;
}

// We can only have one instance of the file opened at a time
int sfs_fopen(char* name)
{
    int fileFound = 0;
    int inodeIndex = -1;

    // Illegal length
    if(strlen(name) > MAX_FILENAME_LEN)
    {
        printf("Filename provided is too long. Max is %d but name is %ld\n", MAX_FILENAME_LEN, sizeof(name));
        return -1;
    }

    // I node of the directory 
    i_node * dir_i_node = inodetableCACHE[superblockCACHE->i_rootdir];  
    // Size of directory in data block (valid+invalid), bytes
    int dir_size = dir_i_node->size;

    /*------------------*/
    /* Find File i node */
    /*------------------*/
    int num_dir_entries = dir_size/sizeof(dir_entry);
    // Look through cache
    for(int i = 0; !fileFound && i < num_dir_entries; i++)
    {
        // Get ith element of the directory cache
        dir_entry * direntry = directoryCACHE[i];
        if(direntry->valid)
        {
            if(strcmp(name, direntry->filename) == 0)
            {
                inodeIndex = direntry->i_node;
                fileFound = 1;
            }
        }
    }

    /*-----------------*/
    /* Create new file */
    /*-----------------*/
    if(!fileFound)
    {
        inodeIndex = sfs_fcreate(name);
    }

    /*-----------------*/
    /* Add to open FDT */
    /*-----------------*/
    if(inodeIndex > -1)
    {
        i_node * file_inode = inodetableCACHE[inodeIndex];

        int openIndex = -1;
        for(int i = 0; i < MAX_OPEN_FILE; i++)
        {
            open_entry * open_e = open_fdt[i];
            // If entry is valid, verify if it points to the same file
            if(open_e->valid)
            {
                if(open_e->iptr == inodeIndex)
                {
                    return i;
                }
            }
            else
            {
                // If it is the first invalid entry, set the openIndex so we can add 
                // the new file entry at this index if it is not already opened
                if(openIndex == -1)
                {
                    openIndex = i;
                }
            }
            
        }
        
        // We can add the entry if we did not find the file in the open file table
        if(openIndex > -1)
        {
            open_fdt[openIndex]->valid = 1;
            // Start the file ptr in append mode
            open_fdt[openIndex]->fileptr = file_inode->size;
            // Associate the inode pointer to the current inode
            open_fdt[openIndex]->iptr = inodeIndex;

            // Return the open fdt index
            return openIndex;
        }
    }
    
    // Unsuccessful operation, return error
    return -1;
}

int sfs_fclose(int fileID)
{
    if(fileID > -1 && fileID < MAX_OPEN_FILE)
    {
        open_entry * open_e = open_fdt[fileID];
        if(open_e->valid == 0)
        {
            // Already closed file
            return -1;
        }
        open_e->valid = 0;

        return 0;
    }

    // If fileID is not in the appropriate range, return error -1
    return -1;
}

int sfs_fwrite(int fileID, const char* buf, int length)
{
    // Get the entry associated with the fileID
    open_entry * openentry = open_fdt[fileID];
    // The open entry will point to inode number
    int inodeIndex = openentry->iptr;
    int fileptr = openentry->fileptr;
    // Get the inode from the cache (always up to date)
    i_node * inode = inodetableCACHE[inodeIndex];
    // This is the index of the first block, we will need to find which data block it points to in the inode
    int writeblockindex = fileptr/BLOCK_SIZE;
    // update fileptr to point to specific block location
    int fileptr_write = fileptr % BLOCK_SIZE;

    int remaining_len = length;
    
    int datablock;
    int writesize = 0;
    char * currentBufSrc = (char *) buf;

    if(!openentry->valid)
    {
        // If the file was closed, we can't write to it
        return 0;
    }

    // File pointer points outside of the maximum scope of the file, max file size was reached
    if(writeblockindex >= num_directptr + BLOCK_SIZE/sizeof(indirect_ptr))
    {
        return writesize;
    }

    while(remaining_len > 0)
    {
        /*----------------*/
        /* Get data block */
        /*----------------*/
        if(writeblockindex < num_directptr)
        {
            datablock = inode->directptr[writeblockindex];

            // We need to create a new block
            if(datablock == -1)
            {
                datablock = find_free_data_block(1);
                if(datablock == -1)
                {
                    // No more space in the disk
                    printf("No more space to create a datablock\n");
                    break;
                }
                else
                {
                    // Udpate file inode to point to the new data block
                    inode->directptr[writeblockindex] = datablock;
                }
            }
        }
        else
        {
            // This holds the data block of the indirect pointer
            int indirectptrblock;

            // We point at the begining of the first block of the indirect pointers
            if(inode->indirectptr == -1)
            {
                // Find new data block to hold the indirect pointers
                indirectptrblock = find_free_data_block(1);
                datablock = find_free_data_block(1);
                if(indirectptrblock == -1 || datablock == -1)
                {
                    // No more space in the disk
                    printf("invalid block returned\n");
                    break;
                }
                else
                {
                    // Update inode to point to new data block
                    inode->num_indirectptr = inode->num_indirectptr + 1;
                    inode->indirectptr = indirectptrblock;

                    // Write indirect ptr block to the disk
                    char * indirectptrblock_todisk = (char *) malloc(BLOCK_SIZE);
                    indirect_ptr * indptr_entry = (indirect_ptr *) indirectptrblock_todisk;
                    indptr_entry->datablockindex = datablock;
                    write_blocks(data_starting_ind + indirectptrblock, 1, indirectptrblock_todisk);
                    free(indirectptrblock_todisk);                
                }
            }
            else
            {
                indirectptrblock = inode->indirectptr;

                // Find the index of the indirect pointer entry in the indirect pointer
                int indirectptrIndex = writeblockindex - num_directptr;

                char * indirectptrblock_fromdisk = (char *) malloc(BLOCK_SIZE);
                read_blocks(data_starting_ind + indirectptrblock, 1, indirectptrblock_fromdisk);
                
                if(indirectptrIndex < inode->num_indirectptr)
                {
                    // We are looking at an existing block 
                    // We can simply get the data block index from the pointer 
                    datablock = ((indirect_ptr *) (indirectptrblock_fromdisk + indirectptrIndex * sizeof(indirect_ptr)))->datablockindex;
                }
                else 
                {
                    // We need to create a new indirect pointer if remaining space
                    if(inode->num_indirectptr < BLOCK_SIZE/sizeof(indirect_ptr))
                    {
                        // Update number of indirect pointers in the inode of the file 
                        inode->num_indirectptr = inode->num_indirectptr + 1;

                        // Find free data block for the new file block
                        datablock = find_free_data_block(1);
                        indirect_ptr * indptr_entry = (indirect_ptr *) (indirectptrblock_fromdisk + indirectptrIndex * sizeof(indirect_ptr));
                        indptr_entry->datablockindex = datablock;
                        write_blocks(data_starting_ind + indirectptrblock, 1, indirectptrblock_fromdisk);
                        free(indirectptrblock_fromdisk);                        
                    }
                    else 
                    {
                        // We have reached the maximum file length, can no longer write to file
                        break;
                    }
                }
            }
        }

        // Data block should point to the data block on disk to which we need to write

        // On a single block we can write writelen    
        int writelen = BLOCK_SIZE - fileptr_write;

        /*-------------------------*/
        /* Get datablock from disk */
        /*-------------------------*/
        char * datablock_fromdisk = (char *) malloc(BLOCK_SIZE);
        // Copy content of current data block
        read_blocks(data_starting_ind + datablock, 1, datablock_fromdisk);
        
        char * datablock_startcopy = datablock_fromdisk + fileptr_write;

        // Copy the smallest buffer size between block remaining space and total buffer remaining
        if(writelen < remaining_len)
        {
            memcpy(datablock_startcopy, currentBufSrc, writelen);
            // Update the size of the file in the inode with the new write
            inode->size = inode->size + writelen;
            writesize = writesize + writelen;

            // Update buffer to continue writing content 
            currentBufSrc = currentBufSrc + writelen;

            // Update the file ptr to the end of the write
            openentry->fileptr = openentry->fileptr + writelen;
            
            // Remaining length of buffer to be written to memory
            remaining_len = remaining_len - writelen;
        }
        else
        {
            memcpy(datablock_startcopy, currentBufSrc, remaining_len);
            // Update the size of the file in the inode with the new write
            inode->size = inode->size + remaining_len;
            writesize = writesize + remaining_len;

            // Update buffer to continue writing content 
            currentBufSrc = currentBufSrc + remaining_len;

            // Update the file ptr to the end of the write
            openentry->fileptr = openentry->fileptr + remaining_len;

            // Remaining length of buffer to be written to memory
            remaining_len = remaining_len - remaining_len;
        }

        // Write back to disk the modified content of the block
        
        write_blocks(data_starting_ind + datablock, 1, datablock_fromdisk);
    
        // Update fileptr_write to 0, because after first block write, the following writes will always be at 
        // the beginning of the next block, therefore no offset in the block.
        fileptr_write = 0;

        // Update the writeblock index to next block in the list of blocks
        writeblockindex = writeblockindex + 1;
    }
    // Update the file inode on disk
    save_inodetableCACHE_to_DISK(inodeIndex/inode_per_block);

    return writesize;
}

int sfs_fread(int fileID, char* buf, int length)
{
     // Get the entry associated with the fileID
    open_entry * openentry = open_fdt[fileID];
    // The open entry will point to inode number
    int inodeIndex = openentry->iptr;
    int fileptr = openentry->fileptr;
    // Get the inode from the cache (always up to date)
    i_node * inode = inodetableCACHE[inodeIndex];
    // This is the index of the first block, we will need to find which data block it points to in the inode
    int readblockindex = fileptr/BLOCK_SIZE;
    // update fileptr to point to specific block location
    int fileptr_read = fileptr % BLOCK_SIZE;

    int datablock;
    int readsize = 0;
    char * currentBufDest = (char *) buf;

    int remaining_len;
    
    // If we want to read less than the file size, remaining length to read is the length
    if(inode->size > length)
    {
        remaining_len = length;
    } 
    // If we want to read a larger size than the file size, only read file size
    else
    {
        remaining_len = inode->size;
    }

    if(!openentry->valid)
    {
        // If the file was closed, we can't read from it
        return 0;
    }

    // File pointer points outside of the maximum scope of the file, max file size was reached
    if(readblockindex >= num_directptr + BLOCK_SIZE/sizeof(indirect_ptr))
    {
        printf("out of bounds read\n");
        return readsize;
    }

    while(remaining_len > 0)
    {
        /*----------------*/
        /* Get data block */
        /*----------------*/
        if(readblockindex < num_directptr)
        {
            datablock = inode->directptr[readblockindex];

            // We need to create a new block
            if(datablock == -1)
            {
                // No more content to read
                // At the end of the directptr list
                printf("No more direct pointers \n");
                break;
            }
        }
        else
        {
            // This holds the data block of the indirect pointer
            int indirectptrblock;

            // We point at the begining of the first block of the indirect pointers
            if(inode->indirectptr == -1)
            {
                // no more content to read
                // no indirect ptr list
                printf("No indirect pointers \n");

                break;
            }
            else
            {
                indirectptrblock = inode->indirectptr;

                // Find the index of the indirect pointer entry in the indirect pointer
                int indirectptrIndex = readblockindex - num_directptr;

                char * indirectptrblock_fromdisk = (char *) malloc(BLOCK_SIZE);
                read_blocks(data_starting_ind + indirectptrblock, 1, indirectptrblock_fromdisk);

                if(indirectptrIndex < inode->num_indirectptr)
                {
                    // We are looking at an existing block 
                    // We can simply get the data block index from the pointer 
                    datablock = ((indirect_ptr *) (indirectptrblock_fromdisk + indirectptrIndex * sizeof(indirect_ptr)))->datablockindex;
                }
                else 
                {
                    // no more content to read
                    // at the end of the indirectptr list
                    printf("No more indirect pointers \n");
                    break;
                }
            }
        }

        // Data block should point to the data block on disk to which we need to read

        // On a single block we can read    
        int readlen = BLOCK_SIZE - fileptr_read;

        // printf("Readlen %d\n", readlen);

        /*-------------------------*/
        /* Get datablock from disk */
        /*-------------------------*/
        char * datablock_fromdisk = (char *) malloc(BLOCK_SIZE);
        // Copy content of current data block
        read_blocks(data_starting_ind + datablock, 1, datablock_fromdisk);
        
        char * datablock_startread = datablock_fromdisk + fileptr_read;

        // Copy the smallest buffer size between block remaining space and total buffer remaining
        if(readlen < remaining_len)
        {            
            memcpy(currentBufDest, datablock_startread, readlen);
            readsize = readsize + readlen;

            // Update the buffer destination to continue appending buffer
            currentBufDest = currentBufDest + readlen;

            // Update the file ptr to the end of the read
            openentry->fileptr = openentry->fileptr + readlen;
            
            // Remaining remaining_len of buffer to be read from memory
            remaining_len = remaining_len - readlen;
        }
        else
        {
            memcpy(currentBufDest, datablock_startread, remaining_len);
            readsize = readsize + remaining_len;

            // Update the buffer destination to continue appending buffer
            currentBufDest = currentBufDest + remaining_len;

            // Update the file ptr to the end of the read
            openentry->fileptr = openentry->fileptr + remaining_len;

            // Remaining remaining_len of buffer to be read from memory
            remaining_len = remaining_len - remaining_len;
        }

        // Update fileptr_read to 0, because after first block read, the following reads will always be at 
        // the beginning of the next block, therefore no offset in the block.
        fileptr_read = 0;

        // Update the readblock index to next block in the list of blocks
        readblockindex = readblockindex + 1;
    }

    // Update the file inode on disk
    save_inodetableCACHE_to_DISK(inodeIndex/inode_per_block);

    return readsize;
}

int sfs_fseek(int fileID, int loc)
{   
    int inodeIndex = -1;
    // Verify if fileID is valid
    if(fileID > -1 && fileID < MAX_OPEN_FILE)
    {
        if(open_fdt[fileID]->valid)
        {
            inodeIndex = open_fdt[fileID]->iptr;
            // Get inode from cache  
            i_node * in = inodetableCACHE[inodeIndex];

            // If loc is out of range
            if(loc < 0 || loc > in->size)
            {
                return -1;
            }
            else
            {
                open_fdt[fileID]->fileptr = 0;
            }
        }
        else 
        {
            // If the file does not contain valid information
            return -1;
        }
    }
    else 
    {
        // Not valid ID
        return -1;
    }
    
    return 0;
}

int sfs_remove(char* file)
{
    int dir_inode_index = superblockCACHE->i_rootdir;
    i_node * dir_inode = inodetableCACHE[dir_inode_index];
    int dirIndex = -1;
    
    
    /*------------------------*/
    /* Find file in directory */
    /*------------------------*/
    // Loop through all files in the directory
    for(int i = 0; i < dir_inode->size/sizeof(dir_entry); i++)
    {
        //Verify if directory entry is valid
        if(directoryCACHE[i]->valid)
        {
            if(strcmp(directoryCACHE[i]->filename, file) == 0)
            {
                dirIndex = i;
            }
        }
    }

    if(dirIndex == -1)
    {
        printf("Could not find file to remove\n");
        return -1;
    }
    else
    {
        i_node * file_inode = inodetableCACHE[directoryCACHE[dirIndex]->i_node];
        // Get the number of blocks allocated for the file
        // It will be the ceiling of the file size divided by the block size
        int num_block_used = file_inode->size/BLOCK_SIZE + 1;

        /*------------------------------------*/
        /* Free every data block for the file */
        /*------------------------------------*/
        // Iterate through the file's direct pointers
        for(int i = 0; i < num_block_used && i < num_directptr; i++)
        {
            int datablock = file_inode->directptr[i];
            // Free the data blocks in the freebitmap 
            // Set datablock to 1 (free)
            update_freebitmap_CACHE_and_DISK(data_starting_ind + datablock, 1);
        } 

        // Verify if there are indirect pointers to free 
        if(num_block_used > num_directptr)
        {
            int indirectptr_block = file_inode->indirectptr;

            char * indirectptr_fromdisk = (char *) malloc(BLOCK_SIZE);
            read_blocks(data_starting_ind + indirectptr_block, 1, indirectptr_fromdisk);

            // Iterate through the files indirect pointers
            for(int i = 0; i < file_inode->num_indirectptr; i++)
            {
                int datablock = ((indirect_ptr *) indirectptr_fromdisk)->datablockindex;  
                // Free the data blocks in the freebitmap 
                update_freebitmap_CACHE_and_DISK(data_starting_ind + datablock, 1);
            }

            free(indirectptr_fromdisk);
            
            // Free the indirect pointer block
            update_freebitmap_CACHE_and_DISK(data_starting_ind + indirectptr_block, 1);
        }

        /*------------------------------------*/
        /* Remove file inode from inode table */
        /*------------------------------------*/
        // Invalidate cache entry
        inodetableCACHE[directoryCACHE[dirIndex]->i_node]->valid = 0;
        // Udpate cache 
        save_inodetableCACHE_to_DISK(directoryCACHE[dirIndex]->i_node/inode_per_block);

        /*---------------------------------------*/
        /* Remove directory entry from directory */
        /*---------------------------------------*/
        // Invalidate cache entry
        directoryCACHE[dirIndex]->valid = 0;
        // Update disk
        save_directoryCACHE_to_DISK(dirIndex);


        /*-------------------*/
        /* Update superblock */
        /*-------------------*/
        // Update superblock information in cache
        superblockCACHE->dir_num_elements = superblockCACHE->dir_num_elements - 1;
        superblockCACHE->num_inodes = superblockCACHE->num_inodes - 1;
        // Udpate superblock to disk
        write_blocks(0, 1, (char *) superblockCACHE);
    }
    
    // printf("Successfully removed file\n");

    return 0;
}