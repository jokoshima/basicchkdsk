#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <math.h>

#include "bootsect.h"
#include "bpb.h"
#include "direntry.h"
#include "fat.h"
#include "dos.h"


// TAKEN FROM DOS_LS.C. COPYRIGHT JOEL SOMMERS

void print_indent(int indent)
{
    int i;
    for (i = 0; i < indent*4; i++)
	printf(" ");
}

void update_bitmap(uint16_t cluster, uint8_t *clust_bitmap)
{
    clust_bitmap[cluster/8] |= (1 << (7 - (cluster % 8)));
}

uint16_t is_data_cluster(uint16_t cluster, uint8_t *clust_bitmap){
    return clust_bitmap[cluster/8] & (1 << (7 - (cluster % 8)));
}

int is_taken_cluster(uint16_t cluster, struct bpb33 *bpb, uint8_t *image_buf)
{
    uint16_t max_cluster = (bpb->bpbSectors / bpb->bpbSecPerClust) & FAT12_MASK;
    return  cluster >= (FAT12_MASK & CLUST_FIRST) && 
            cluster <= (FAT12_MASK & CLUST_LAST) &&
            cluster < max_cluster &&
            get_fat_entry(cluster, image_buf, bpb) != (CLUST_FREE & FAT12_MASK) && 
            get_fat_entry(cluster, image_buf, bpb) != (CLUST_BAD & FAT12_MASK);

}

uint16_t analyze_dirent(struct direntry *dirent, int indent, uint8_t *image_buf, struct bpb33* bpb, uint8_t *clust_bitmap)
{
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
    if (name[0] == SLOT_EMPTY){
	    return followclust;
    }

    /* skip over deleted entries */
    if (((uint8_t)name[0]) == SLOT_DELETED){
	    return followclust;
    }

    if (((uint8_t)name[0]) == 0x2E){
	    // dot entry ("." or "..")
	    // skip it
        return followclust;
    }

    /* names are space padded - remove the spaces */
    for (i = 8; i > 0; i--) {
	    if (name[i] == ' ') 
	        name[i] = '\0';
	    else 
	        break;
    }

    /* remove the spaces from extensions */
    for (i = 3; i > 0; i--) {
	    if (extension[i] == ' ') 
	        extension[i] = '\0';
	    else 
	        break;
    }

    if ((dirent->deAttributes & ATTR_WIN95LFN) == ATTR_WIN95LFN){
	    // ignore any long file name extension entries
	    //
	    // printf("Win95 long-filename entry seq 0x%0x\n", dirent->deName[0]);
    }
    else if ((dirent->deAttributes & ATTR_VOLUME) != 0) {
	    printf("Volume: %s\n", name);
    } 
    else if ((dirent->deAttributes & ATTR_DIRECTORY) != 0) {
        // don't deal with hidden directories; MacOS makes these
        // for trash directories and such; just ignore them.
	    
        if ((dirent->deAttributes & ATTR_HIDDEN) != ATTR_HIDDEN){
	        print_indent(indent);
    	    printf("%s/ (directory)\n", name);
            file_cluster = getushort(dirent->deStartCluster);
            followclust = file_cluster;
            
            uint16_t cluster = file_cluster;
            while (!is_end_of_file(cluster)) {
                update_bitmap(cluster, clust_bitmap);
                cluster = get_fat_entry(cluster, image_buf, bpb);
            }
        }
    }
    else {
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

        int cl_count = 0; //#of clusters in a file
        int bytesPerClust = bpb->bpbBytesPerSec * bpb->bpbSecPerClust; // # of bytes in a cluster (512)
        uint16_t cluster = getushort(dirent->deStartCluster); // might need uint16_t type!
        int size_sig = size; // can't do arithmetic w/ unsigned

        while (!is_end_of_file(cluster)) {
            update_bitmap(cluster, clust_bitmap);
            cluster = get_fat_entry(cluster, image_buf, bpb);
            cl_count++;
        }
        int cl_size = cl_count * bytesPerClust; // bytes in file
        //updated FAT, but didnt write the data
        if (cl_size - size >= bytesPerClust) { //badimage1
            //missing a block
            printf("missing block: starting cluster is %d\n", getushort(dirent->deStartCluster));
        }
        //wrote the data but didnt update the fat
        if ((cl_size - size_sig) < 0) { //badimage2
            //excessive blocks
            printf("excessive blocks: starting cluster is %d\n", getushort(dirent->deStartCluster));
        }

    }

    return followclust;
}


void follow_dir(uint16_t cluster, int indent,
		uint8_t *image_buf, struct bpb33* bpb, uint8_t *clust_bitmap)
{
    while (is_valid_cluster(cluster, bpb))
    {
        struct direntry *dirent = (struct direntry*)cluster_to_addr(cluster, image_buf, bpb);

        int numDirEntries = (bpb->bpbBytesPerSec * bpb->bpbSecPerClust) / sizeof(struct direntry);
        int i = 0;
	for ( ; i < numDirEntries; i++)
	{
            
            uint16_t followclust = analyze_dirent(dirent, indent, image_buf, bpb, clust_bitmap);
            if (followclust)
                follow_dir(followclust, indent+1, image_buf, bpb, clust_bitmap);
            dirent++;
	}

	cluster = get_fat_entry(cluster, image_buf, bpb);
    }
}


void traverse_root(uint8_t *image_buf, struct bpb33* bpb, uint8_t *clust_bitmap)
{
    uint16_t cluster = 0;

    struct direntry *dirent = (struct direntry*)cluster_to_addr(cluster, image_buf, bpb);

    int i = 0;
    for ( ; i < bpb->bpbRootDirEnts; i++)
    {
        uint16_t followclust = analyze_dirent(dirent, 0, image_buf, bpb, clust_bitmap);
        if (is_valid_cluster(followclust, bpb))
            follow_dir(followclust, 1, image_buf, bpb, clust_bitmap);

        dirent++;
    }
}

// END TAKEN FUNCTIONS

void cluster_scan(uint8_t *image_buf, struct bpb33* bpb, uint8_t *clust_bitmap)
{
    uint16_t totalClusters = CLUST_LAST & FAT12_MASK;
    for (int i=CLUST_FIRST; i<totalClusters; i++){
        if (is_taken_cluster((uint16_t)i, bpb, image_buf) && !is_data_cluster((uint16_t)i, clust_bitmap)){
            printf("Orphan cluster: %d\n", i);
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
    
    uint16_t totalClusters = CLUST_LAST & FAT12_MASK;
    // +1 in case there is a partial byte in the bitmap for the last clusters
    uint8_t clust_bitmap[totalClusters/8 + 1];
    memset(clust_bitmap, 0, totalClusters/8 + 1);
    // Unlike dos_ls.c, we're traversing the root to analyze directories here
    // and filling in the bitmap with referenced clusters
    traverse_root(image_buf, bpb, clust_bitmap);
    

    cluster_scan(image_buf, bpb, clust_bitmap);




    unmmap_file(image_buf, &fd);
    return 0;
}
