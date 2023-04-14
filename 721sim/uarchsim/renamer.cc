#define __STDC_FORMAT_MACROS

#include "renamer.h"
#include <cassert>
#include <cmath>

renamer::renamer(uint64_t n_log_regs,
        uint64_t n_phys_regs,
        uint64_t n_chkpts,
        uint64_t n_active){
    
    //Run the assertions
    assert(n_phys_regs > n_log_regs);
    //assert((0 <= n_branches) && (n_branches <= 64)); //TODO: is there anything like that for CPR
    assert(n_active > 0);
  
    //initialize the data structures
    map_table_size = n_log_regs;
    num_phys_reg = n_phys_regs;
    rmt = new uint64_t[n_log_regs];
    prf = new uint64_t[n_phys_regs];
    prf_ready = new uint64_t[n_phys_regs];
    prf_usage_counter = new uint64_t[n_phys_regs];
    prf_unmapped = new uint64_t[n_phys_regs];

    uint64_t j;
    //AMT and RMT should have the same value at the beginning
    //However, not sure what would the content be. if amt[0] = 0, amt[1] = 0
    //then all the logical registers are mapped to p0. OTOH, amt[0] = 0 and
    //amt[1] = 1,..., amt[n] = n indicate r0->p0, r1->p1,..
    //the contents of the prf does not matter I suppose.
    for (j=0; j < n_log_regs; j++){
        rmt[n_log_regs - 1 - j] = j;
    }

    //initializing the physical register files to be consistent with
    //an empty pipeline state
    for (j=0; j < n_phys_regs; j++){
        prf_ready[j] = 1;
        if (reg_in_rmt(j)){
            prf_unmapped[j] = 0;
        } else {
            prf_unmapped[j] = 1;
        }
        prf_usage_counter[j] = 0;
    }


    //checkpoint buffer
    chkpt_buffer_head = 0;
    chkpt_buffer_tail = 0;
    chkpt_buffer_head_phase = 0;
    chkpt_buffer_tail_phase = 0;
    num_checkpoints = n_chkpts;  //CHANGE THIS TO COMMAND LINE ARG LATER

    checkpoint_buffer = new chkpt[num_checkpoints];
    uint64_t i;
    for (i = 0; i < num_checkpoints; i++){
        checkpoint_buffer[i].rmt = new uint64_t[n_log_regs];
        checkpoint_buffer[i].unmapped_bits = new uint64_t[n_phys_regs];
        checkpoint_buffer[i].load_counter = 0;
        checkpoint_buffer[i].store_counter = 0;
        checkpoint_buffer[i].branch_counter = 0;
        checkpoint_buffer[i].load_counter = 0;
        checkpoint_buffer[i].amo = false;
        checkpoint_buffer[i].csr = false;
        checkpoint_buffer[i].exception = false;
    }

    //populating the first checkpoint
    uint64_t k;
    for (k=0; k < n_log_regs; k++){
        checkpoint_buffer[chkpt_buffer_head].rmt[k] = rmt[k];
        //increameting the usage counter of the prf since we're checkpointing
        //the RMT at this location
        prf_usage_counter[rmt[k]] += 1;
    }

    for (j=0; j < n_phys_regs; j++){
        checkpoint_buffer[chkpt_buffer_head].unmapped_bits[j] = prf_unmapped[j];
    }

    //move the checkpoint tail ahead
    chkpt_buffer_tail++;

    //free list; free_list_size = prf - n_log_regs (721ss-prf-2 slide, p19)
    free_list_size = n_phys_regs - n_log_regs;
    fl.list = new uint64_t[free_list_size];
    fl.head = 0;
    fl.tail = 0;
    fl.head_phase = 0;
    fl.tail_phase = 1;
    assert(this->free_list_is_full()); //free list should be full at init

    //Free list contains registers that are not allocated or committed
    //i.e. registers that are not in AMT or RMT.
    for (i=0; i <free_list_size; i++){
        fl.list[i] = n_log_regs + i;
    }

}

bool renamer::stall_reg(uint64_t bundle_dst){
    uint64_t available_physical_regs = this->free_list_regs_available();
    if (available_physical_regs == UINT64_MAX) {
        printf("FATAL ERROR: free list is in incosistent state\n");
        exit(EXIT_FAILURE);
    }

    if (available_physical_regs < bundle_dst){
        return true;
    }

    return false;
}

