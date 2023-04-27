#define __STDC_FORMAT_MACROS

#include "renamer.h"
#include <cassert>
#include <cmath>
#include <iostream>
#include <bitset>

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
        rmt[j] = j;
    }

    //initializing the physical register files to be consistent with
    //an empty pipeline state
    for (j=0; j < n_phys_regs; j++){
        prf_ready[j] = 1;
        prf_usage_counter[j] = 0;
        prf_unmapped[j] = 1;
    }
    

    //checkpoint buffer
    chkpt_buffer_head = 0;
    chkpt_buffer_tail = 0;
    chkpt_buffer_head_phase = 0;
    chkpt_buffer_tail_phase = 0;
    num_checkpoints = n_chkpts;

    checkpoint_buffer = new chkpt[num_checkpoints];
    uint64_t i;
    for (i = 0; i < num_checkpoints; i++){
        checkpoint_buffer[i].rmt = new uint64_t[n_log_regs];
        checkpoint_buffer[i].unmapped_bits = new uint64_t[n_phys_regs];
        checkpoint_buffer[i].load_counter = 0;
        checkpoint_buffer[i].store_counter = 0;
        checkpoint_buffer[i].branch_counter = 0;
        checkpoint_buffer[i].uncompleted_instruction_counter = 0;
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
        map(rmt[k]);
        inc_usage_counter(rmt[k]);
    }

    for (j=0; j < n_phys_regs; j++){
        checkpoint_buffer[chkpt_buffer_head].unmapped_bits[j] = prf_unmapped[j];
    }

    //move the checkpoint tail ahead
    chkpt_buffer_tail++;

    //free list; free_list_size = prf - n_log_regs (721ss-prf-2 slide, p19)
    free_list_size = n_phys_regs - n_log_regs;
    _fl_count = free_list_size;
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
    if (checkpoint_buffer_is_empty()) return this->num_checkpoints;
     
    int used, free;
    if (this->chkpt_buffer_head_phase == this->chkpt_buffer_tail_phase){
        assert((this->chkpt_buffer_tail > this->chkpt_buffer_head));
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

    checkpoint_buffer[i].load_counter = 0;
    checkpoint_buffer[i].store_counter = 0;
    checkpoint_buffer[i].branch_counter = 0;
    checkpoint_buffer[i].uncompleted_instruction_counter = 0;
    checkpoint_buffer[i].amo = false;
    checkpoint_buffer[i].csr = false;
    checkpoint_buffer[i].exception = false;

    assert_checkpoint_buffer_invariance();
}

void renamer::free_checkpoint(){
    //make sure there is at least one more checkpoint after the head
    assert((this->num_checkpoints - this->get_free_checkpoint_count())> 1);

    reset_checkpoint(this->chkpt_buffer_head);
    //now move the head ahead
    //printf("release:    %d\n", this->chkpt_buffer_head);

    this->chkpt_buffer_head++;

    if (this->chkpt_buffer_head == this->num_checkpoints){
        this->chkpt_buffer_head = 0;
        this->chkpt_buffer_head_phase = !this->chkpt_buffer_head_phase;
    }

    assert_checkpoint_buffer_invariance();
}

bool renamer::stall_checkpoint(uint64_t bundle_chkpt){
    //Get the number of available checkpoints
    if (bundle_chkpt > this->get_free_checkpoint_count()){
        return true;
    }

    return false;
}

void renamer::set_exception(uint64_t checkpoint_ID){
    this->checkpoint_buffer[checkpoint_ID].exception = true;
}

