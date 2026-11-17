#include <stdio.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <string.h>
#include <unistd.h>

#define NUM_PAGES 256            // 2^16 / 2^8 = 2^8 virtual pages
#define PAGE_SIZE 256            // 2^8 bytes
#define NUM_FRAMES 128           // 2^15 / 2^8 = 2^7 actual frames
#define FRAME_SIZE 256           // 2^8 bytes
#define BACKINGSTORE_SIZE 65536  // 2^16 bytes
#define PHYSICALRAM_SIZE 32768   // 2^15 bytes
#define TLB_SIZE 16              // 2^4 bytes

signed char physicalram[PHYSICALRAM_SIZE]; // declare global simulated ram

// each entry in the TLB connects a page to a frame
typedef struct {
    int page;
    int frame;
} TLBentry;

TLBentry tlb[TLB_SIZE];
int next_free_tlbentry = 0;

//we define the three TLB operation functions as given in part 2

int search_TLB(int pagenum) {
    // return frame if the pagenumber location is stored in tlb. Otherwise -1
    for (int i = 0; i < TLB_SIZE; i++) {
        if (tlb[i].page == pagenum) {
            return tlb[i].frame;
        }
    }
    return -1;
}

void TLB_Add(int pagenum, int framenum) {
    tlb[next_free_tlbentry].page = pagenum;
    tlb[next_free_tlbentry].frame = framenum;
    next_free_tlbentry = (next_free_tlbentry + 1) % TLB_SIZE;
    return;
}

void TLB_Update(int evicted_p, int new_p, int framenum) {
    for (int i = 0; i < TLB_SIZE; i++) {
        if (tlb[i].page == evicted_p) {
            tlb[i].page = new_p;
            tlb[i].frame = framenum;
            return;
        }
    }
    TLB_Add(new_p, framenum);
    return;
}

int main(int argc, char *argv[]) {

    // initialize tlb to e,pty so we dont have 0->0
    for (int i = 0; i < TLB_SIZE; i++) {
        tlb[i].page = -1;
        tlb[i].frame = -1;
    }

    // pass filename as argument or just default
    const char *addr_file = (argc > 1) ? argv[1] : "addresses.txt";
    const char *backing_file = (argc > 2) ? argv[2] : "BACKING_STORE.bin";

    //open addresses.txt as a filepointer
    FILE *fp = fopen(addr_file, "r");
    if (fp == NULL) {
        perror("Error while trying to open addresses.txt");
        return 1;
    }

    // initialise pagetable, set each page to -1, i.e., no assigned frame
    int pagetable[NUM_PAGES];
    for (int i = 0; i < NUM_PAGES; i++) {
        pagetable[i] = -1;
    }

    // initialize holders of each logical address (from addresses.txt)
    // as well as the pagenum and offset logical address is split into
    int logicaladdress, pagenum, offset;

    // keep track of the circular pointer for frames, and fault count
    int next_free_frame = 0;
    int fault_count = 0;

    // track number of addresses and TLB hits to check for printing
    int totaladdresses = 0;
    int TLBhits = 0;

    // load backing store as a read only file, then store all 2^16 bytes contiguously memory 
    int readbacking = open(backing_file, O_RDONLY);
    if (readbacking < 0) {
        perror("error opening backing store");
        fclose(fp);
        return 1;
    }

    // note on flags: first flag: we dont care where this block is stored
    // second flag: our block will be 2^16 bytes, third flag: we can only read from this block
    // fourth flag: store in private copy, i.e., any changes made dont affect original bin file,
    // fifth flag: fd we read from, sixth flag: offset to start reading; 0, read the whole file
    signed char *backingstore = mmap(0, BACKINGSTORE_SIZE, PROT_READ, MAP_PRIVATE, readbacking, 0);
    if (backingstore == MAP_FAILED) {
        perror("error mapping backing store to memory");
        close(readbacking);
        fclose(fp);
        return 1;
    }

    // iterate address.txt till eof
    while (fscanf(fp, "%d", &logicaladdress) == 1) {

        // track total
        totaladdresses++;

        // extract pagenum (which page we are checking)
        // and offset, i.e., how far into the page
        offset = logicaladdress & 0xFF;
        pagenum = (logicaladdress >> 8) & 0xFF;

        // first check TLB for framenum
        int framenum = search_TLB(pagenum);

        if (framenum != -1) {
            TLBhits++;
        } else {
            // check where the currently checked page is being stored
            // either it is pointing to the physical location it is store
            // (0 <= framenum <= 127) or framenum = -1, i.e., page fault
            // (not stored in physical ram, we need to add it)
            framenum = pagetable[pagenum];
            if (framenum != -1) {
                TLB_Add(pagenum, framenum);
            }
        }

        // page fault after checking both tlb and directly
        if (framenum == -1) {

            // page fault; increment fault counter, and get the next available frame
            // then use modulo to find next free frame
            fault_count++;
            framenum = next_free_frame;
            next_free_frame = (next_free_frame + 1) % NUM_FRAMES;

            // we want to give physical framenum to page #pagenum, so tell any page
            // that may be stored at that physical frame that it has no home now
            // (we only go until we find the respective pagetable if it exists so we 
            // can break right away if it is found - only one page can belong to a frame)
            // now with TLB, we want to store this evicted page temporarily, to evict if in TLB
            int to_evict = -2;
            for (int i = 0; i < NUM_PAGES; i++) {
                if (pagetable[i] == framenum) {
                    to_evict = i;
                    pagetable[i] = -1;
                    break;
                }
            }

            // evict the page if needed, then add the entry to tlb
            TLB_Update(to_evict, pagenum, framenum);

            // give this now freed frame to this pagenum
            pagetable[pagenum] = framenum;

            // load from backing store into physical ram here
            // we are reading by page, so copy PAGE_SIZE bytes
            // we go page by page till we reach the page to copy from
            // in backingstore, then frame by frame to the place
            // in ram to paste into. The length we paste is the page size
            memcpy(physicalram + (framenum * FRAME_SIZE), backingstore + (pagenum * PAGE_SIZE), PAGE_SIZE);
        }

        // page fault or not, we now get exact physical address
        // simulated ram is a 2^15 byte array, first, the first 
        // 7 bits will be the frame, leave 8 bytes for the physical offset
        // in the frame. our framenum is 0<=x<=127, multiply by 258 (<< 8)
        int physicaladdress = framenum << 8;
        // then the offset is the same, since frame size = page size (naturally)
        // fill in bottom 8 bytes with offset, bottom 8 are 00000000, | works
        physicaladdress |= offset;

        // get the value we just copied, output all the details
        signed char val = physicalram[physicaladdress];
        printf("Virtual address: %d Physical address = %d Value=%d\n", logicaladdress, physicaladdress, val);
    }

    // printing remainigng statistics as designated in example output
    printf("Total addresses = %d\n", totaladdresses);
    printf("Page_faults = %d\n", fault_count);
    printf("TLB Hits = %d\n", TLBhits);

    // cleanup
    munmap(backingstore, BACKINGSTORE_SIZE);
    close(readbacking);
    fclose(fp);
    return 0;
}