#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <ctype.h>

#include "bootsect.h"
#include "bpb.h"
#include "direntry.h"
#include "fat.h"
#include "dos.h"



/* write the values into a directory entry */
void write_dirent(struct direntry *dirent, char *filename, 
		  uint16_t start_cluster, uint32_t size)
{
    char *p, *p2;
    char *uppername;
    int len, i;

    /* clean out anything old that used to be here */
    memset(dirent, 0, sizeof(struct direntry));

    /* extract just the filename part */
    uppername = strdup(filename);
    p2 = uppername;
    for (i = 0; i < strlen(filename); i++) 
    {
		if (p2[i] == '/' || p2[i] == '\\') {
	    uppername = p2+i+1;
		}
    }

    /* convert filename to upper case */
    for (i = 0; i < strlen(uppername); i++) 
    {
	uppername[i] = toupper(uppername[i]);
    }

    /* set the file name and extension */
    memset(dirent->deName, ' ', 8);
    p = strchr(uppername, '.');
    memcpy(dirent->deExtension, "___", 3);
    if (p == NULL) 
    {
		fprintf(stderr, "No filename extension given - defaulting to .___\n");
    }
    else 
    {
		*p = '\0';
		p++;
		len = strlen(p);
		if (len > 3) len = 3;
		memcpy(dirent->deExtension, p, len);
    }

    if (strlen(uppername)>8) 
    {
	uppername[8]='\0';
    }
    memcpy(dirent->deName, uppername, strlen(uppername));
    free(p2);

    /* set the attributes and file size */
    dirent->deAttributes = ATTR_NORMAL;
    putushort(dirent->deStartCluster, start_cluster);
    putulong(dirent->deFileSize, size);

    /* could also set time and date here if we really
       cared... */
}

/* create_dirent finds a free slot in the directory, and write the directory entry */

void create_dirent(struct direntry *dirent, char *filename, 
		   uint16_t start_cluster, uint32_t size,
		   uint8_t *image_buf, struct bpb33* bpb)
{
    while (1) 
    {
	if (dirent->deName[0] == SLOT_EMPTY) 
	{
	    /* we found an empty slot at the end of the directory */
	    write_dirent(dirent, filename, start_cluster, size);
	    dirent++;

	    /* make sure the next dirent is set to be empty, just in
	       case it wasn't before */
	    memset((uint8_t*)dirent, 0, sizeof(struct direntry));
	    dirent->deName[0] = SLOT_EMPTY;
	    return;
	}

	if (dirent->deName[0] == SLOT_DELETED) 
	{
	    /* we found a deleted entry - we can just overwrite it */
	    write_dirent(dirent, filename, start_cluster, size);
	    return;
	}
	dirent++;
    }
}

void print_indent(int indent)
{
    int i;
    for (i = 0; i < indent*4; i++)
	printf(" ");
}

