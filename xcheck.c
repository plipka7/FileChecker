#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "fs.h"

/*File types*/
#define T_DIR  1
#define T_FILE 2
#define T_DEV  3

/*dirents per block*/
#define DPB (BSIZE / sizeof(struct dirent))

/*Direct block addrs per block.*/
#define APB (BSIZE / sizeof(uint))

/*Returns the bit in the bitmap, relative to a bitmap block*/
#define BITINDEX(i) ((7 + 16*((i%BPB)/8)) - (i%BPB))

struct superblock* sb; //pointer to the superblock
char* image_start; //refers to the start of the fs image
int fd;

/*Info on inodes tracked by the program, will be used
  to compare with the values actually stored in the inodes.*/
typedef struct {
  char  type;
  short ref_count;
  char  in_use;
}inode_info;

/*Info on data blocks tracked by the program, will be used
  to ensure no two inodes reference the same data block or
  that the same inode doesn't reference the same data block 
  twice.*/
char* db_info; //data blocks referenced by files and inodes.
uint db_start; //the block number that the data blocks start at.
uint db_end;   //number of the last data block

inode_info* inodes_dirs; //holds the above data for inodes, as seen by directories
inode_info* inodes_files; //holds the above data as listed in the files themselves

static int open_file(char* file, int* fd);
static void check_file(struct dinode* file, int inum);
static void check_dir(struct dinode* dir, int inum);
void print_inode(struct dinode* node, int inum);
//static void check_bitmap(int bm_block, int index);
//static void get_db(char** pos, uint block, char dir);
static void free_mem(void);
static void loop_dirents(uint cur_block, int fb, int inum);
int
main(int argc, char* argv[]) {
  int i;
  int image_size;
  char* cur_pos;
  struct dinode* cur_inode;

  /*Check for correct input*/
  if(argc != 2) {
    fprintf(stderr, "Usage: xcheck <file_system_image>\n");
    exit(1);
  }

  image_size = open_file(argv[1], &fd);

  /*Attempt to map the file into memory*/
  if((image_start = mmap(NULL, image_size, PROT_READ, MAP_PRIVATE,
		fd, 0))  == (caddr_t)-1) {
    printf("map fail\n");
    exit(1);
  }

  /*Super block is the second block in the image*/
  sb = (struct superblock*)(image_start + BSIZE);

  /*Data starts one block past the bitmap block that contains the bit
    for the last block.*/
  db_start = BBLOCK(sb->size, sb->ninodes) + 1;
  db_end = sb->size - 1;
  
  /*Allocate the needed dynamic variables now that we know the sizes*/
  inodes_dirs = malloc(sizeof(inode_info)*sb->ninodes);
  memset(inodes_dirs, 0, 4*sb->ninodes);
  inodes_files = malloc(sizeof(inode_info)*sb->ninodes);
  memset(inodes_dirs, 0, 4*sb->ninodes);
  db_info = malloc(sizeof(char)*sb->size);
  memset(db_info, 'f', sb->nblocks);
  
  /*Start at the beginning of the inodes.*/
  cur_pos = image_start + BSIZE*2;

  /*Loop through all the inodes in the file.*/
  for(i = 0; i < sb->ninodes; i++) {
    cur_inode = (struct dinode*)cur_pos;
    /*Root directory must have inum of ROOTINO*/
    if(i == ROOTINO && cur_inode->type != T_DIR) {
      free_mem();
      fprintf(stderr, "ERROR: root directory does not exist.\n");
      exit(1);
    }
    //print_inode(cur_inode, i);
    /*Action is determined by the type of the inode.*/
    switch(cur_inode->type) {
      /*Inode is not being used.*/
    case 0:
      /*Check that the inode isn't marked as used in the bitmap.*/
      break;
      
      /*Inode refers to a directory.*/
    case T_DIR:
      if(i != ROOTINO && cur_inode->nlink > 1) {
	free_mem();
	fprintf(stderr, "ERROR: directory appears more than once in file system.\n");
	exit(1);
      }
      
      if(cur_inode->nlink < 1) {
	free_mem();
	fprintf(stderr, "ERROR: inode marked use but not found in a directory.\n");
	exit(1);
      }
      /*Put the directory into the global variable.*/
      inodes_files[i].type = 'd';
      inodes_files[i].ref_count = cur_inode->nlink;
      inodes_files[i].in_use = 1;
      check_dir(cur_inode, i);
      break;
      
      /*Inode refers to a file.*/
    case T_FILE:
      /*Check that the file is referenced at least once.*/
      if(cur_inode->nlink < 1) {
	free_mem();
	fprintf(stderr, "ERROR: inode marked use but not found in a directory.\n");
	exit(1);
      }
      /*Put the file into the global variable.*/
      inodes_files[i].type = 'f';
      inodes_files[i].ref_count = cur_inode->nlink;
      inodes_files[i].in_use = 1;
      check_file(cur_inode, i);
      break;

    /*Device type, only have to mark data blocks as in use.*/
    case T_DEV:
      check_file(cur_inode, i);
      break;

      /*Inode type isn't valid*/  
    default:
      fprintf(stderr, "ERROR: bad inode.\n");
      free_mem();
      exit(1);
    }
    cur_pos += sizeof(struct dinode);  //move cur_pos to the next inode
  }

  /*At this point we need to check the global variables to if there values are
    consistent with a consistent file system.
    FIRST: check that the inode table seen by the dirs is the same as the actual ino           de table*/
  for(int j = 2; j < sb->ninodes; j++) {
    if(inodes_dirs[j].in_use != inodes_files[j].in_use) {
      // printf("refs: %d\n", inodes_dirs[j].ref_count);
      // printf("type: %c\n", inodes_files[j].type);
      // printf("inum: %d\n", j);
      // for(int k = 0; k < sb->ninodes; k++)
      //	printf("in_use inum%d: %d\n", k, inodes_dirs[k].in_use);
      // printf("\n");
      free_mem();
      fprintf(stderr, "ERROR: inode marked use but not found in a directory.\n");
      exit(1);
    }

    if(inodes_dirs[j].ref_count != inodes_files[j].ref_count) {
      //printf("inum:%d\n", j);
      //printf("type:%c\nidrc:%d\nifrc:%d\n", inodes_files[j].type, inodes_dirs[j].ref_count,
      //     inodes_files[j].ref_count);
      free_mem();
      fprintf(stderr, "ERROR: bad reference count for file.\n");
      exit(1);
    }
  }

  /*Check db_info against the bitmap.*/
  for(int j = db_start; j < sb->nblocks - 1; j++) {
    /*Get the correct bitmap block*/
    char* bm_block = (char*)(image_start + BBLOCK(j, sb->ninodes)*BSIZE);
    

    /*Find the 4 byte chunk the data block is represented in.*/
    char chunk = bm_block[(j%BPB)/8];
    
    /*Find the bit number*/
    int bit_num = (j%BPB)%8;

    /*Find the bitmap bit value*/
    int bm_bit;
    if((chunk & (1 << bit_num)) > 0)
      bm_bit = 1;
    else
      bm_bit = 0;
    
    /*Make sure the bm_bit matches the expected.*/
    if(bm_bit == 1 && db_info[j] == 'f') {
      free_mem();
      fprintf(stderr, "ERROR: bitmap marks block in use but it is not in use.\n");
      exit(1);
    }
    else if(bm_bit == 0 && db_info[j] == 't') {
      free_mem();
      fprintf(stderr, "ERROR: address used by inode but marked free in bitmap.\n");
      exit(1);
    }
  }
  
  free_mem();
  /*Check that the image at least had a root directory.*/
  if(i < ROOTINO) {
    fprintf(stderr, "ERROR: root directory does not exist.\n");
    exit(1);
  }
  exit(0); //file system is consistent.
}