bool renamer::checkpoint_buffer_is_empty(){
    if ((this->chkpt_buffer_head == this->chkpt_buffer_tail) && 
        (this->chkpt_buffer_head_phase == this->chkpt_buffer_tail_phase)){

        return true;
    }

    return false;
}

bool renamer::checkpoint_buffer_is_full(){
    if ((this->chkpt_buffer_head == this->chkpt_buffer_tail) && 
        (this->chkpt_buffer_head_phase != this->chkpt_buffer_tail_phase)){

        return true;
    }

    return false;
    
}

uint64_t renamer::get_free_checkpoint_count(){
    if (checkpoint_buffer_is_full()) return 0;
    if (checkpoint_buffer_is_empty()) return num_checkpoints;
     
    int used, free;
    if (this->chkpt_buffer_head_phase == this->chkpt_buffer_tail_phase){
        assert(this->chkpt_buffer_tail > this->chkpt_buffer_head);
        used = this->chkpt_buffer_tail - this->chkpt_buffer_head;
        free = this->num_checkpoints - used;
        return free;
    }

    else if (this->chkpt_buffer_head_phase != this->chkpt_buffer_tail_phase){
        assert(this->chkpt_buffer_head > this->chkpt_buffer_tail);
        free = this->chkpt_buffer_head - this->chkpt_buffer_tail; 
        return free;
    }

    // inconsistent state
    return -1; 
}

void renamer::reset_checkpoint(uint64_t i){
    for (uint64_t k=0; k < map_table_size; k++){
        checkpoint_buffer[i].rmt[k] = 0;
    }
    for (uint64_t k=0; k < num_phys_reg; k++){
        checkpoint_buffer[i].unmapped_bits[k] = 1;
    }

    checkpoint_buffer[i].load_counter = 0;
    checkpoint_buffer[i].store_counter = 0;
    checkpoint_buffer[i].branch_counter = 0;
    checkpoint_buffer[i].load_counter = 0;
    checkpoint_buffer[i].amo = false;
    checkpoint_buffer[i].csr = false;
    checkpoint_buffer[i].exception = false;

}

void renamer::free_checkpoint(){
    //clean out all the counters and flags
    reset_checkpoint(chkpt_buffer_head);
    //now move the head ahead
    chkpt_buffer_head++;

    if (chkpt_buffer_head == num_checkpoints){
        chkpt_buffer_head = 0;
        chkpt_buffer_head_phase = !chkpt_buffer_head_phase;
    }

}

bool renamer::stall_checkpoint(uint64_t bundle_chkpt){
    //Get the number of available checkpoints
    if (bundle_chkpt > get_free_checkpoint_count()){
        return true;
    }

    return false;
}

void renamer::set_exception(uint64_t checkpoint_ID){
    //WHAT??
    checkpoint_buffer[checkpoint_ID].exception = true;
}

uint64_t renamer::get_checkpoint_ID(bool load, bool store, bool branch, bool amo, bool csr){
    //return the nearest prior checkpoint
    uint64_t checkpoint_ID;
    if (chkpt_buffer_tail == 0){
        checkpoint_ID = num_checkpoints - 1;
    } else {
        checkpoint_ID = chkpt_buffer_tail - 1;
    }

    //increament other counters, set the flags etc
    if (load){checkpoint_buffer[checkpoint_ID].load_counter++;}
    if (store){checkpoint_buffer[checkpoint_ID].store_counter++;}
    if (branch){checkpoint_buffer[checkpoint_ID].branch_counter++;}

    
    checkpoint_buffer[checkpoint_ID].amo = amo;
    checkpoint_buffer[checkpoint_ID].csr = csr;

    //unconditionally increamenting
    checkpoint_buffer[checkpoint_ID].uncompleted_instruction_counter++;

    return checkpoint_ID;
}

uint64_t renamer::rename_rsrc(uint64_t log_reg){
    inc_usage_counter(rmt[log_reg]);
    return this->rmt[log_reg]; 
}

bool renamer::free_list_is_empty(){
    if ((this->fl.head == this->fl.tail) && 
        (this->fl.head_phase == this->fl.tail_phase)) return true;

    return false;
}