uint64_t renamer::get_checkpoint_ID(bool load, bool store, bool branch, bool amo, bool csr){
    //return the nearest prior checkpoint
    uint64_t checkpoint_ID;
    if (this->chkpt_buffer_tail == 0){
        checkpoint_ID = this->num_checkpoints - 1;
    } else {
        checkpoint_ID = this->chkpt_buffer_tail - 1;
    }

    //increament other counters, set the flags etc
    if (load == true){this->checkpoint_buffer[checkpoint_ID].load_counter++;}
    if (store == true){this->checkpoint_buffer[checkpoint_ID].store_counter++;}
    if (branch == true){this->checkpoint_buffer[checkpoint_ID].branch_counter++;}

    
    this->checkpoint_buffer[checkpoint_ID].amo = amo;
    this->checkpoint_buffer[checkpoint_ID].csr = csr;

    //unconditionally increamenting
    this->checkpoint_buffer[checkpoint_ID].uncompleted_instruction_counter++;

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
        (this->fl.head_phase != this->fl.tail_phase)){
        //printf("free list is FULL!\n");

        //checking if none of them in the RMT
        //print_rmt();
        /*
        for (uint64_t i=0; i < free_list_size; i++){
            if (reg_in_rmt(fl.list[i])){
                printf("%d is in free list AND in RMT\n", fl.list[i]);
            }
        }
        */

        return true;
    }

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
    if (in_free_list(phys_reg)){
        printf("renamer::push_free_list() - BAD push. %d is already in FL.\n", phys_reg);
    }

    this->fl.list[this->fl.tail] = phys_reg;
    this->fl.tail++;
    if (this->fl.tail == this->free_list_size){
        this->fl.tail = 0;
        this->fl.tail_phase = !this->fl.tail_phase;
    }

    //printf("renamer::push_free_list(): %d to the free list\n", phys_reg);

    _fl_count += 1;
    //print_free_list();
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
    this->inc_usage_counter(result);

    this->unmap(old);

    return result;
}


void renamer::checkpoint(){
    assert(!this->checkpoint_buffer_is_full());
    uint64_t x = this->chkpt_buffer_tail;
    uint64_t i;
    for(i = 0; i < map_table_size; i++){
        this->checkpoint_buffer[x].rmt[i] = rmt[i];
        inc_usage_counter(rmt[i]);
    }

    this->checkpoint_buffer[x].uncompleted_instruction_counter = 0;
    this->checkpoint_buffer[x].load_counter = 0;
    this->checkpoint_buffer[x].store_counter = 0;
    this->checkpoint_buffer[x].branch_counter = 0;
    this->checkpoint_buffer[x].amo = false;
    this->checkpoint_buffer[x].csr = false;
    this->checkpoint_buffer[x].exception = false;

    uint64_t j;
    for (j=0; j < num_phys_reg; j++){
        this->checkpoint_buffer[x].unmapped_bits[j] = prf_unmapped[j];
    }

    //printf("checkpoint: %d\n", x);

    //advancing the tail 
    this->chkpt_buffer_tail++;
    if (this->chkpt_buffer_tail == this->num_checkpoints){
        this->chkpt_buffer_tail = 0;
        this->chkpt_buffer_tail_phase = !this->chkpt_buffer_tail_phase;
    }
}

void renamer::map(uint64_t phys_reg){
    // Clear the unmapped bit of physical register
    this->prf_unmapped[phys_reg] = 0;
}

void renamer::unmap(uint64_t phys_reg){
    // Set the unmapped bit of physical register
    // Check if phys_reg’s usage counter is 0; if so,
    // push phys_reg onto the Free List.
    this->prf_unmapped[phys_reg] = 1;
    if ((this->prf_unmapped[phys_reg] == 1) && (this->prf_usage_counter[phys_reg] == 0)){
        //push it onto the free list:
        bool result = this->push_free_list(phys_reg);
        if (!result){
            printf("Could not push to free list since it's full\n");
            assert(0);
            //exit(EXIT_FAILURE);
        }
    }
}

void renamer::inc_usage_counter(uint64_t phys_reg){
    //printf("inc_usage_counter: P%lu free list size: %d\n", phys_reg, free_list_regs_available());
    this->prf_usage_counter[phys_reg] += 1;
}

