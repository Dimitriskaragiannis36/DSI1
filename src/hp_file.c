#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bf.h"
#include "hp_file.h"
#include "record.h"

// Υπολογισμός του μέγιστου αριθμού εγγραφών ανά block (αφαιρώντας τον χώρο των μεταδεδομένων του block)
#define MAX_RECORDS_PER_BLOCK ((BF_BLOCK_SIZE - sizeof(HP_block_info)) / sizeof(Record))

// Μακροεντολή για έλεγχο σφαλμάτων BF. Επιστρέφει -1 σε περίπτωση λάθους σύμφωνα με την εκφώνηση.
#define CALL_BF(call)                \
{                                    \
  BF_ErrorCode code = call;          \
  if (code != BF_OK) {               \
    BF_PrintError(code);             \
    return -1;                       \
  }                                  \
}

/* Η συνάρτηση HP_CreateFile χρησιμοποιείται για τη δημιουργία και
κατάλληλη αρχικοποίηση ενός άδειου αρχείου σωρού με όνομα fileName. */
int HP_CreateFile(char *fileName) {
    CALL_BF(BF_CreateFile(fileName));
    int file_desc;
    CALL_BF(BF_OpenFile(fileName, &file_desc));

    // Δημιουργία του πρώτου block (block 0) για τα μεταδεδομένα του αρχείου
    BF_Block *block;
    BF_Block_Init(&block);
    CALL_BF(BF_AllocateBlock(file_desc, block));

    void *data = BF_Block_GetData(block);
    HP_info *hp_info = (HP_info*)data;

    // Αρχικοποίηση των μεταδεδομένων
    hp_info->lastBlockId = 0;  // Μέχρι στιγμής υπάρχει μόνο το block 0
    hp_info->recordsPerBlock = MAX_RECORDS_PER_BLOCK;

    BF_Block_SetDirty(block);
    CALL_BF(BF_UnpinBlock(block));
    BF_Block_Destroy(&block);
    
    CALL_BF(BF_CloseFile(file_desc));
    return 0;
}

/* Η συνάρτηση HP_OpenFile ανοίγει το αρχείο, διαβάζει τα μεταδεδομένα από 
το πρώτο μπλοκ και επιστρέφει έναν δείκτη σε αυτά. */
HP_info* HP_OpenFile(char *fileName, int *file_desc) {
    if (BF_OpenFile(fileName, file_desc) != BF_OK) {
        return NULL;
    }

    BF_Block *block;
    BF_Block_Init(&block);
    
    // Λήψη του block 0
    if (BF_GetBlock(*file_desc, 0, block) != BF_OK) {
        BF_Block_Destroy(&block);
        return NULL;
    }

    // Παίρνουμε τον δείκτη προς τη μνήμη του block
    void *data = BF_Block_GetData(block);
    HP_info *hp_info = (HP_info*)data;

    // ΠΡΟΣΟΧΗ: ΔΕΝ κάνουμε Unpin το block 0 εδώ! 
    // Το αφήνουμε καρφιτσωμένο (pinned) ώστε οι αλλαγές στα μεταδεδομένα 
    // (π.χ. lastBlockId) να γίνονται απευθείας στην ενδιάμεση μνήμη.
    
    // Καταστρέφουμε μόνο τον τοπικό δείκτη BF_Block, όχι το ίδιο το block.
    BF_Block_Destroy(&block);
    
    return hp_info;
}

/* Η συνάρτηση HP_CloseFile κλείνει το αρχείο. */
int HP_CloseFile(int file_desc, HP_info* header_info) {
    BF_Block *block;
    BF_Block_Init(&block);
    
    // Ξαναπαίρνουμε το block 0 για να μπορέσουμε να το κάνουμε unpin
    CALL_BF(BF_GetBlock(file_desc, 0, block));
    
    // Μαρκάρουμε ως dirty γιατί πιθανότατα το lastBlockId άλλαξε κατά τις εισαγωγές
    BF_Block_SetDirty(block); 
    
    // Κάνουμε Unpin 2 φορές: Μία για το GetBlock που μόλις κάναμε,
    // και μία για το GetBlock που είχε γίνει ανοιχτό στην HP_OpenFile.
    CALL_BF(BF_UnpinBlock(block));
    CALL_BF(BF_UnpinBlock(block));
    
    BF_Block_Destroy(&block);
    
    CALL_BF(BF_CloseFile(file_desc));
    return 0;
}

/* Η συνάρτηση HP_InsertEntry εισάγει μια νέα εγγραφή στο αρχείο. */
int HP_InsertEntry(int file_desc, HP_info* header_info, Record record) {
    BF_Block *block;
    BF_Block_Init(&block);

    int last_block = header_info->lastBlockId;
    void *block_data;
    HP_block_info *block_info;

    if (last_block == 0) {
        // Δεν υπάρχουν ακόμα data blocks, φτιάχνουμε το πρώτο (block 1)
        CALL_BF(BF_AllocateBlock(file_desc, block));
        last_block = 1;
        header_info->lastBlockId = last_block; // Ενημερώνεται απευθείας στη RAM!
        
        block_data = BF_Block_GetData(block);
        block_info = (HP_block_info*)block_data;
        block_info->recordCount = 0;
    } else {
        // Φέρνουμε το τελευταίο data block
        CALL_BF(BF_GetBlock(file_desc, last_block, block));
        block_data = BF_Block_GetData(block);
        block_info = (HP_block_info*)block_data;
    }

    // Έλεγχος αν το τρέχον block έχει γεμίσει
    if (block_info->recordCount >= header_info->recordsPerBlock) {
        // Αν είναι γεμάτο, το κάνουμε unpin...
        CALL_BF(BF_UnpinBlock(block));

        // ...και δεσμεύουμε ένα νέο block!
        CALL_BF(BF_AllocateBlock(file_desc, block));
        last_block++;
        header_info->lastBlockId = last_block;
        
        block_data = BF_Block_GetData(block);
        block_info = (HP_block_info*)block_data;
        block_info->recordCount = 0;
    }

    // Βρίσκουμε το σημείο που θα μπει η εγγραφή (ΜΕΤΑ το HP_block_info)
    Record *records = (Record*)(block_data + sizeof(HP_block_info));
    records[block_info->recordCount] = record;
    block_info->recordCount++;

    // Μαρκάρουμε το block ως dirty και το κάνουμε unpin
    BF_Block_SetDirty(block);
    CALL_BF(BF_UnpinBlock(block));
    BF_Block_Destroy(&block);

    return last_block;
}

/* Εκτύπωση όλων των εγγραφών με βάση το ID */
int HP_GetAllEntries(int file_desc, HP_info* header_info, int id) {
    BF_Block *block;
    BF_Block_Init(&block);
    int blocks_read = 0;

    // Ξεκινάμε από το 1, καθώς το block 0 έχει μόνο μεταδεδομένα!
    for (int i = 1; i <= header_info->lastBlockId; i++) {
        CALL_BF(BF_GetBlock(file_desc, i, block));
        blocks_read++;

        void *data = BF_Block_GetData(block);
        HP_block_info *block_info = (HP_block_info*)data;
        Record *records = (Record*)(data + sizeof(HP_block_info));

        // Ελέγχουμε τις εγγραφές του τρέχοντος block
        for (int j = 0; j < block_info->recordCount; j++) {
            if (records[j].id == id) {
                printRecord(records[j]);
            }
        }

        CALL_BF(BF_UnpinBlock(block));
    }

    BF_Block_Destroy(&block);
    return blocks_read;
}