int
open_file(char* file, int* fd)
{
  struct stat file_info; //store the info for the next file

  /*Open the next file*/
  if((*fd = open(file, O_RDONLY)) < 0) {
    fprintf(stderr, "image not found.\n");
    exit(1);
  }
  /*Get the file size*/
  if(fstat(*fd, &file_info) < 0) {
    fprintf(stderr, "image not found.\n");
    exit(1);
  }
  return file_info.st_size;
}

void
print_inode(struct dinode* node, int inum) {
  printf("INUM: %d\n", inum);
  printf("TYPE: ");
  switch(node->type) {
  case 0:
    printf("NOT USED\n");
    return;

  case 1:
    printf("DIR\n");
    break;

  case 2:
    printf("FILE\n");
    break;

  case 3:
    printf("DEV\n");
    break;
  }
  printf("links: %d\n", node->nlink);
  printf("size: %u\n", node->size);
  return;
}

void
check_file(struct dinode* file, int inum) {
  uint cur_block; //current data block we're working on
  int i;
  uint* id_block; //indirect addr block
  
  // printf("checking file inum%d\n", inum);
  
  /*****Loop through all the addrs in the file.*****/
  /*First the direct addrs*/
  for(i = 0; i < NDIRECT; i++) {
    cur_block = file->addrs[i]; //addr as a block number from image start
    
    /*Check if there are more valid addrs in the inode*/
    if(cur_block == 0)
      continue;

    /*Check that the addr is valid*/
    if(cur_block < db_start || cur_block > sb->size - 1) {
      free_mem();
      fprintf(stderr, "ERROR: bad direct address in inode.\n");
      exit(1);
    }

    /*Update the global variable*/
    if(db_info[cur_block] == 't') {
      /*Addr was already referenced by another inode.*/
      free_mem();
      fprintf(stderr, "ERROR: direct address used more than once.\n");
      exit(1);
    }

    db_info[cur_block] = 't';
  }

  /*Check if an indirect addr is used.*/
  if(i == NDIRECT && file->addrs[i] != 0) {
    cur_block = file->addrs[NDIRECT];
    
    /*Check that the indirect addr is valid*/
    if(cur_block < db_start || cur_block > sb->size - 1) {
      free_mem();
      fprintf(stderr, "ERROR: bad indirect address in inode.\n");
      exit(1);
    }

    if(db_info[cur_block] == 't') {
      free_mem();
      fprintf(stderr, "ERROR: indirect address used more than once.\n");
      exit(1);
    }

    db_info[cur_block] = 't';

    /*Get the indirect addr data block.*/
    id_block = (uint*)(image_start + BSIZE*cur_block);
    
    /*Loop through all the addrs in the data block.*/
    //printf("checking indirect_block\n");
    for(int j = 0; j < APB; j++) {
      /*Check if there are more addrs in the indirect block*/
      if(id_block[j] == 0)
	continue;

      /*Check that the addr is valid.*/
      if(id_block[j] < db_start || id_block[j] > db_end) {
	free_mem();
	fprintf(stderr, "ERROR: bad indirect address in inode.\n");
	exit(1);
      }

      /*Check if the addr has already been referenced*/
      if(db_info[id_block[j]] == 't') {
	free_mem();
	fprintf(stderr, "ERROR: indirect address used more than once.\n");
	exit(1);
      }

      /*Update the global variable*/
      db_info[id_block[j]] = 't';
    }
  }
  return;
}