// find inconsistent sizes 
uint16_t print_dirent(struct direntry *dirent, int indent, uint8_t *image_buf, struct bpb33 *bpb, int *clust_ref) {

    uint16_t followclust = 0;

    int i;
    char name[9];
    char extension[4];
    uint32_t size;
    uint16_t file_cluster;
    name[8] = ' ';
    extension[3] = ' ';
    memcpy(name, &(dirent->deName[0]), 8);
    memcpy(extension, dirent->deExtension, 3);
    if (name[0] == SLOT_EMPTY)
    {
	return followclust;
    }

    /* skip over deleted entries */
    if (((uint8_t)name[0]) == SLOT_DELETED)
    {
	return followclust;
    }

    if (((uint8_t)name[0]) == 0x2E)
    {
	// dot entry ("." or "..")
	// skip it
        return followclust;
    }

    /* names are space padded - remove the spaces */
    for (i = 8; i > 0; i--) 
    {
	if (name[i] == ' ') 
	    name[i] = '\0';
	else 
	    break;
    }

    /* remove the spaces from extensions */
    for (i = 3; i > 0; i--) 
    {
	if (extension[i] == ' ') 
	    extension[i] = '\0';
	else 
	    break;
    }

    if ((dirent->deAttributes & ATTR_WIN95LFN) == ATTR_WIN95LFN)
    {
	// ignore any long file name extension entries
	//
	// printf("Win95 long-filename entry seq 0x%0x\n", dirent->deName[0]);
    }
    else if ((dirent->deAttributes & ATTR_VOLUME) != 0) 
    {
	printf("Volume: %s\n", name);
    } 
    else if ((dirent->deAttributes & ATTR_DIRECTORY) != 0) 
    {
        // don't deal with hidden directories; MacOS makes these
        // for trash directories and such; just ignore them.
	if ((dirent->deAttributes & ATTR_HIDDEN) != ATTR_HIDDEN)
        {
	    print_indent(indent);
    	    printf("%s/ (directory)\n", name);
            file_cluster = getushort(dirent->deStartCluster);
            followclust = file_cluster;
        }
    }
    else 
    {
        /*
         * a "regular" file entry
         * print attributes, size, starting cluster, etc.
         */
	int ro = (dirent->deAttributes & ATTR_READONLY) == ATTR_READONLY;
	int hidden = (dirent->deAttributes & ATTR_HIDDEN) == ATTR_HIDDEN;
	int sys = (dirent->deAttributes & ATTR_SYSTEM) == ATTR_SYSTEM;
	int arch = (dirent->deAttributes & ATTR_ARCHIVE) == ATTR_ARCHIVE;

	size = getulong(dirent->deFileSize);
	print_indent(indent);
	printf("%s.%s (%u bytes) (starting cluster %d) %c%c%c%c\n", 
	       name, extension, size, getushort(dirent->deStartCluster),
	       ro?'r':' ', 
               hidden?'h':' ', 
               sys?'s':' ', 
               arch?'a':' ');

	int chain_count = 0;			// cluster chain count in FAT
	uint16_t cluster = getushort(dirent->deStartCluster);
	uint16_t first = cluster;
	uint16_t prev; 

	// go through cluster chain to detect any "bad" clusters 
	while(is_valid_cluster(cluster, bpb)) {
		clust_ref[cluster]++;
		if (clust_ref[cluster] > 1) {   // multiple refs to same cluster
			dirent->deName[0] = SLOT_DELETED;
			clust_ref[cluster]--;
		}
		prev = cluster;
		cluster = get_fat_entry(cluster, image_buf, bpb);

		if (cluster == (FAT12_MASK & CLUST_BAD)) {  // found bad cluster
			set_fat_entry(cluster,FAT12_MASK & CLUST_FREE, image_buf, bpb);
			set_fat_entry(prev,FAT12_MASK & CLUST_EOFS, image_buf,bpb);
			chain_count++;
			break;
		}

		chain_count++;
   	 }
		int meta_clusters = 0;		// metadata # clusters
		uint32_t newsize = 0;

		if (size%512 == 0) {
		 	meta_clusters = size/512;
		}
		else {
			meta_clusters = (size/512) + 1;
		}

		if (meta_clusters < chain_count) {  // meta file size < cluster length in FAT
			cluster = get_fat_entry(first + meta_clusters -1, image_buf, bpb);
	
			while (is_valid_cluster(cluster,bpb)) {
				prev = cluster;
				set_fat_entry(prev, FAT12_MASK & CLUST_FREE, image_buf, bpb);
				cluster = get_fat_entry(cluster,image_buf, bpb);
			}
			set_fat_entry(first + meta_clusters - 1, FAT12_MASK & CLUST_EOFS, image_buf, bpb); 

			printf("New file size: %d\n", newsize);
			printf("Old file size: %d\n", size);
		}
		else if (meta_clusters < chain_count) {  // meta file size > # clusters in FAT
			newsize = chain_count * bpb->bpbBytesPerSec;
			putulong(dirent->deFileSize, newsize);
		}

	}
    return followclust;
}



uint16_t get_dirent(struct direntry *dirent, char *buffer) {
    uint16_t followclust = 0;
    memset(buffer, 0, MAXFILENAME);

    int i;
    char name[9];
    char extension[4];
    uint16_t file_cluster;
    name[8] = ' ';
    extension[3] = ' ';
    memcpy(name, &(dirent->deName[0]), 8);
    memcpy(extension, dirent->deExtension, 3);
    if (name[0] == SLOT_EMPTY) return followclust;

    /* skip over deleted entries */
    if (((uint8_t)name[0]) == SLOT_DELETED) return followclust;

    // dot entry ("." or "..")
    // skip it
    if (((uint8_t)name[0]) == 0x2E) return followclust;

    /* names are space padded - remove the spaces */
    for (i = 8; i > 0; i--) {
        if (name[i] == ' ') name[i] = '\0';
        else break;
    }

    /* remove the spaces from extensions */
    for (i = 3; i > 0; i--) {
        if (extension[i] == ' ') extension[i] = '\0';
        else break;
    }

    if ((dirent->deAttributes & ATTR_WIN95LFN) == ATTR_WIN95LFN) {
        // ignore any long file name extension entries
        //
        // printf("Win95 long-filename entry seq 0x%0x\n", 
        //          dirent->deName[0]);
    } else if ((dirent->deAttributes & ATTR_DIRECTORY) != 0) {
        // don't deal with hidden directories; MacOS makes these
        // for trash directories and such; just ignore them.
        if ((dirent->deAttributes & ATTR_HIDDEN) != ATTR_HIDDEN) {
            strcpy(buffer, name);
            file_cluster = getushort(dirent->deStartCluster);
            followclust = file_cluster;
        }
    } else {
        /*
         * a "regular" file entry
         * print attributes, size, starting cluster, etc.
         */
        strcpy(buffer, name);
        if (strlen(extension))  {
            strcat(buffer, ".");
            strcat(buffer, extension);
        }
    }

    return followclust;
}