bool renamer::free_list_is_full(){
    if ((this->fl.head == this->fl.tail) && 
        (this->fl.head_phase != this->fl.tail_phase)) return true;

    return false;
}

void renamer::restore_free_list(){
    this->fl.head = this->fl.tail;
    this->fl.head_phase = !this->fl.tail_phase;
}

bool renamer::push_free_list(uint64_t phys_reg){
    //if it's full, you cannot push more into it
    if (this->free_list_is_full()){
        return false;
    }

    this->fl.list[this->fl.tail] = phys_reg;
    this->fl.tail++;
    if (this->fl.tail == this->free_list_size){
        this->fl.tail = 0;
        this->fl.tail_phase = !this->fl.tail_phase;
    }

    assert_free_list_invariance();
    return true; 
}

uint64_t renamer::pop_free_list(){
    if (this->free_list_is_empty()){
        return UINT64_MAX;
    }

    uint64_t result;
    result = this->fl.list[this->fl.head];

    //advance the head pointer of the free list
    this->fl.head++;
    if (this->fl.head == this->free_list_size){
        //wrap around
        this->fl.head = 0;
        this->fl.head_phase = !this->fl.head_phase;
    }

    assert_free_list_invariance();
    return result;
}

void renamer::assert_free_list_invariance(){
    if (this->fl.head_phase != this->fl.tail_phase){
        assert(!(this->fl.head < this->fl.tail));
    }
    if (this->fl.head_phase == this->fl.tail_phase){
        assert(!(this->fl.head > this->fl.tail));
    }
}

int renamer::free_list_regs_available(){
    if (this->free_list_is_full()) return this->free_list_size;
    if (this->free_list_is_empty()) return 0;
    
    uint64_t available = UINT64_MAX;
    if (this->fl.head_phase != this->fl.tail_phase){
        //otherwise inconsistent state: tail cannot be ahead of head,
        //means you are inserting entry when the list is already full
        assert(this->fl.head > this->fl.tail);
        available = this->fl.tail - this->fl.head + this->free_list_size;
        return available;
    }

    if (this->fl.head_phase == this->fl.tail_phase){
        //otherwise inconsistent state: head cant be ahead of tail, means
        // it allocated registers it does not have
        assert(this->fl.head < this->fl.tail);
        //available regsiters
        available = this->fl.tail - this->fl.head;
        return available;
    }


    return available; //it should never come here bc of the assertions 
}

uint64_t renamer::rename_rdst(uint64_t log_reg){
    uint64_t old = this->rmt[log_reg];
    uint64_t result = this->pop_free_list();

    if (result == UINT64_MAX){
        printf("FATAL ERROR: rename_rdst - not enough free list entry\n");
        exit(EXIT_FAILURE);
    } else {
        //update RMT //TODO: see if it's in AMT
        if (this->reg_in_rmt(result)){
            printf("%lu (popped from freelist) is already in RMT\n", result);
            exit(EXIT_FAILURE);
        }

        //if the popped reg from free list is not in RMT, assign it to RMT
        this->rmt[log_reg] = result; 
    }
   
    assert(old != this->rmt[log_reg]);
    this->map(result);
    this->unmap(old);

    inc_usage_counter(result);
    return result;
}


void renamer::checkpoint(){
    //TODO: reimplement for CPR
    //checkpoint the current RMT 
    //find the insertion point of the checkpoint buffer
    uint64_t x = chkpt_buffer_tail;
    uint64_t i;
    for(i = 0; i < map_table_size; i++){
        checkpoint_buffer[x].rmt[i] = rmt[i];
        inc_usage_counter(rmt[i]);
    }

    checkpoint_buffer[x].uncompleted_instruction_counter = 0;
    checkpoint_buffer[x].load_counter = 0;
    checkpoint_buffer[x].store_counter = 0;
    checkpoint_buffer[x].branch_counter = 0;
    checkpoint_buffer[x].amo = false;
    checkpoint_buffer[x].csr = false;
    checkpoint_buffer[x].exception = false;

    uint64_t j;
    for (j=0; j < num_phys_reg; j++){
        checkpoint_buffer[x].unmapped_bits[j] = prf_unmapped[j];
    }

    //advancing the tail 
    chkpt_buffer_tail++;
    if (chkpt_buffer_tail == num_checkpoints){
        chkpt_buffer_tail = 0;
        chkpt_buffer_tail_phase = !chkpt_buffer_tail_phase;
    }
}