void renamer::dec_usage_counter(uint64_t phys_reg){
    //printf("dec_usage_counter: P%lu free list size: %d\n", phys_reg, free_list_regs_available());
    assert(this->prf_usage_counter[phys_reg] > 0);
    this->prf_usage_counter[phys_reg] -= 1;

    if (this->prf_unmapped[phys_reg] == 1){
        this->unmap(phys_reg);
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
    //printf("instruction for chkpt_id %d has completed\n", checkpoint_ID);
    assert(this->checkpoint_buffer[checkpoint_ID].uncompleted_instruction_counter > 0);
    this->checkpoint_buffer[checkpoint_ID].uncompleted_instruction_counter--;
    if (this->checkpoint_buffer[checkpoint_ID].uncompleted_instruction_counter < 0){
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

    chkpt_id = this->chkpt_buffer_head;

    num_loads = this->checkpoint_buffer[chkpt_id].load_counter;
    num_stores = this->checkpoint_buffer[chkpt_id].store_counter;
    num_branches = this->checkpoint_buffer[chkpt_id].branch_counter;

    amo = this->checkpoint_buffer[chkpt_id].amo;
    csr = this->checkpoint_buffer[chkpt_id].csr;
    exception = this->checkpoint_buffer[chkpt_id].exception;


    if ((this->checkpoint_buffer[chkpt_id].uncompleted_instruction_counter == 0) &&
        ((this->is_chkpt_valid((this->chkpt_buffer_head + 1) % this->num_checkpoints))|| 
          (this->checkpoint_buffer[chkpt_id].exception)
        )
       ){
        return true;
    }

    return false;
}

void renamer::commit(uint64_t log_reg){
    //NOTE: double-check which RMT to use, might be a source of error
    uint64_t phys_reg = this->checkpoint_buffer[this->chkpt_buffer_head].rmt[log_reg];
    this->dec_usage_counter(phys_reg);
}

void renamer::generate_squash_mask_array(uint64_t *array, uint64_t rc){
    uint64_t i = rc;
    while (i != this->chkpt_buffer_tail){
        array[i] = 1; 
        i = (i+1) % this->num_checkpoints;
        if (i == this->chkpt_buffer_tail) break;
    }

    array[rc] = 0;

}

uint64_t renamer::generate_squash_mask(uint64_t rc){
    uint64_t i = rc;
    uint64_t mask = 0;

    while (i != this->chkpt_buffer_tail){
        mask += pow(2, i);
        i = (i+1) % this->num_checkpoints;
        if (i == this->chkpt_buffer_tail) break;
    }

    /*
    printf("rc: %d, head: %d(%d), tail: %d(%d), num: %d\n", 
        rc, chkpt_buffer_head, chkpt_buffer_head_phase,
        chkpt_buffer_tail, chkpt_buffer_tail_phase,
        num_checkpoints
    );
    std::cout<<std::bitset<8>(mask)<<std::endl;
    */
    
    return mask;
}

/*
uint64_t renamer::generate_squash_mask(uint64_t rc){
    uint64_t i = rc;
    uint64_t mask = 0;

    while (i != chkpt_buffer_tail){
        mask += pow(2, i);
        i = (i+1) % num_checkpoints;
        if (i == chkpt_buffer_tail) break;
    }

    return mask;
}
*/

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
        //assert(this->checkpoint_buffer[this->chkpt_buffer_head].uncompleted_instruction_counter > 0);
    }

    assert(is_chkpt_valid(rollback_chkpt));

    //restore the RMT from the rollback checkpoint
    for (uint64_t i=0; i < map_table_size; i++){
        this->rmt[i] = this->checkpoint_buffer[rollback_chkpt].rmt[i];
    }

    //restore the unmapped bits from the rollback checkpoint
    uint64_t checkpointed_bit, current_bit;
    for (uint64_t j=0; j < this->num_phys_reg; j++){
        checkpointed_bit = this->checkpoint_buffer[rollback_chkpt].unmapped_bits[j];
        current_bit = this->prf_unmapped[j];
        if (current_bit != checkpointed_bit){
            if ((current_bit == 0) && (checkpointed_bit == 1)){
                this->unmap(j);
            } 
            else if ((current_bit == 1) && (checkpointed_bit == 0)){
                this->map(j);
            }
        }
    }

    //generate squash mask
    /*
    if (next){printf("rollback\n");}
    else{printf("squash\n");}
    */
    squash_mask = generate_squash_mask(rollback_chkpt);

    uint64_t *squash_these_checkpoints_for_rollback;
    squash_these_checkpoints_for_rollback = new uint64_t[this->num_checkpoints];


    for (uint64_t j=0; j < num_checkpoints; j++){
        squash_these_checkpoints_for_rollback[j] = 0;
    }

    generate_squash_mask_array(squash_these_checkpoints_for_rollback, rollback_chkpt);

/*
    total_loads = 0;
    total_stores = 0;
    total_branches = 0;
*/
    for (uint64_t j=0; j < this->num_checkpoints; j++){
        if (squash_these_checkpoints_for_rollback[j] == 1){
            assert(this->is_chkpt_valid(j));
            for (uint64_t k=0; k < this->map_table_size; k++){
                dec_usage_counter(this->checkpoint_buffer[j].rmt[k]);
            }
        }
    }

    reset_checkpoint(rollback_chkpt);

    //set the tail right after the rollback checkpoint 
    uint64_t new_tail = (rollback_chkpt + 1) % this->num_checkpoints;

    while (this->chkpt_buffer_tail != new_tail){
        //printf("tail afer decreamenting: %d\n", this->chkpt_buffer_tail);
        this->chkpt_buffer_tail = (this->chkpt_buffer_tail - 1) % this->num_checkpoints; 

        if (this->chkpt_buffer_tail == (num_checkpoints - 1)){
            this->chkpt_buffer_tail_phase = !this->chkpt_buffer_tail_phase;
        }

        if (this->chkpt_buffer_tail == new_tail) break;
    }
    
    if (this->chkpt_buffer_head != this->chkpt_buffer_tail){
        reset_checkpoint(this->chkpt_buffer_tail); //make sure it is empty
    }

    return squash_mask;
}