// scan through file hierarchy and check for consistencies in every dir entry
void follow_dir(uint16_t cluster, int indent,
		uint8_t *image_buf, struct bpb33* bpb,int *clust_ref)
{
    while (is_valid_cluster(cluster, bpb))
    {
        struct direntry *dirent = (struct direntry*)cluster_to_addr(cluster, image_buf, bpb);

        int numDirEntries = (bpb->bpbBytesPerSec * bpb->bpbSecPerClust) / sizeof(struct direntry);
	   	char buff[MAXFILENAME];

	for (int i = 0; i < numDirEntries; i++)
	{
       uint16_t followclust = get_dirent(dirent,buff);
            if (followclust){
                follow_dir(followclust, indent+1, image_buf, bpb, clust_ref);
			}
			dirent++;
	}

	cluster = get_fat_entry(cluster, image_buf, bpb);
    }
}

void traverse_root(uint8_t *image_buf, struct bpb33* bpb,int *clust_ref)
{
    uint16_t cluster = 0;
	char buff [MAXFILENAME];

    struct direntry *dirent = (struct direntry*)cluster_to_addr(cluster, image_buf, bpb);

	for (int i = 0 ; i < bpb->bpbRootDirEnts; i++) {
		uint16_t followclust = get_dirent(dirent, buff);
		
	if (is_valid_cluster(followclust, bpb)) {  
			clust_ref[followclust]++;	
			follow_dir(followclust,1,image_buf,bpb,clust_ref);
		}
		dirent++;
	}

 
}

// goes through cluster list and saves the # of orphans in memory 
void store_orphans(uint8_t *image_buf, struct bpb33* bpb,int *clust_ref) {
    int num_orphans = 0;

    for (int i = 0; i < bpb->bpbSectors; i++) {
		uint16_t cluster = get_fat_entry(i, image_buf, bpb); 
	
		if (clust_ref[i] == 0 && cluster != (FAT12_MASK & CLUST_FREE) && cluster != (FAT12_MASK & CLUST_BAD)) {
			num_orphans++;
			int file_size = bpb->bpbBytesPerSec;
			clust_ref[i] = 1;
	
			uint16_t store = cluster; 
			while (is_valid_cluster(store, bpb)) {
				
				if (clust_ref[store] > 1) {  // more than 1 ref to an orphan 
					struct direntry *dirent = (struct direntry*)cluster_to_addr(store, image_buf, bpb); 
					dirent->deName[0] = SLOT_DELETED; 
					clust_ref[store]--;
				}
				else if (clust_ref[store] == 0) {		
					clust_ref[store]++;
				}	
				else if (clust_ref[store] == 1) {			
					set_fat_entry(store, (FAT12_MASK & CLUST_EOFS), image_buf, bpb);
				}
			
				file_size += bpb->bpbBytesPerSec;	//adjust file size entry 
				store = get_fat_entry(store, image_buf, bpb);
			}

		printf("File size: %d\n",file_size);

		
		char filename[1024] = "";
		strcat(filename, "found");
		char arr[10];
		strcat(filename, arr);
		strcat(filename, ".dat");
		char *f = filename; 
		
		struct direntry *dir = (struct direntry*)root_dir_addr(image_buf,bpb);
		create_dirent(dir, f, i, file_size, image_buf, bpb); //create directory entries for any unreferenced file data 
		
		}
	}	
	
}


void usage(char *progname) {
    fprintf(stderr, "usage: %s <imagename>\n", progname);
    exit(1);
}

int main(int argc, char** argv) {
    uint8_t *image_buf;
    int fd;
    struct bpb33* bpb;
    if (argc < 2) {
        usage(argv[0]);
    }

	image_buf = mmap_file(argv[1], &fd);
    bpb = check_bootsector(image_buf);

	int *clust_ref = malloc(sizeof(int) * bpb->bpbSectors);
	for (int i=0; i <bpb->bpbSectors;i++) {				//init array of clusters
			clust_ref[i]=0;
	}


    traverse_root(image_buf, bpb,clust_ref);
    store_orphans(image_buf, bpb,clust_ref);
	

    free(bpb);
    unmmap_file(image_buf, &fd);
	free(clust_ref);
    return 0;
}