/*Check that the passed in directory is consistent*/
void
check_dir(struct dinode* dir, int inum) {
  
  //int d_num = 0;
  uint cur_block;
  int i = 0;
  uint* id_block;

  /*Loop through all the addrs and the corresponding dirents*/
  for(i = 0; i < NDIRECT; i++) {
    cur_block = dir->addrs[i];
    /*We've looped through all the data blocks*/
    if(cur_block == 0) 
      continue;

    /*Check for a valid data block addr*/
    if(cur_block < db_start || cur_block > db_end) {
      free_mem();
      fprintf(stderr, "ERROR: bad direct address in inode.\n");
      exit(1);
    }

    /*Check that the data block hasn't already been referenced.*/
    if(db_info[cur_block] == 't') {
      /*Addr was already referenced by another inode.*/
      free_mem();
      fprintf(stderr, "ERROR: direct address used more than once.\n");
      exit(1);
    }
    db_info[cur_block] = 't'; //Show that the addr has been referenced.

    /*Send loop_dirents the proper args, so it can check the proper things*/
    if(i != 0) 
      loop_dirents(cur_block, 'n', inum);
    else if( i == 0)
      loop_dirents(cur_block, 1, inum);
  }

  /*Check if the directory has an indirect block*/
  if(i == NDIRECT && dir->addrs[i] != 0) {
    cur_block = dir->addrs[i];
    if(cur_block < db_start || cur_block > db_end) {
      free_mem();
      fprintf(stderr, "ERROR: bad indirect address in inode.\n");
      exit(1);
    }
    
    /*Check that the data block hasn't already been referenced.*/
    if(db_info[cur_block] == 't') {
      /*Addr was already referenced by another inode.*/
      free_mem();
      fprintf(stderr, "ERROR: indirect address used more than once.\n");
      exit(1);
    }
    
    db_info[cur_block] = 't'; //Show that the addr has been referenced.
    
    /*Get the indirect block.*/
    id_block = (uint*)(image_start + BSIZE*cur_block);

    /*Loop through all the data block addrs in the indirect block.*/
    for(int j = 0; j < APB; j++) {
      /*Been through all the addrs*/
      if(id_block[j] == 0)
	continue;
      
      /*Check that the indirect addr is valid*/
      if(id_block[j] < db_start || id_block[j] > db_end) {
	free_mem();
	fprintf(stderr, "ERROR: bad indirect address in inode.\n");
	exit(1);
      }
      
      /*Check that the direct addr isn't already used*/
      if(db_info[id_block[j]] == 't') {
	free_mem();
	fprintf(stderr, "ERROR: indirect address used more than once.\n");
	exit(1);
      }
      db_info[id_block[j]] = 't';
      
      loop_dirents(id_block[j], 'n', inum);
    }
  }
  return;
}