void renamer::assert_checkpoint_buffer_invariance(){
    if (this->checkpoint_buffer_is_full()){
        //it can be full. head == tail and hp != tp
        return;
    }

    if (this->chkpt_buffer_tail_phase == this->chkpt_buffer_head_phase){
        assert(this->chkpt_buffer_tail > this->chkpt_buffer_head);
    }
    else {
        assert(this->chkpt_buffer_tail < this->chkpt_buffer_head);
    }

}


bool renamer::is_chkpt_valid(uint64_t chkpt_id){
    if (this->checkpoint_buffer_is_full()){
        return true;
    }
    if (this->checkpoint_buffer_is_empty()){
        return false;
    }

    if (this->chkpt_buffer_tail_phase == this->chkpt_buffer_head_phase){
        assert(this->chkpt_buffer_tail > this->chkpt_buffer_head);
        if ((chkpt_id >= this->chkpt_buffer_head) && (this->chkpt_buffer_tail > chkpt_id)){
            return true;
        } else {
            return false;    
        }
    }
    else {
        assert(this->chkpt_buffer_tail < this->chkpt_buffer_head);
        if ((chkpt_id >= this->chkpt_buffer_head) || (this->chkpt_buffer_tail > chkpt_id)){
            return true; 
        } else {
            return false;
        }
    }

    printf("end of is_chkpt_valid function, should not come here\n");
    exit(EXIT_FAILURE);
}