void renamer::map(uint64_t phys_reg){
// Clear the unmapped bit of physical register
    prf_unmapped[phys_reg] = 0;
}

void renamer::unmap(uint64_t phys_reg){
// Set the unmapped bit of physical register
// Check if phys_reg’s usage counter is 0; if so,
// push phys_reg onto the Free List.
    prf_unmapped[phys_reg] = 1;
    if ((this->prf_unmapped[phys_reg] == 1) && (this->prf_usage_counter[phys_reg] == 0)){
        //push it onto the free list:
        push_free_list(phys_reg);
    }
    
}

void renamer::inc_usage_counter(uint64_t phys_reg){
    this->prf_usage_counter[phys_reg] += 1;
}

void renamer::dec_usage_counter(uint64_t phys_reg){
    bool __success = false;
    assert(this->prf_usage_counter[phys_reg] > 0);
    this->prf_usage_counter[phys_reg] -= 1;
    if ((this->prf_unmapped[phys_reg] == 1) && (this->prf_usage_counter[phys_reg] == 0)){
        //push it onto the free list:
        __success = push_free_list(phys_reg);
    }

    if (!__success){
        printf("Free list is full; failed to push reclaimed register onto the free list\n");
        exit(EXIT_FAILURE);
    }
}

bool renamer::is_ready(uint64_t phys_reg){
    return this->prf_ready[phys_reg];
}

void renamer::clear_ready(uint64_t phys_reg){
    this->prf_ready[phys_reg] = 0;
}

void renamer::set_ready(uint64_t phys_reg){
    this->prf_ready[phys_reg] = 1;
}

uint64_t renamer::read(uint64_t phys_reg){
    this->dec_usage_counter(phys_reg);
    return this->prf[phys_reg];
}

void renamer::write(uint64_t phys_reg, uint64_t value){
    this->dec_usage_counter(phys_reg);
    this->prf[phys_reg] = value; 
}

void renamer::set_complete(uint64_t checkpoint_ID){
    //TODO: reimplement for CPR
    checkpoint_buffer[checkpoint_ID].uncompleted_instruction_counter--;
    if (checkpoint_buffer[checkpoint_ID].uncompleted_instruction_counter < 0){
        printf("uncompleted IC went below 0. Should not happen\n");
        exit(EXIT_FAILURE);
    }
}


bool renamer::precommit(uint64_t &chkpt_id,
                        uint64_t &num_loads,
                        uint64_t &num_stores,
                        uint64_t &num_branches,
                        bool &amo, bool &csr, bool &exception
                        ){

    //check if there is at least one more checkpoint after the oldest one
    if (!checkpoint_buffer_is_empty() && 
        //see if the number of used checkpoint is at least 2
        ((num_checkpoints - get_free_checkpoint_count()) > 1)){
        
        if (checkpoint_buffer[chkpt_buffer_head].uncompleted_instruction_counter == 0){
            return true;
        }

    }

    return false;
}

void renamer::commit(uint64_t log_reg){
    //NOTE: double-check which RMT to use, might be a source of error
    uint64_t phys_reg = checkpoint_buffer[chkpt_buffer_head].rmt[log_reg];
    this->dec_usage_counter(phys_reg);

    prf_usage_counter[phys_reg] -= 1;
    if (prf_usage_counter[phys_reg] < 0) {
        printf("Usage counter for %d went negative. Should not happen\n", phys_reg);
        exit(EXIT_FAILURE);
    }
}


void renamer::generate_squash_mask_array(uint64_t *array, uint64_t rc){
    /*
    only set 1 for checkpoints that will be squashed based on the following
    criteria:
        1. it's younger than the rc(rollback checkpoint)
        2. do not include the rollback checkpoint
        3. the youngest checkpoint is 1 behind tail
    */
    for (unsigned long i = 0; i < num_checkpoints; i++){
        if (chkpt_buffer_head_phase == chkpt_buffer_tail_phase){
            if ((i > rc) && (i < chkpt_buffer_tail)){
                //set to 1
                array[i] = 1; 
            }
        }
        else {
            if ((i < chkpt_buffer_tail) || (i > rc)){
                //set to 1
                array[i] = 1; 
            }

        }
    }

}