static void
free_mem() {
  free(inodes_dirs);
  free(inodes_files);
  free(db_info);
  munmap(image_start, BSIZE*sb->size);
  close(fd);
  return;
}

static void
loop_dirents(uint cur_block, int fb, int inum) {
  static int turn = 0;
  struct dinode* cur_inode;
  struct dirent* cur_dirent;
  int j = 0;

  /*Get the data block*/
  cur_dirent = (struct dirent*)(image_start + BSIZE*cur_block);
  //printf("inum%d seen\n", cur_dirent[j].inum);
  /*Check that the dir has . and .. entries and that the . points to itself*/
  if(fb == 1) {
    if(strcmp(cur_dirent[j].name, ".") != 0 || cur_dirent[j].inum != inum) {
      free_mem();
      fprintf(stderr, "ERROR: directory not properly formatted.\n");
      exit(1);
    }
    
    j++;
    //printf("inum%d seen\n", cur_dirent[j].inum);
    if(inum != ROOTINO) {
      if(strcmp(cur_dirent[j].name, "..") != 0) {
	free_mem();
	fprintf(stderr, "ERROR: directory not properly formatted.\n");
	exit(1);
      }
      
    }
    else if(inum == ROOTINO) {
      if(strcmp(cur_dirent[j].name, "..") != 0 || cur_dirent[j].inum != inum) {
	free_mem();
	fprintf(stderr, "ERROR: root directory does not exist.\n");
	exit(1);
      }
    }
    j++;
  }
    
  /*Loop through all the dirents in the data block.*/
  for(; j < DPB; j++) {
    //printf("inum%d seen\n", cur_dirent[j].inum);
    /*Check if we're done with all the dirents*/
    if((cur_dirent[j]).inum == 0)
      continue;
    
    /*Get the inode.*/
    cur_inode = (struct dinode*)(image_start + BSIZE*2
				 + sizeof(struct dinode)*(cur_dirent[j]).inum);

    /*Check the inode reference for inconsistencies.*/
    if(cur_inode->type == T_DIR) {
      /*Check if the directory has already been referenced.*/
      if(cur_dirent[j].inum != ROOTINO && inodes_dirs[(cur_dirent[j]).inum].in_use == 1) {
	free_mem();
	fprintf(stderr, "ERROR: directory appears more than once in file system.\n");
	exit(1);
      }
      /*Add the directory reference to the global variable.*/
      inodes_dirs[(cur_dirent[j]).inum].in_use = 1;
      inodes_dirs[(cur_dirent[j]).inum].type = 'd';
      (inodes_dirs[(cur_dirent[j]).inum].ref_count)++;
    }
    /*Add file reference to the global variable.*/
    else if(cur_inode->type == T_FILE) {
      inodes_dirs[(cur_dirent[j]).inum].in_use = 1;
      inodes_dirs[(cur_dirent[j]).inum].type = 'f';
      (inodes_dirs[(cur_dirent[j]).inum].ref_count)++;
    }
    
    /*Check if the pointer to inode is marked in use.*/
    else if(cur_inode->type == 0) {
      free_mem();
      fprintf(stderr, "ERROR: inode referred to in directory but marked free.\n");
      exit(1);
    }
  }
  turn++;
  return;
}