void renamer::squash(){
    //the renamer should be rolled back to the committed state of the machine
    //printf("renamer::squash() initiating a complete squash\n");
    uint64_t total_loads, total_stores, total_branches;
    //FIXME: this seems like a ghetto solution but lets see

    this->rollback(this->chkpt_buffer_head,
                   false, total_loads,
                   total_stores, total_branches
    );

    this->reset_checkpoint(chkpt_buffer_head);
    this->reset_checkpoint(chkpt_buffer_tail);
    return;

    /*
    //restore the RMT
    uint64_t i;
    for (i = 0; i < map_table_size; i++){
        rmt[i] = checkpoint_buffer[chkpt_buffer_head].rmt[i];
    }

    //restore the unmapped bits from the rollback checkpoint
    uint64_t checkpointed_bit, current_bit;
    for (uint64_t j=0; j < num_phys_reg; j++){
        checkpointed_bit = checkpoint_buffer[chkpt_buffer_head].unmapped_bits[j];
        current_bit = prf_unmapped[j];
        if (current_bit != checkpointed_bit){
            if ((current_bit == 0) && (checkpointed_bit == 1)){
                unmap(j);
            } 
            else if ((current_bit == 1) && (checkpointed_bit == 0)){
                map(j);
            }
        }
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
           reset_checkpoint(j);
        }
    }

    //set new tail, right after the head
    chkpt_buffer_tail = chkpt_buffer_head + 1;
    chkpt_buffer_tail_phase = chkpt_buffer_head_phase;
    if (chkpt_buffer_tail == num_checkpoints){
        //set it to 0
        chkpt_buffer_tail = 0;
        chkpt_buffer_tail_phase = !chkpt_buffer_tail_phase;
    }

    reset_checkpoint(chkpt_buffer_tail);

    return;
    */
}


//Debugging helper functions
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

/*
void renamer::print_prf_unampped(){
    printf("---------------------PRF UNMAPPED-----------------\n");
    for (int i=0; i < this->num_phys_reg; i++){
        printf("| %8llu ", prf_unmapped[i]);
    }
    printf("\n-------------------END_PRF_UNMAPPED-----------------\n");
}
*/

void renamer::print_prf_usage(){
    printf("---------------------PRF_USAGE-----------------\n");
    for (int i=0; i < this->num_phys_reg; i++){
        printf("| %8llu ", prf_usage_counter[i]);
    }
    printf("\n-------------------END_PRF_USAGE-----------------\n");

}
/*
void renamer::print_amt(){
    printf("---------------------AMT-----------------\n");
    for (int i=0; i < this->map_table_size; i++){
        printf("| %3d ", amt[i]);
    }
    printf("\n-------------------END_AMT-----------------\n");
}
*/
void renamer::print_rmt(){
    printf("---------------------RMT-----------------\n");
    for (int i=0; i < this->map_table_size; i++){
        printf("| %3d ", rmt[i]);
    }
    printf("\n-------------------END_RMT-----------------\n");

}
/*
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

uint64_t renamer::get_mapped_count(){
    uint64_t mapped = this->num_phys_reg - this->get_unmapped_count();
    assert(mapped >= 0);

    return mapped;
}

uint64_t renamer::get_unmapped_count(){
    uint64_t i, count=0;
    for (i=0; i < num_phys_reg; i++){
        if (this->prf_unmapped[i] == 1){
            count++;
        }
    }

    assert (count >= 0);
    return count;
}


bool renamer::in_free_list(uint64_t phys_reg){
    uint64_t i;
    for (i=0; i < this->free_list_size; i++){
        if (this->fl.list[i] == phys_reg){
            //check validity
            if (fl.head_phase == fl.tail_phase){
                if ((i >= fl.head) && (i < fl.tail)){return true;}
                return false;
            } else {
                if ((i < fl.tail) || (i >= fl.head)){return true;}
                else {return false;}
            } 
        }
    }

    return false;
}

bool renamer::reg_in_rmt(uint64_t phys_reg){
    uint64_t i;
    for (i=0; i < this->map_table_size; i++){
        if (this->rmt[i] == phys_reg) return true;    
    }
    return false;
}