uint64_t renamer::generate_squash_mask(uint64_t rc){
    /*
    only set 1 for checkpoints that will be squashed based on the following
    criteria:
        1. it's younger than the rc(rollback checkpoint)
        2. do not include the rollback checkpoint
        3. the youngest checkpoint is 1 behind tail
    */
    uint64_t mask = 0;
    for (uint64_t i = 0; i < num_checkpoints; i++){
        if (chkpt_buffer_head_phase == chkpt_buffer_tail_phase){
            if ((i > rc) && (i < chkpt_buffer_tail)){
                //set to 1
                mask += pow(2, i);
            }
        }
        else {
            if ((i < chkpt_buffer_tail) || (i > rc)){
                //set to 1
                mask += pow(2, i);
            }

        }
    }

    return mask;
}

uint64_t renamer::rollback(uint64_t chkpt_id, bool next,
                           uint64_t &total_loads, uint64_t &total_stores, 
                           uint64_t &total_branches){
    uint64_t squash_mask;
    uint64_t rollback_chkpt;

    if (!next){
        rollback_chkpt = chkpt_id;
    }
    else {
        rollback_chkpt = (chkpt_id + 1) % num_checkpoints; 
    }

    //assert the rollback_chkpt is valid
    assert(is_chkpt_valid(rollback_chkpt));

    //restore the RMT from the rollback checkpoint
    for (uint64_t i=0; i < map_table_size; i++){
        rmt[i] = checkpoint_buffer[rollback_chkpt].rmt[i];
    }

    //restore the unmapped bits from the rollback checkpoint
    uint64_t unmapped_bit;
    for (uint64_t j=0; j < num_phys_reg; j++){
        unmapped_bit = checkpoint_buffer[rollback_chkpt].unmapped_bits[j];
        if (unmapped_bit == 1) this->unmap(j);
        else this->map(j);
    }

    //generate squash mask
    squash_mask = generate_squash_mask(rollback_chkpt);

    //update the prf_usage_counter for phys reg. that are checkpointed in the
    //to-be-squashed checkpointed
    uint64_t *squash_mask_array;
    squash_mask_array = new uint64_t[num_checkpoints];


    for (uint64_t j=0; j < num_checkpoints; j++)squash_mask_array[j] = 0;
    generate_squash_mask_array(squash_mask_array, rollback_chkpt);

    for (uint64_t j=0; j < num_checkpoints; j++){
        if (squash_mask_array[j] == 1){
            for (uint64_t k=0; k < map_table_size; k++){
                //TODO: verify we need to decreament
                dec_usage_counter(checkpoint_buffer[j].rmt[k]);
                //FIXME: do we need to unmap these physical registers?
            }
        } else{
            if (j != rollback_chkpt){
                total_loads += checkpoint_buffer[j].load_counter;
                total_stores += checkpoint_buffer[j].store_counter;
                total_branches += checkpoint_buffer[j].branch_counter;
            }
        }
    }

    //start executing from this point
    checkpoint_buffer[rollback_chkpt].load_counter = 0; 
    checkpoint_buffer[rollback_chkpt].store_counter = 0; 
    checkpoint_buffer[rollback_chkpt].branch_counter = 0; 
    checkpoint_buffer[rollback_chkpt].uncompleted_instruction_counter = 0; 
    checkpoint_buffer[rollback_chkpt].amo = false; 
    checkpoint_buffer[rollback_chkpt].csr = false; 
    checkpoint_buffer[rollback_chkpt].exception = false; 

    return squash_mask;
}

bool renamer::is_chkpt_valid(uint64_t chkpt_id){
    /* return true for all the valid checkpoints, INCLUDING the head*/
    if (checkpoint_buffer_is_full()) return true;
    if (checkpoint_buffer_is_empty()) return false;

    if (chkpt_buffer_tail_phase == chkpt_buffer_head_phase){
        assert(chkpt_buffer_tail > chkpt_buffer_head);
        if ((chkpt_id >= chkpt_buffer_head) && (chkpt_buffer_tail > chkpt_id)){
            return true;
        }
    }
    else {
        assert(chkpt_buffer_tail < chkpt_buffer_head);
        if ((chkpt_id >= chkpt_buffer_head) || (chkpt_buffer_tail < chkpt_id)){
            return true; 
        }
    }

}

