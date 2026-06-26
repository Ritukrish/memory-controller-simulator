#include<stdio.h>
#include<stdlib.h>
#include<string.h>


#define MAX_REQS 1000

//config file 

typedef struct{
    int tRC;
    int tHit;
    int num_banks;
    int queue_size;
    int row_bits;
    int bank_bits;
    int col_bits;

    int enable_log;
}  Config;

// Request queue
typedef struct{
    int cycle ;
    int op;
    unsigned int addr;
    int data;
    int id;
    int row;
    int bank;
    int col;
    int issued;
    int completion_cycle;

} Request;

// status of bank in memory controller
typedef struct 
{
    int has_open_row;
    unsigned int open_row;
    int busy_until;
    int active_cycle;
   
} BankStatus;

//storage data and address
typedef struct 
{
    unsigned int addr;
    int data;

} StorageEntry;

StorageEntry memory_storage [MAX_REQS];
int storage_count =0 ;

void write_to_storage(unsigned int addr, int data){
    for (int i=0; i<storage_count; i++){
        if ( memory_storage[i].addr == addr){
            memory_storage[i].data =    data;
            return;
        }
    }
    memory_storage[storage_count].addr = addr;
    memory_storage[storage_count].data = data;
    storage_count++;
}