void renamer::squash(){
    //the renamer should be rolled back to the committed state of the machine

    //restore the RMT
    uint64_t i;
    for (i = 0; i < map_table_size; i++){
        rmt[i] = checkpoint_buffer[chkpt_buffer_head].rmt[i];
    }

    //restore unmapped bit from the oldest checkpoint
    uint64_t unmapped_bit;
    for (i = 0; i < num_phys_reg; i++){
        unmapped_bit = checkpoint_buffer[chkpt_buffer_head].unmapped_bits[i];
        //free list restoring via aggressive register reclamation
        if (unmapped_bit == 1) this->unmap(i);
        else this->map(i);
        prf_unmapped[i] = unmapped_bit;
    }

    //reinitialize free list    

    //keep the head, remove everything before tail?
    uint64_t to_squash[num_checkpoints];
    for (int j=0; j < num_checkpoints; j++){
        to_squash[j] = 0;
        if (is_chkpt_valid(j)){
            to_squash[j] = 1;
        }
        //excluding head bc it is the oldest checkpoint
        if (j == chkpt_buffer_head){
            to_squash[j] = 0;
        }
    }

    for (int j=0 ; j < num_checkpoints; j++){
        if (to_squash[j] == 1) {//squash
           for (int k=0; k < map_table_size; k++){
                dec_usage_counter(checkpoint_buffer[j].rmt[k]); 
            } 
        }
    }

    return;
}


//Debugging helper functions
/*
void renamer::print_free_list(){
    uint64_t i=0;
    printf("--------------FREE LIST-------------------\n");
    while (i < free_list_size){
        printf("| %3llu ", fl.list[i]);
        i++;
    }
    printf("|\n");
    printf("------------END FREE LIST-----------------\n");
    printf("FL: tail: %d, tail_phase:%d, head: %d, head_phase: %d\n", 
            fl.tail, fl.tail_phase, fl.head, fl.head_phase);
}
void renamer::print_amt(){
    printf("---------------------AMT-----------------\n");
    for (int i=0; i < this->map_table_size; i++){
        printf("| %3d ", amt[i]);
    }
    printf("\n-------------------END_AMT-----------------\n");
}
void renamer::print_rmt(){
    printf("---------------------RMT-----------------\n");
    for (int i=0; i < this->map_table_size; i++){
        printf("| %3d ", rmt[i]);
    }
    printf("\n-------------------END_RMT-----------------\n");

}
void renamer::print_prf(){
    printf("---------------------PRF-----------------\n");
    for (int i=0; i < this->num_phys_reg; i++){
        printf("| %8llu ", prf[i]);
    }
    printf("\n-------------------END_PRF-----------------\n");

}
void renamer::print_prf_ready(){
    printf("---------------------PRF_READY-----------------\n");
    for (int i=0; i < this->num_phys_reg; i++){
        printf("| %llu ", prf_ready[i]);
    }
    printf("\n-------------------END_PRF_READY-----------------\n");

}
void renamer::print_active_list(bool between_head_and_tail){
    int i=0, n=active_list_size;
    if (between_head_and_tail) {
        i = this->al.head;
        n = this->al.tail;
    }
    if (n - i <= 0){
        i = 0;
        n = active_list_size;
        printf("ACTIVE LIST IS FULL. PRINTING ALL\n");
    }

    printf("| idx | log| phys | com | exc | dest | PC | _ret |\n");
    for (i; i < n; i++){
        al_entry_t *t;
        t = &this->al.list[i];
        printf("| %3d | %3d | %3d | %3d | %3d | %3d| %llu |\n",
                i,     t->logical, t->physical, t->completed,
                t->exception, t->has_dest, t->pc);
    }
    printf("AL Head: %d, AL tail: %d, Head Phase: %d, Tail Phase: %d\n",
            this->al.head, this->al.tail, this->al.head_phase, this->al.tail_phase
        );

}
*/

bool renamer::reg_in_rmt(uint64_t phys_reg){
    uint64_t i;
    for (i=0; i < this->map_table_size; i++){
        if (this->rmt[i] == phys_reg) return true;    
    }
    return false;
}
